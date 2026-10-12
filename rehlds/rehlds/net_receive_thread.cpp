#include "precompiled.h"

#ifndef _WIN32
#include <poll.h>
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

	const char *const s_DropReasonNames[NTD_COUNT] =
	{
		"queue full",
		"malformed",
		"rate limited",
		"banned",
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
		&& maxQueriesWindow == other.maxQueriesWindow
		&& filtersVersion == other.filtersVersion
		&& numFilters == other.numFilters;
}

CNetReceiveThread::CNetReceiveThread() :
	m_bEnabled(false),
	m_bRunning(false),
	m_QueueSize(DEFAULT_QUEUE_SIZE),
	m_PublishedInputs(),
	m_bConfigPublished(false),
	m_ReportedDrops(0),
#ifdef _WIN32
	m_hThread(nullptr),
#endif
	m_bStopRequested(false),
	m_NumSockets(0),
	m_ConfigVersion(0),
	m_SeenConfigVersion(0),
	m_BanReplyWindowStart(0),
	m_BanRepliesInWindow(0),
	m_Received(0),
	m_BanReplies(0),
	m_RecvErrors(0),
	m_MaxQueueDepth(0)
{
	for (int i = 0; i < NTD_COUNT; i++)
		m_Dropped[i].store(0, std::memory_order_relaxed);
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

	if (!COM_CheckParm("-netthread"))
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
	m_SeenConfigVersion = m_ConfigVersion.load();
	m_bConfigPublished = false;
	BeginFrame();

	m_bStopRequested.store(false);

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
}

void CNetReceiveThread::BeginFrame()
{
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
	inputs.filtersVersion = ipfilters_version;
	inputs.numFilters = numipfilters;
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

void CNetReceiveThread::Run()
{
	SetCurrentThreadName("rehlds-net");

	while (!m_bStopRequested.load(std::memory_order_relaxed))
	{
		if (!WaitReadable(WAIT_TIMEOUT_MS))
			continue;

		RefreshConfig();

		for (int i = 0; i < m_NumSockets; i++)
			Drain(m_Sockets[i]);
	}

	m_Config.reset();
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
	m_SeenConfigVersion = version;
}

void CNetReceiveThread::Drain(SOCKET sock)
{
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
				return;

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
				return;
			}

			continue;
		}

		Count(m_Received);

		netadr_t adr;
		Q_memset(&adr, 0, sizeof(adr));
		SockadrToNetadr(&from, &adr);

		uint64 now = Sys_MonotonicNs();
		int dropReason = Classify(buffer, size, adr, now);
		if (dropReason != NTD_NONE)
		{
			Count(m_Dropped[dropReason]);

			if (dropReason == NTD_BANNED)
				SendBan(sock, &from, fromlen, now);

			continue;
		}

		if (!slot)
		{
			Count(m_Dropped[NTD_QUEUE_FULL]);
			continue;
		}

		slot->from = adr;
		slot->size = size;
		slot->arrivalNs = now;
		m_Queue.CommitPush();

		uint32 depth = (uint32)m_Queue.Size();
		if (depth > m_MaxQueueDepth.load(std::memory_order_relaxed))
			m_MaxQueueDepth.store(depth, std::memory_order_relaxed);
	}
}

int CNetReceiveThread::Classify(const unsigned char *data, int size, const netadr_t &from, uint64 now)
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

	// Connected clients' packets are matched against the client list on the main thread
	if (*(uint32 *)data != 0xFFFFFFFF)
		return NTD_NONE;

	// Same order as SV_ReadPackets: rate limits, then IP filters
	if (config->rateLimitEnabled
		&& !m_RateLimit.CheckIP(from, (time_t)time(nullptr), config->maxQueriesSec, config->maxQueriesSecGlobal, config->maxQueriesWindow))
		return NTD_RATE_LIMITED;

	if (IsBanned(from, now))
		return NTD_BANNED;

	return NTD_NONE;
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

void CNetReceiveThread::Cmd_Stats()
{
	if (!m_bEnabled)
	{
		Con_Printf("Network thread is off (start the server with -netthread)\n");
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
}

void NetReceiveThread_Init()
{
	g_NetReceiveThread.Init();
}
