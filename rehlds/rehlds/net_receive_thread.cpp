#include "precompiled.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

CNetReceiveThread g_NetReceiveThread;

cvar_t net_thread_filter = { "net_thread_filter", "1", 0, 1.0f, nullptr };

namespace
{
	const int DEFAULT_QUEUE_SIZE = 1024;
	const int MIN_QUEUE_SIZE = 64;
	const int MAX_QUEUE_SIZE = 16384;

	// Upper bound of the thread's wait, so that Stop() never waits longer
	const int WAIT_TIMEOUT_MS = 50;

	// Packets read from one socket before checking the others and the stop flag
	const int MAX_PACKETS_PER_DRAIN = 1024;

	// "You have been banned" replies sent by the thread, per second
	const int MAX_BAN_REPLIES_PER_SEC = 20;

	const uint64 NS_PER_SEC = 1000000000ull;

	// Answers older than this are not used, the queries go to the main thread again:
	// nobody asked for them for a while, or the main thread no longer captures them
	// (Steam stopped). Asked for, they are captured again after one second.
	const uint64 MAX_REPLY_AGE_NS = 2 * NS_PER_SEC;

	// Split sequence numbers of the answers sent by the thread, far from those of
	// NET_SendLong (a counter from 1 for every split packet the server sends)
	const uint32 FIRST_SPLIT_SEQUENCE = 0x40000000;

	// A challenge is valid for the period it was given in and the next one
	const uint64 CHALLENGE_PERIOD_NS = 30 * NS_PER_SEC;

	const char *const s_DropReasonNames[NTD_COUNT] =
	{
		"queue full",
		"malformed",
		"rate limited",
		"banned",
		"unknown address",
	};

	const char BAN_MESSAGE[] = "You have been banned from this server.\n";

	int LastSocketError()
	{
#ifdef _WIN32
		return WSAGetLastError();
#else
		return errno;
#endif
	}

	bool IsWouldBlock(int err)
	{
#ifdef _WIN32
		return err == WSAEWOULDBLOCK;
#else
		return err == EWOULDBLOCK || err == EAGAIN;
#endif
	}

	// ICMP errors reported on the UDP socket, ignored like in NET_QueuePacket
	bool IsIgnoredError(int err)
	{
#ifdef _WIN32
		return err == WSAECONNRESET || err == WSAECONNREFUSED || err == WSAENETRESET || err == WSAEINTR;
#else
		return err == ECONNRESET || err == ECONNREFUSED || err == EINTR;
#endif
	}

	void SetCurrentThreadName(const char *name)
	{
#ifndef _WIN32
		pthread_setname_np(pthread_self(), name);
#endif
	}

	void Cmd_NetThreadStats()
	{
		g_NetReceiveThread.Cmd_Stats();
	}
}

bool CNetReceiveThread::FilterInputs::operator==(const FilterInputs &other) const
{
	return filterEnabled == other.filterEnabled
		&& rateLimitEnabled == other.rateLimitEnabled
		&& filterBan == other.filterBan
		&& maxQueriesSec == other.maxQueriesSec
		&& maxQueriesSecGlobal == other.maxQueriesSecGlobal
		&& maxChallengesSec == other.maxChallengesSec
		&& maxQueriesWindow == other.maxQueriesWindow
		&& filtersVersion == other.filtersVersion
		&& numFilters == other.numFilters
		&& preprocessHooked == other.preprocessHooked
		&& clientsVersion == other.clientsVersion;
}

CNetReceiveThread::CNetReceiveThread() :
	m_bEnabled(false),
	m_bRunning(false),
	m_QueueSize(DEFAULT_QUEUE_SIZE),
	m_PublishedInputs(),
	m_bConfigPublished(false),
	m_ReportedDrops(0),
	m_NumClients(0),
	m_ClientsVersion(0),
#ifdef _WIN32
	m_hThread(nullptr),
#endif
	m_bStopRequested(false),
	m_NumSockets(0),
	m_bMainWaiting(false),
#ifdef _WIN32
	m_hWakeEvent(nullptr),
#else
	m_WakeFd(-1),
#endif
	m_ConfigVersion(0),
	m_SeenConfigVersion(0),
	m_NextSplitSequence(FIRST_SPLIT_SEQUENCE),
	m_BanReplyWindowStart(0),
	m_BanRepliesInWindow(0),
	m_Received(0),
	m_BanReplies(0),
	m_RecvErrors(0),
	m_RecvCalls(0),
	m_MaxQueueDepth(0)
{
#ifndef _WIN32
	m_Batch = nullptr;
#endif
	for (int i = 0; i < NTD_COUNT; i++)
		m_Dropped[i].store(0, std::memory_order_relaxed);
	for (int i = 0; i < A2SQ_COUNT; i++)
	{
		m_Answered[i].store(0, std::memory_order_relaxed);
		m_QueryDemand[i].store(false, std::memory_order_relaxed);
	}
	m_ChallengesSent.store(0, std::memory_order_relaxed);
	m_ChallengesLimited.store(0, std::memory_order_relaxed);
	m_ChallengeKey[0] = m_ChallengeKey[1] = 0;

	// Statistics are relaxed atomics, read by the main thread for display only
	RACE_BENIGN(&m_Received, sizeof(m_Received), "statistics");
	RACE_BENIGN(m_Dropped, sizeof(m_Dropped), "statistics");
	RACE_BENIGN(&m_BanReplies, sizeof(m_BanReplies), "statistics");
	RACE_BENIGN(&m_RecvErrors, sizeof(m_RecvErrors), "statistics");
	RACE_BENIGN(&m_RecvCalls, sizeof(m_RecvCalls), "statistics");
	RACE_BENIGN(&m_MaxQueueDepth, sizeof(m_MaxQueueDepth), "statistics");
	RACE_BENIGN(m_Answered, sizeof(m_Answered), "statistics");
	RACE_BENIGN(&m_ChallengesSent, sizeof(m_ChallengesSent), "statistics");
	RACE_BENIGN(&m_ChallengesLimited, sizeof(m_ChallengesLimited), "statistics");

	// Hint for the main thread, which captures an answer when a query arrived
	RACE_BENIGN(m_QueryDemand, sizeof(m_QueryDemand), "flag");

	// Flags: what they guard is synchronised by the config mutex and by pthread_join
	RACE_BENIGN(&m_ConfigVersion, sizeof(m_ConfigVersion), "flag");
	RACE_BENIGN(&m_bStopRequested, sizeof(m_bStopRequested), "flag");
}

CNetReceiveThread::~CNetReceiveThread()
{
#ifdef _WIN32
	// Waiting for a thread while the loader lock is held (DLL unload) deadlocks:
	// NET_Shutdown has normally stopped it already.
	if (m_bRunning)
	{
		m_bStopRequested.store(true);
		CloseHandle(m_hThread);
		m_bRunning = false;
	}
#else
	Stop();
#endif
}

#ifdef _WIN32
DWORD WINAPI CNetReceiveThread::ThreadEntry(LPVOID param)
{
	((CNetReceiveThread *)param)->Run();
	return 0;
}
#else
void *CNetReceiveThread::ThreadEntry(void *param)
{
	((CNetReceiveThread *)param)->Run();
	return nullptr;
}
#endif

void CNetReceiveThread::Init()
{
	Cvar_RegisterVariable(&net_thread_filter);
	Cmd_AddCommand("net_thread_stats", Cmd_NetThreadStats);

	// On by default; -netthread is still accepted
	if (COM_CheckParm("-nonetthread"))
		return;

	// A replay reads its packets from the recording
	if (g_NetReplay.IsReplaying())
		return;

#ifdef _WIN32
	// The testsuite (Windows only) records and replays socket calls in order:
	// keep them on the main thread
	if (g_RehldsRuntimeConfig.testPlayerMode != TPM_DISABLE)
	{
		Con_Printf("Network thread disabled by the testsuite\n");
		return;
	}
#endif

	int queueParm = COM_CheckParm("-netthread_queue");
	if (queueParm && queueParm + 1 < com_argc)
	{
		m_QueueSize = Q_atoi(com_argv[queueParm + 1]);
		if (m_QueueSize < MIN_QUEUE_SIZE)
			m_QueueSize = MIN_QUEUE_SIZE;
		else if (m_QueueSize > MAX_QUEUE_SIZE)
			m_QueueSize = MAX_QUEUE_SIZE;
	}

#ifndef _WIN32
	// Allocated once, kept until exit like the thread
	if (COM_CheckParm("-netbatch"))
		m_Batch = (unsigned char (*)[MAX_UDP_PACKET])Mem_Malloc(RECV_BATCH * MAX_UDP_PACKET);
#endif

	m_bEnabled = true;
}

void CNetReceiveThread::Start(SOCKET serverSocket, SOCKET serverSocketIpx)
{
	if (!m_bEnabled || m_bRunning)
		return;

	m_NumSockets = 0;
	if (serverSocket != INVALID_SOCKET)
		m_Sockets[m_NumSockets++] = serverSocket;
	if (serverSocketIpx != INVALID_SOCKET)
		m_Sockets[m_NumSockets++] = serverSocketIpx;

	if (!m_NumSockets)
		return;

	if (!m_Queue.Capacity() && !m_Queue.Allocate(m_QueueSize))
	{
		Con_Printf("WARNING: network thread: cannot allocate the receive queue, reading packets on the main thread\n");
		m_bEnabled = false;
		return;
	}

	m_Queue.Clear();
	m_Config.reset();
	if (!m_ChallengeKey[0] && !m_ChallengeKey[1])
	{
		A2S_RandomKey(m_ChallengeKey);
		m_ChallengeLimit.SetKey(m_ChallengeKey);
	}
	m_SeenConfigVersion = m_ConfigVersion.load();

	// The answers published before a restart (new sockets) are still valid
	{
		std::lock_guard<std::mutex> lock(m_ConfigMutex);
		m_Replies = m_SharedReplies;
	}

	// Wake handle of WaitForPacket; without it the main thread sleeps until the tick
#ifdef _WIN32
	if (!m_hWakeEvent)
		m_hWakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
#else
	if (m_WakeFd < 0)
		m_WakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
	m_bConfigPublished = false;
	BeginFrame();

	m_bStopRequested.store(false);

	// CUtlRBTree's sentinel node is a function-local static shared by every tree of
	// the same type, initialised on first use under a compiler guard that race
	// detectors do not understand: initialise it here, before the thread exists.
	{
		netadr_t adr;
		Q_memset(&adr, 0, sizeof(adr));
		CIPRateLimit warmUp;
		warmUp.CheckIP(adr, 0, 1.0f, 1.0f, 1.0f);
	}

#ifdef _WIN32
	m_hThread = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
	bool created = m_hThread != nullptr;
#else
	bool created = pthread_create(&m_Thread, nullptr, ThreadEntry, this) == 0;
#endif

	if (!created)
	{
		Con_Printf("WARNING: network thread: cannot create the thread, reading packets on the main thread\n");
		m_bEnabled = false;
		return;
	}

	m_bRunning = true;
	Con_DPrintf("Network thread started, queue of %u packets\n", (unsigned int)m_Queue.Capacity());
}

void CNetReceiveThread::Stop()
{
	if (!m_bRunning)
		return;

	m_bStopRequested.store(true);

#ifdef _WIN32
	WaitForSingleObject(m_hThread, INFINITE);
	CloseHandle(m_hThread);
#else
	pthread_join(m_Thread, nullptr);
#endif

	// Packets still queued were received from sockets that are about to be closed
	m_Queue.Clear();
	m_bRunning = false;

#ifdef _WIN32
	if (m_hWakeEvent)
	{
		CloseHandle(m_hWakeEvent);
		m_hWakeEvent = nullptr;
	}
#else
	if (m_WakeFd >= 0)
	{
		close(m_WakeFd);
		m_WakeFd = -1;
	}
#endif
}

void CNetReceiveThread::ClientsChanged()
{
	if (m_bRunning)
		BeginFrame();
}

bool CNetReceiveThread::UpdateClientList()
{
	// Same clients as the loop of SV_ReadPackets
	ClientAddress clients[MAX_CLIENTS];
	int numClients = 0;

	for (int i = 0; i < g_psvs.maxclients && numClients < MAX_CLIENTS; i++)
	{
		const client_t *cl = &g_psvs.clients[i];
		if (!cl->connected && !cl->active && !cl->spawned)
			continue;

		if (cl->netchan.remote_address.type != NA_IP)
			continue;

		ClientAddress &client = clients[numClients++];
		Q_memset(&client, 0, sizeof(client));
		client.ip = *(const uint32 *)cl->netchan.remote_address.ip;
		client.port = cl->netchan.remote_address.port;
	}

	if (numClients == m_NumClients && !Q_memcmp(clients, m_Clients, numClients * sizeof(ClientAddress)))
		return false;

	Q_memcpy(m_Clients, clients, numClients * sizeof(ClientAddress));
	m_NumClients = numClients;
	m_ClientsVersion++;
	return true;
}

void CNetReceiveThread::BeginFrame()
{
	UpdateClientList();

	FilterInputs inputs = CurrentInputs();
	if (!m_bConfigPublished || !(inputs == m_PublishedInputs))
		PublishConfig(inputs);

	uint32 drops = 0;
	for (int i = 0; i < NTD_COUNT; i++)
		drops += m_Dropped[i].load(std::memory_order_relaxed);

	g_FrameProfiler.AddCounter(FPC_NET_THREAD_DROPS, (int)(drops - m_ReportedDrops));
	m_ReportedDrops = drops;
}

CNetReceiveThread::FilterInputs CNetReceiveThread::CurrentInputs() const
{
	FilterInputs inputs;
	inputs.filterEnabled = net_thread_filter.value != 0.0f;
	inputs.rateLimitEnabled = g_RehldsHookchains.m_PreprocessPacket.isEmpty()
		&& g_RehldsHookchains.m_SV_CheckConnectionLessRateLimits.isEmpty();
	inputs.filterBan = sv_filterban.value;
	inputs.maxQueriesSec = max_queries_sec.value;
	inputs.maxQueriesSecGlobal = max_queries_sec_global.value;
	inputs.maxQueriesWindow = max_queries_window.value;
	inputs.maxChallengesSec = sv_a2s_challenges_sec.value;
	inputs.filtersVersion = ipfilters_version;
	inputs.numFilters = numipfilters;
	inputs.preprocessHooked = !g_RehldsHookchains.m_PreprocessPacket.isEmpty();
	inputs.clientsVersion = m_ClientsVersion;
	return inputs;
}

void CNetReceiveThread::PublishConfig(const FilterInputs &inputs)
{
	// Not std::make_shared: it needs a newer libstdc++ than the one shipped with HLDS
	std::shared_ptr<FilterConfig> config(new FilterConfig);
	config->filterEnabled = inputs.filterEnabled;
	config->rateLimitEnabled = inputs.rateLimitEnabled;
	config->negativeFilter = inputs.filterBan == 1.0f;
	config->maxQueriesSec = inputs.maxQueriesSec;
	config->maxQueriesSecGlobal = inputs.maxQueriesSecGlobal;
	config->maxQueriesWindow = inputs.maxQueriesWindow;
	config->maxChallengesSec = inputs.maxChallengesSec;
	config->dropUnknownAddresses = inputs.filterEnabled && !inputs.preprocessHooked;
	config->numClients = m_NumClients;
	Q_memcpy(config->clients, m_Clients, m_NumClients * sizeof(ClientAddress));

	// Same rules as SV_FilterPacket, with the expiry converted from realtime
	// to a monotonic deadline that the thread can check on its own
	uint64 now = Sys_MonotonicNs();
	config->entries.reserve(numipfilters);
	for (int i = 0; i < numipfilters; i++)
	{
		const ipfilter_t &filter = ipfilters[i];

		FilterEntry entry;
		entry.mask = filter.mask;
		entry.compare = filter.compare.u32;
		entry.expiresNs = 0;

		if (filter.compare.u32 != 0xFFFFFFFF && filter.banEndTime != 0.0f)
		{
			double remaining = filter.banEndTime - realtime;
			if (remaining <= 0.0)
				continue;

			entry.expiresNs = now + (uint64)(remaining * NS_PER_SEC);
		}

		config->entries.push_back(entry);
	}

	{
		std::lock_guard<std::mutex> lock(m_ConfigMutex);
		m_SharedConfig = config;
	}

	m_ConfigVersion.fetch_add(1, std::memory_order_release);
	m_PublishedInputs = inputs;
	m_bConfigPublished = true;
}

bool CNetReceiveThread::Pop(sizebuf_t *msg, netadr_t *from)
{
	Packet *packet = m_Queue.Front();
	if (!packet)
		return false;

	NET_TransferRawData(msg, packet->data, packet->size);
	*from = packet->from;

	if (g_FrameProfiler.IsActive())
		g_FrameProfiler.AddCounter(FPC_NET_QUEUE_WAIT_US, (int)((Sys_MonotonicNs() - packet->arrivalNs) / 1000));

	m_Queue.Pop();
	return true;
}

// The main thread announces that it waits, then checks the queue; the network
// thread queues, then checks whether the main thread waits. With a full fence on
// each side, at least one of them sees the other's change: the main thread either
// finds the packet or is woken. A wake-up that arrives after the main thread
// stopped waiting only cuts its next wait short.
void CNetReceiveThread::WaitForPacket(uint64 deadlineNs)
{
	m_bMainWaiting.store(true);
	std::atomic_thread_fence(std::memory_order_seq_cst);

	uint64 now = Sys_MonotonicNs();
	if (!HasPackets() && now < deadlineNs)
	{
#ifdef _WIN32
		// Millisecond timeout, rounded down: the caller sleeps the rest precisely
		DWORD timeoutMs = (DWORD)((deadlineNs - now) / 1000000);
		if (m_hWakeEvent && timeoutMs > 0)
			WaitForSingleObject(m_hWakeEvent, timeoutMs);
#else
		if (m_WakeFd >= 0)
		{
			struct pollfd fd;
			fd.fd = m_WakeFd;
			fd.events = POLLIN;
			fd.revents = 0;

			uint64 remaining = deadlineNs - now;
			struct timespec timeout;
			timeout.tv_sec = (time_t)(remaining / NS_PER_SEC);
			timeout.tv_nsec = (long)(remaining % NS_PER_SEC);

			if (ppoll(&fd, 1, &timeout, nullptr) > 0)
			{
				uint64 value;
				if (read(m_WakeFd, &value, sizeof(value)) < 0)
				{
					// EAGAIN: already reset, nothing to do
				}
			}
		}
#endif
	}

	m_bMainWaiting.store(false, std::memory_order_relaxed);
}

void CNetReceiveThread::WakeMainThread()
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (!m_bMainWaiting.load())
		return;

#ifdef _WIN32
	if (m_hWakeEvent)
		SetEvent(m_hWakeEvent);
#else
	if (m_WakeFd >= 0)
	{
		uint64 one = 1;
		if (write(m_WakeFd, &one, sizeof(one)) < 0)
		{
			// EAGAIN: the counter is already set, the main thread wakes anyway
		}
	}
#endif
}

void CNetReceiveThread::Run()
{
	SetCurrentThreadName("rehlds-net");

	while (!m_bStopRequested.load(std::memory_order_relaxed))
	{
		if (!WaitReadable(WAIT_TIMEOUT_MS))
			continue;

		RefreshConfig();

		int queued = 0;
		for (int i = 0; i < m_NumSockets; i++)
			queued += Drain(m_Sockets[i]);

		if (queued)
			WakeMainThread();
	}

	m_Config.reset();
	m_Replies.reset();
}

bool CNetReceiveThread::WaitReadable(int timeoutMs)
{
#ifdef _WIN32
	fd_set readSet;
	FD_ZERO(&readSet);
	for (int i = 0; i < m_NumSockets; i++)
		FD_SET(m_Sockets[i], &readSet);

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = timeoutMs * 1000;
	return select(0, &readSet, nullptr, nullptr, &tv) > 0;
#else
	struct pollfd fds[2];
	for (int i = 0; i < m_NumSockets; i++)
	{
		fds[i].fd = m_Sockets[i];
		fds[i].events = POLLIN;
		fds[i].revents = 0;
	}

	return poll(fds, m_NumSockets, timeoutMs) > 0;
#endif
}

void CNetReceiveThread::RefreshConfig()
{
	uint32 version = m_ConfigVersion.load(std::memory_order_acquire);
	if (version == m_SeenConfigVersion)
		return;

	std::lock_guard<std::mutex> lock(m_ConfigMutex);
	m_Config = m_SharedConfig;
	m_Replies = m_SharedReplies;
	m_SeenConfigVersion = version;
}

int CNetReceiveThread::Drain(SOCKET sock)
{
#ifndef _WIN32
	if (m_Batch)
		return DrainBatched(sock);
#endif

	int queued = 0;
	for (int count = 0; count < MAX_PACKETS_PER_DRAIN; count++)
	{
		// Receive straight into the next queue slot; when the queue is full, into
		// a scratch buffer so that the packet can still be classified and dropped
		Packet *slot = m_Queue.BeginPush();
		unsigned char *buffer = slot ? slot->data : m_Scratch;

		struct sockaddr from;
		socklen_t fromlen = sizeof(from);
		int size = recvfrom(sock, (char *)buffer, MAX_UDP_PACKET, 0, &from, &fromlen);
		if (size < 0)
		{
			int err = LastSocketError();
			if (IsWouldBlock(err))
				return queued;

			if (!IsIgnoredError(err))
			{
#ifdef _WIN32
				if (err == WSAEMSGSIZE)
				{
					Count(m_Received);
					Count(m_Dropped[NTD_MALFORMED]);
					continue;
				}
#endif
				Count(m_RecvErrors);
				return queued;
			}

			continue;
		}

		if (HandlePacket(sock, buffer, size, &from, fromlen))
			queued++;
	}
	return queued;
}

#ifndef _WIN32
// The packets are received into the thread's own buffers, and those queued are copied
// into the queue: a few hundred bytes for a game packet, against one system call per
// packet. A batch smaller than RECV_BATCH means the socket is empty, which saves the
// last call that only finds it so
int CNetReceiveThread::DrainBatched(SOCKET sock)
{
	struct mmsghdr messages[RECV_BATCH];
	struct iovec vectors[RECV_BATCH];
	struct sockaddr addresses[RECV_BATCH];

	int queued = 0;
	for (int count = 0; count < MAX_PACKETS_PER_DRAIN; count += RECV_BATCH)
	{
		for (int i = 0; i < RECV_BATCH; i++)
		{
			vectors[i].iov_base = m_Batch[i];
			vectors[i].iov_len = MAX_UDP_PACKET;
			Q_memset(&messages[i].msg_hdr, 0, sizeof(messages[i].msg_hdr));
			messages[i].msg_hdr.msg_name = &addresses[i];
			messages[i].msg_hdr.msg_namelen = sizeof(addresses[i]);
			messages[i].msg_hdr.msg_iov = &vectors[i];
			messages[i].msg_hdr.msg_iovlen = 1;
		}

		int received = recvmmsg(sock, messages, RECV_BATCH, MSG_DONTWAIT, nullptr);
		if (received < 0)
		{
			int err = LastSocketError();
			if (IsWouldBlock(err))
				return queued;

			if (!IsIgnoredError(err))
			{
				Count(m_RecvErrors);
				return queued;
			}

			continue;
		}

		Count(m_RecvCalls);
		for (int i = 0; i < received; i++)
		{
			// Truncated to MAX_UDP_PACKET like recvfrom, Classify drops it
			if (HandlePacket(sock, m_Batch[i], (int)messages[i].msg_len, &addresses[i], (int)messages[i].msg_hdr.msg_namelen))
				queued++;
		}

		if (received < RECV_BATCH)
			return queued;
	}
	return queued;
}
#endif

bool CNetReceiveThread::HandlePacket(SOCKET sock, const unsigned char *buffer, int size, struct sockaddr *from, int fromlen)
{
	Count(m_Received);

	netadr_t adr;
	Q_memset(&adr, 0, sizeof(adr));
	SockadrToNetadr(from, &adr);

	uint64 now = Sys_MonotonicNs();
	int queryType = A2SQ_NONE;
	bool sendChallenge = false;
	int dropReason = Classify(buffer, size, adr, now, queryType, sendChallenge);
	if (dropReason != NTD_NONE)
	{
		Count(m_Dropped[dropReason]);

		if (dropReason == NTD_BANNED)
			SendBan(sock, from, fromlen, now);

		return false;
	}

	if (queryType != A2SQ_NONE)
	{
		if (sendChallenge)
		{
			SendChallenge(sock, from, fromlen, adr, now);
			Count(m_ChallengesSent);
		}
		else
		{
			SendQueryReply(sock, from, fromlen, queryType);
			Count(m_Answered[queryType]);
		}
		return false;
	}

	// The slot the packet was received into, or the one it is copied into
	Packet *slot = m_Queue.BeginPush();
	if (!slot)
	{
		Count(m_Dropped[NTD_QUEUE_FULL]);
		return false;
	}

	if (buffer != slot->data)
		Q_memcpy(slot->data, buffer, size);

	slot->from = adr;
	slot->size = size;
	slot->arrivalNs = now;
	m_Queue.CommitPush();

	uint32 depth = (uint32)m_Queue.Size();
	if (depth > m_MaxQueueDepth.load(std::memory_order_relaxed))
		m_MaxQueueDepth.store(depth, std::memory_order_relaxed);

	return true;
}

int CNetReceiveThread::Classify(const unsigned char *data, int size, const netadr_t &from, uint64 now, int &queryType, bool &sendChallenge)
{
	// Rejected by NET_QueuePacket on the synchronous path as well:
	// oversized packets, and split packets (only the server sends them)
	if (size >= MAX_UDP_PACKET)
		return NTD_MALFORMED;

	if (size >= 4 && *(int32 *)data == NET_HEADER_FLAG_SPLITPACKET)
		return NTD_MALFORMED;

	const FilterConfig *config = m_Config.get();
	if (!config || !config->filterEnabled)
		return NTD_NONE;

	if (size < 4)
		return NTD_MALFORMED;

	if (*(uint32 *)data != 0xFFFFFFFF)
	{
		// Netchan traffic: SV_ReadPackets ignores it unless it comes from a client
		if (config->dropUnknownAddresses && from.type == NA_IP && !IsClientAddress(from))
			return NTD_UNKNOWN_ADDRESS;

		return NTD_NONE;
	}

	// A2S queries, when the thread answers them (sv_a2s_cache 1) and the rate limits
	// are the engine's (no plugin hook): their own rate limits, then the IP filters.
	// Those it has no answer for yet go to the main thread, where Steam answers them.
	uint32 challenge;
	int query = (m_Replies && config->rateLimitEnabled && from.type == NA_IP) ? A2S_ClassifyQuery(data, size, challenge) : A2SQ_NONE;
	if (query != A2SQ_NONE)
	{
		// Tell the main thread that this answer is wanted
		if (!m_QueryDemand[query].load(std::memory_order_relaxed))
			m_QueryDemand[query].store(true, std::memory_order_relaxed);

		// Like Steam: answers go to addresses that proved they receive packets,
		// except loopback ones, which cannot be spoofed from the network
		bool hasReply = HasReply(query, now);
		bool needsChallenge = hasReply && from.ip[0] != 127 && !IsValidChallenge(from, challenge, now);

		// Challenges and answers have separate budgets: a spoofed flood only gets
		// challenges, and leaves the answers to the addresses that proved theirs
		if (needsChallenge)
		{
			if (!m_ChallengeLimit.Check(*(const uint32 *)from.ip, now, config->maxQueriesSec, config->maxQueriesWindow, config->maxChallengesSec))
			{
				Count(m_ChallengesLimited);
				return NTD_RATE_LIMITED;
			}
		}
		else if (!m_QueryRateLimit.CheckIP(from, (time_t)time(nullptr), config->maxQueriesSec, config->maxQueriesSecGlobal, config->maxQueriesWindow))
			return NTD_RATE_LIMITED;

		if (IsBanned(from, now))
			return NTD_BANNED;

		if (hasReply)
		{
			queryType = query;
			sendChallenge = needsChallenge;
		}

		return NTD_NONE;
	}

	// Same order as SV_ReadPackets: rate limits, then IP filters
	if (config->rateLimitEnabled
		&& !m_RateLimit.CheckIP(from, (time_t)time(nullptr), config->maxQueriesSec, config->maxQueriesSecGlobal, config->maxQueriesWindow))
		return NTD_RATE_LIMITED;

	if (IsBanned(from, now))
		return NTD_BANNED;

	return NTD_NONE;
}

bool CNetReceiveThread::HasReply(int queryType, uint64 now) const
{
	const A2SReplies *replies = m_Replies.get();
	if (!replies)
		return false;

	const A2SReplies::Reply &reply = replies->replies[queryType];
	return !reply.packet.empty() && now - reply.capturedNs <= MAX_REPLY_AGE_NS;
}

bool CNetReceiveThread::IsValidChallenge(const netadr_t &from, uint32 challenge, uint64 now) const
{
	if (challenge == A2S_NO_CHALLENGE)
		return false;

	uint32 ip = *(const uint32 *)from.ip;
	uint64 period = now / CHALLENGE_PERIOD_NS;
	return challenge == A2S_MakeChallenge(m_ChallengeKey, ip, period)
		|| challenge == A2S_MakeChallenge(m_ChallengeKey, ip, period - 1);
}

void CNetReceiveThread::SendChallenge(SOCKET sock, const struct sockaddr *to, int tolen, const netadr_t &from, uint64 now)
{
	// Same packet as Steam's challenges
	uint32 challenge = A2S_MakeChallenge(m_ChallengeKey, *(const uint32 *)from.ip, now / CHALLENGE_PERIOD_NS);

	unsigned char packet[9];
	*(int32 *)packet = -1;
	packet[4] = S2C_CHALLENGE;
	Q_memcpy(&packet[5], &challenge, sizeof(challenge));

	sendto(sock, (const char *)packet, sizeof(packet), 0, to, tolen);
}

void CNetReceiveThread::SendQueryReply(SOCKET sock, const struct sockaddr *to, int tolen, int queryType)
{
	const std::vector<unsigned char> &packet = m_Replies->replies[queryType].packet;
	int size = (int)packet.size();
	int count = A2S_SplitCount(size);

	uint32 sequence = 0;
	if (count > 1)
	{
		sequence = m_NextSplitSequence++;
		if (m_NextSplitSequence > 0x7FFFFFFF)
			m_NextSplitSequence = FIRST_SPLIT_SEQUENCE;
	}

	for (int part = 0; part < count; part++)
	{
		int partSize = A2S_SplitPart(packet.data(), size, part, sequence, m_ReplyPacket);
		sendto(sock, (const char *)m_ReplyPacket, partSize, 0, to, tolen);
	}
}

bool CNetReceiveThread::IsBanned(const netadr_t &from, uint64 now) const
{
	const FilterConfig *config = m_Config.get();
	uint32 ip = *(const uint32 *)from.ip;

	for (int i = (int)config->entries.size() - 1; i >= 0; i--)
	{
		const FilterEntry &entry = config->entries[i];
		if (entry.expiresNs && entry.expiresNs <= now)
			continue;

		if ((ip & entry.mask) == entry.compare)
			return config->negativeFilter;
	}

	return !config->negativeFilter;
}

bool CNetReceiveThread::IsClientAddress(const netadr_t &from) const
{
	const FilterConfig *config = m_Config.get();
	uint32 ip = *(const uint32 *)from.ip;

	for (int i = 0; i < config->numClients; i++)
	{
		if (config->clients[i].ip == ip && config->clients[i].port == from.port)
			return true;
	}

	return false;
}

void CNetReceiveThread::SendBan(SOCKET sock, const struct sockaddr *to, int tolen, uint64 now)
{
	// The reply goes to an address that may be spoofed: keep it from being
	// used for reflection
	if (now - m_BanReplyWindowStart >= NS_PER_SEC)
	{
		m_BanReplyWindowStart = now;
		m_BanRepliesInWindow = 0;
	}

	if (m_BanRepliesInWindow >= MAX_BAN_REPLIES_PER_SEC)
		return;

	m_BanRepliesInWindow++;

	// Same packet as SV_SendBan
	unsigned char packet[4 + 1 + sizeof(BAN_MESSAGE)];
	*(int32 *)packet = -1;
	packet[4] = 108;
	Q_memcpy(&packet[5], BAN_MESSAGE, sizeof(BAN_MESSAGE));

	sendto(sock, (const char *)packet, sizeof(packet), 0, to, tolen);
	Count(m_BanReplies);
}

void CNetReceiveThread::PublishQueryReplies(std::shared_ptr<const A2SReplies> replies)
{
	{
		std::lock_guard<std::mutex> lock(m_ConfigMutex);
		m_SharedReplies = replies;
	}

	m_ConfigVersion.fetch_add(1, std::memory_order_release);
}

bool CNetReceiveThread::TakeQueryDemand(int queryType)
{
	if (!m_QueryDemand[queryType].load(std::memory_order_relaxed))
		return false;

	m_QueryDemand[queryType].store(false, std::memory_order_relaxed);
	return true;
}

void CNetReceiveThread::Cmd_Stats()
{
	if (!m_bEnabled)
	{
		Con_Printf("Network thread is off (-nonetthread, or a replay or the testsuite)\n");
		return;
	}

	Con_Printf("Network thread: %s, queue %u/%u packets (max %u), filter %s%s\n",
		m_bRunning ? "running" : "stopped",
		(unsigned int)m_Queue.Size(), (unsigned int)m_Queue.Capacity(),
		m_MaxQueueDepth.load(std::memory_order_relaxed),
		m_PublishedInputs.filterEnabled ? "on" : "off",
		(m_PublishedInputs.filterEnabled && !m_PublishedInputs.rateLimitEnabled) ? " (rate limits left to plugin hooks)" : "");

	Con_Printf("  received      %u\n", m_Received.load(std::memory_order_relaxed));
	for (int i = 0; i < NTD_COUNT; i++)
		Con_Printf("  dropped: %-12s %u\n", s_DropReasonNames[i], m_Dropped[i].load(std::memory_order_relaxed));
	Con_Printf("  ban replies   %u\n", m_BanReplies.load(std::memory_order_relaxed));
	Con_Printf("  recv errors   %u\n", m_RecvErrors.load(std::memory_order_relaxed));
#ifndef _WIN32
	if (m_Batch)
	{
		uint32 calls = m_RecvCalls.load(std::memory_order_relaxed);
		uint32 received = m_Received.load(std::memory_order_relaxed);
		Con_Printf("  recvmmsg calls %u (%.1f packets per call)\n", calls, calls ? (double)received / calls : 0.0);
	}
#endif
	Con_Printf("  A2S answered  info %u, players %u, rules %u, challenges %u (%u rate limited)\n",
		m_Answered[A2SQ_INFO].load(std::memory_order_relaxed),
		m_Answered[A2SQ_PLAYER].load(std::memory_order_relaxed),
		m_Answered[A2SQ_RULES].load(std::memory_order_relaxed),
		m_ChallengesSent.load(std::memory_order_relaxed),
		m_ChallengesLimited.load(std::memory_order_relaxed));
	A2SCache_Stats();
}

void NetReceiveThread_Init()
{
	g_NetReceiveThread.Init();
	A2SCache_Init();
}
