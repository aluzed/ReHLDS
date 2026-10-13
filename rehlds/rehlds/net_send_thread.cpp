#include "precompiled.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <thread>

CNetSendThread g_NetSendThread;

namespace
{
	const int DEFAULT_QUEUE_SIZE = 1024;
	const int MIN_QUEUE_SIZE = 64;
	const int MAX_QUEUE_SIZE = 16384;

	// Packets handed to one sendmmsg call
	const int MAX_BATCH = 64;

	// Spin before sleeping once the queue is empty: the packets of one tick are built
	// a few microseconds apart, a wake-up for each would cost the main thread a system
	// call per packet
	const uint64 SPIN_NS = 5000;

	// Upper bound of the thread's sleep, in case a wake-up is lost
	const int WAIT_TIMEOUT_MS = 50;

	// The main thread waits this long for room in a full queue, then sends itself
	const uint64 FULL_WAIT_NS = 100000000;

	void SetCurrentThreadName(const char *name)
	{
#ifndef _WIN32
		pthread_setname_np(pthread_self(), name);
#endif
	}

	// Errors NET_SendPacket does not report
	bool IsSilentError(int err)
	{
		return err == WSAEWOULDBLOCK || err == WSAECONNREFUSED || err == WSAECONNRESET;
	}

	void Count(std::atomic<uint32> &counter, uint32 n = 1)
	{
		// Single writer: no need for an atomic read-modify-write
		counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
	}

	void Cmd_NetSendStats()
	{
		g_NetSendThread.Cmd_Stats();
	}
}

CNetSendThread::CNetSendThread() :
	m_bEnabled(false),
	m_bRunning(false),
	m_QueueSize(DEFAULT_QUEUE_SIZE),
	m_ReportedErrors(0),
#ifdef _WIN32
	m_hThread(nullptr),
	m_hWakeEvent(nullptr),
#else
	m_WakeFd(-1),
#endif
	m_Socket(INVALID_SOCKET),
	m_bSleeping(false),
	m_bStopRequested(false),
	m_Sent(0),
	m_SendCalls(0),
	m_Errors(0),
	m_LastError(0),
	m_LargestBatch(0),
	m_Queued(0),
	m_Wakes(0),
	m_QueueFullWaits(0),
	m_SentByMainThread(0)
{
	// Statistics are relaxed atomics, read by the main thread for display only
	RACE_BENIGN(&m_Sent, sizeof(m_Sent), "statistics");
	RACE_BENIGN(&m_SendCalls, sizeof(m_SendCalls), "statistics");
	RACE_BENIGN(&m_LargestBatch, sizeof(m_LargestBatch), "statistics");
	RACE_BENIGN(&m_Errors, sizeof(m_Errors), "statistics");
	RACE_BENIGN(&m_LastError, sizeof(m_LastError), "statistics");

	// Flags of the wake-up handshake (see Send and Run)
	RACE_BENIGN(&m_bSleeping, sizeof(m_bSleeping), "flag");
	RACE_BENIGN(&m_bStopRequested, sizeof(m_bStopRequested), "flag");
}

#ifdef _WIN32
DWORD WINAPI CNetSendThread::ThreadEntry(LPVOID param)
{
	((CNetSendThread *)param)->Run();
	return 0;
}
#else
void *CNetSendThread::ThreadEntry(void *param)
{
	((CNetSendThread *)param)->Run();
	return nullptr;
}
#endif

void CNetSendThread::Init()
{
	Cmd_AddCommand("net_send_stats", Cmd_NetSendStats);

	// On by default; -netsend is still accepted
	if (COM_CheckParm("-nonetsend"))
		return;

	// Nothing is sent during a replay
	if (g_NetReplay.IsReplaying())
		return;

#ifdef _WIN32
	// The testsuite (Windows only) records and replays socket calls in order:
	// keep them on the main thread
	if (g_RehldsRuntimeConfig.testPlayerMode != TPM_DISABLE)
	{
		Con_Printf("Network send thread disabled by the testsuite\n");
		return;
	}
#endif

	int queueParm = COM_CheckParm("-netsend_queue");
	if (queueParm && queueParm + 1 < com_argc)
	{
		m_QueueSize = Q_atoi(com_argv[queueParm + 1]);
		if (m_QueueSize < MIN_QUEUE_SIZE)
			m_QueueSize = MIN_QUEUE_SIZE;
		else if (m_QueueSize > MAX_QUEUE_SIZE)
			m_QueueSize = MAX_QUEUE_SIZE;
	}

	m_bEnabled = true;
}

void CNetSendThread::Start(SOCKET serverSocket)
{
	if (!m_bEnabled || m_bRunning || serverSocket == INVALID_SOCKET)
		return;

	if (!m_Queue.Capacity() && !m_Queue.Allocate(m_QueueSize))
	{
		Con_Printf("WARNING: network send thread: cannot allocate the queue, sending packets on the main thread\n");
		m_bEnabled = false;
		return;
	}

#ifdef _WIN32
	if (!m_hWakeEvent)
		m_hWakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_hWakeEvent)
#else
	if (m_WakeFd < 0)
		m_WakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (m_WakeFd < 0)
#endif
	{
		Con_Printf("WARNING: network send thread: cannot create the wake handle, sending packets on the main thread\n");
		m_bEnabled = false;
		return;
	}

	m_Queue.Clear();
	m_Socket = serverSocket;
	m_bSleeping.store(false);
	m_bStopRequested.store(false);

#ifdef _WIN32
	m_hThread = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
	bool created = m_hThread != nullptr;
#else
	bool created = pthread_create(&m_Thread, nullptr, ThreadEntry, this) == 0;
#endif

	if (!created)
	{
		Con_Printf("WARNING: network send thread: cannot create the thread, sending packets on the main thread\n");
		m_bEnabled = false;
		return;
	}

	m_bRunning = true;
	Con_DPrintf("Network send thread started, queue of %u packets\n", (unsigned int)m_Queue.Capacity());
}

void CNetSendThread::Stop()
{
	if (!m_bRunning)
		return;

	// The thread sends what is queued, then exits
	m_bStopRequested.store(true);
	Wake();

#ifdef _WIN32
	WaitForSingleObject(m_hThread, INFINITE);
	CloseHandle(m_hThread);
	m_hThread = nullptr;
#else
	pthread_join(m_Thread, nullptr);
#endif

	m_Queue.Clear();
	m_Socket = INVALID_SOCKET;
	m_bRunning = false;
	ReportErrors();
}

bool CNetSendThread::Send(SOCKET s, const void *data, int size, const struct sockaddr *to, int tolen)
{
	if (!m_bRunning || s != m_Socket || size <= 0 || size > MAX_ROUTEABLE_PACKET
		|| tolen > (int)sizeof(Packet::to) || !IsMainThread())
		return false;

	Packet *packet = m_Queue.BeginPush();
	if (!packet)
	{
		// The thread is behind (or asleep while a wake-up was lost): wait for room
		// rather than reorder the packets of a destination
		Count(m_QueueFullWaits);
		Wake();

		uint64 deadline = Sys_MonotonicNs() + FULL_WAIT_NS;
		while ((packet = m_Queue.BeginPush()) == nullptr)
		{
			if (Sys_MonotonicNs() > deadline)
			{
				Count(m_SentByMainThread);
				return false;
			}

			std::this_thread::yield();
		}
	}

	Q_memcpy(&packet->to, to, tolen);
	packet->tolen = tolen;
	packet->size = size;
	Q_memcpy(packet->data, data, size);
	m_Queue.CommitPush();
	Count(m_Queued);

	// Pairs with the fence in Run: either the thread sees the packet before it
	// sleeps, or the main thread sees that it sleeps and wakes it
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (m_bSleeping.load(std::memory_order_relaxed))
	{
		m_bSleeping.store(false, std::memory_order_relaxed);
		Count(m_Wakes);
		Wake();
	}

	return true;
}

void CNetSendThread::Wake()
{
#ifdef _WIN32
	if (m_hWakeEvent)
		SetEvent(m_hWakeEvent);
#else
	if (m_WakeFd >= 0)
	{
		uint64 one = 1;
		if (write(m_WakeFd, &one, sizeof(one)) < 0)
		{
			// EAGAIN: the counter is already set, the thread wakes anyway
		}
	}
#endif
}

void CNetSendThread::Sleep()
{
#ifdef _WIN32
	WaitForSingleObject(m_hWakeEvent, WAIT_TIMEOUT_MS);
#else
	struct pollfd fd;
	fd.fd = m_WakeFd;
	fd.events = POLLIN;
	fd.revents = 0;

	if (poll(&fd, 1, WAIT_TIMEOUT_MS) > 0)
	{
		uint64 value;
		if (read(m_WakeFd, &value, sizeof(value)) < 0)
		{
			// EAGAIN: already reset, nothing to do
		}
	}
#endif
}

void CNetSendThread::Run()
{
	SetCurrentThreadName("rehlds-send");

	for (;;)
	{
		if (SendQueued())
			continue;

		// Stop requested and nothing left: the main thread queues nothing more
		if (m_bStopRequested.load())
		{
			if (!m_Queue.Front())
				break;
			continue;
		}

		uint64 spinEnd = Sys_MonotonicNs() + SPIN_NS;
		bool queued = false;
		while (Sys_MonotonicNs() < spinEnd)
		{
			if (m_Queue.Front())
			{
				queued = true;
				break;
			}
			_mm_pause();
		}

		if (queued)
			continue;

		m_bSleeping.store(true, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (!m_Queue.Front() && !m_bStopRequested.load())
			Sleep();
		m_bSleeping.store(false, std::memory_order_relaxed);
	}
}

void CNetSendThread::CountError(int err)
{
	if (IsSilentError(err))
		return;

	m_LastError.store(err, std::memory_order_relaxed);
	Count(m_Errors);
}

int CNetSendThread::SendQueued()
{
	int count = 0;

#ifdef _WIN32
	while (count < MAX_BATCH)
	{
		Packet *packet = m_Queue.Peek(count);
		if (!packet)
			break;

		Count(m_SendCalls);
		if (sendto(m_Socket, (const char *)packet->data, packet->size, 0, &packet->to, packet->tolen) == SOCKET_ERROR)
			CountError(WSAGetLastError());
		count++;
	}
#else
	struct mmsghdr messages[MAX_BATCH];
	struct iovec buffers[MAX_BATCH];

	while (count < MAX_BATCH)
	{
		Packet *packet = m_Queue.Peek(count);
		if (!packet)
			break;

		buffers[count].iov_base = packet->data;
		buffers[count].iov_len = packet->size;

		Q_memset(&messages[count], 0, sizeof(messages[count]));
		messages[count].msg_hdr.msg_name = &packet->to;
		messages[count].msg_hdr.msg_namelen = packet->tolen;
		messages[count].msg_hdr.msg_iov = &buffers[count];
		messages[count].msg_hdr.msg_iovlen = 1;
		count++;
	}

	// sendmmsg stops at the first packet it cannot send: report it and go on with
	// the next, like one sendto per packet would
	int done = 0;
	while (done < count)
	{
		Count(m_SendCalls);
		int sent = sendmmsg(m_Socket, &messages[done], count - done, 0);
		if (sent < 0)
		{
			int err = errno;
			if (err == EINTR)
				continue;

			CountError(err);
			done++;
		}
		else
			done += sent;
	}
#endif

	if (!count)
		return 0;

	m_Queue.Pop(count);
	Count(m_Sent, count);

	if ((uint32)count > m_LargestBatch.load(std::memory_order_relaxed))
		m_LargestBatch.store(count, std::memory_order_relaxed);

	return count;
}

void CNetSendThread::ReportErrors()
{
	uint32 errors = m_Errors.load(std::memory_order_relaxed);
	if (errors == m_ReportedErrors)
		return;

	Con_Printf("NET_SendPacket: ERROR: %s (%u packets)\n", NET_ErrorString(m_LastError.load(std::memory_order_relaxed)), errors - m_ReportedErrors);
	m_ReportedErrors = errors;
}

void CNetSendThread::Cmd_Stats()
{
	if (!m_bEnabled)
	{
		Con_Printf("Network send thread is off (-nonetsend, or a replay or the testsuite)\n");
		return;
	}

	uint32 sent = m_Sent.load(std::memory_order_relaxed);
	uint32 calls = m_SendCalls.load(std::memory_order_relaxed);

	Con_Printf("Network send thread: %s, queue %u/%u packets\n",
		m_bRunning ? "running" : "stopped",
		(unsigned int)m_Queue.Size(), (unsigned int)m_Queue.Capacity());
	Con_Printf("  queued        %u\n", m_Queued.load(std::memory_order_relaxed));
	Con_Printf("  sent          %u in %u system calls (%.1f per call), largest batch %u\n",
		sent, calls, calls ? (double)sent / calls : 0.0, m_LargestBatch.load(std::memory_order_relaxed));
	Con_Printf("  wake-ups      %u\n", m_Wakes.load(std::memory_order_relaxed));
	Con_Printf("  queue full    %u times, %u packets sent by the main thread after waiting\n",
		m_QueueFullWaits.load(std::memory_order_relaxed), m_SentByMainThread.load(std::memory_order_relaxed));
	Con_Printf("  send errors   %u\n", m_Errors.load(std::memory_order_relaxed));
}

void NetSendThread_Init()
{
	g_NetSendThread.Init();
}
