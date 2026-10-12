#pragma once

#include "archtypes.h"
#include "netadr.h"
#include "ipratelimit.h"
#include "threading/spsc_ring.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

// Reasons for dropping a packet on the network thread
enum NetThreadDropReason
{
	NTD_NONE = -1,

	NTD_QUEUE_FULL,           // the main thread did not drain the queue in time
	NTD_MALFORMED,            // oversized, split packet sent to the server, or shorter than 4 bytes
	NTD_RATE_LIMITED,         // connectionless packet over the max_queries_* limits
	NTD_BANNED,               // connectionless packet from an address rejected by the IP filters

	NTD_COUNT
};

// Receives the server socket's packets on a dedicated thread (-netthread).
//
// The thread blocks until the socket is readable, drains it and pushes every
// packet into a single-producer single-consumer queue of preallocated slots.
// NET_GetPacket(NS_SERVER) pops from that queue on the main thread and fills
// net_message and net_from exactly like the synchronous path, so the rest of
// the engine is unchanged. Fake lag and fake loss still run on the main thread.
//
// Early filtering (net_thread_filter 1): connectionless packets that the main
// thread would reject in SV_ReadPackets (IP rate limits, IP filters) are dropped
// on the network thread, so that floods cost no main thread time. The main
// thread publishes the filter list and the limits as an immutable snapshot.
// When a plugin hooks PreprocessPacket or SV_CheckConnectionLessRateLimits, the
// thread only drops malformed and banned packets and leaves rate limiting to the
// main thread, where the hooks run.
class CNetReceiveThread
{
public:
	CNetReceiveThread();
	~CNetReceiveThread();

	// Registers the cvars and commands, reads -netthread
	void Init();

	bool IsEnabled() const { return m_bEnabled; }
	bool IsRunning() const { return m_bRunning; }

	// Called by NET_Config right after the sockets are opened and right before
	// they are closed: the thread must never use a closed socket.
	void Start(SOCKET serverSocket, SOCKET serverSocketIpx);
	void Stop();

	// Main thread, once per server frame before reading packets: publishes the
	// filter configuration when it changed and reports drops to the profiler.
	void BeginFrame();

	// Main thread: pops the oldest packet into msg and from
	bool Pop(sizebuf_t *msg, netadr_t *from);

	void Cmd_Stats();

private:
	struct Packet
	{
		netadr_t from;
		uint64 arrivalNs;
		int size;
		unsigned char data[MAX_UDP_PACKET];
	};

	struct FilterEntry
	{
		uint32 mask;
		uint32 compare;
		uint64 expiresNs;	// Sys_MonotonicNs deadline, 0 for a permanent entry
	};

	// Immutable once published
	struct FilterConfig
	{
		bool filterEnabled;
		bool rateLimitEnabled;
		bool negativeFilter;	// sv_filterban 1: the entries are bans, otherwise the only allowed addresses
		float maxQueriesSec;
		float maxQueriesSecGlobal;
		float maxQueriesWindow;
		std::vector<FilterEntry> entries;
	};

	// Main thread copy of the inputs of the last published configuration
	struct FilterInputs
	{
		bool filterEnabled;
		bool rateLimitEnabled;
		float filterBan;
		float maxQueriesSec;
		float maxQueriesSecGlobal;
		float maxQueriesWindow;
		int filtersVersion;
		int numFilters;

		bool operator==(const FilterInputs &other) const;
	};

	// Native threads: the engine is built without exceptions, so std::thread
	// cannot report a failed creation
#ifdef _WIN32
	static DWORD WINAPI ThreadEntry(LPVOID param);
#else
	static void *ThreadEntry(void *param);
#endif

	void Run();
	bool WaitReadable(int timeoutMs);
	void Drain(SOCKET sock);
	// Returns NTD_NONE to queue the packet, or the reason for dropping it
	int Classify(const unsigned char *data, int size, const netadr_t &from, uint64 now);
	bool IsBanned(const netadr_t &from, uint64 now) const;
	void SendBan(SOCKET sock, const struct sockaddr *to, int tolen, uint64 now);
	void RefreshConfig();

	FilterInputs CurrentInputs() const;
	void PublishConfig(const FilterInputs &inputs);

	void Count(std::atomic<uint32> &counter)
	{
		// Single writer: no need for an atomic read-modify-write
		counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}

	// Main thread state
	bool m_bEnabled;
	bool m_bRunning;
	int m_QueueSize;
	FilterInputs m_PublishedInputs;
	bool m_bConfigPublished;
	uint32 m_ReportedDrops;

#ifdef _WIN32
	HANDLE m_hThread;
#else
	pthread_t m_Thread;
#endif
	std::atomic<bool> m_bStopRequested;
	SOCKET m_Sockets[2];
	int m_NumSockets;

	SpscRing<Packet> m_Queue;

	// Configuration hand-off: the main thread swaps the pointer under the mutex
	// and bumps the version; the thread takes a reference when the version changed.
	std::mutex m_ConfigMutex;
	std::shared_ptr<const FilterConfig> m_SharedConfig;
	std::atomic<uint32> m_ConfigVersion;

	// Network thread state
	std::shared_ptr<const FilterConfig> m_Config;
	uint32 m_SeenConfigVersion;
	CIPRateLimit m_RateLimit;
	unsigned char m_Scratch[MAX_UDP_PACKET];
	uint64 m_BanReplyWindowStart;
	int m_BanRepliesInWindow;

	// Statistics, written by the network thread only
	std::atomic<uint32> m_Received;
	std::atomic<uint32> m_Dropped[NTD_COUNT];
	std::atomic<uint32> m_BanReplies;
	std::atomic<uint32> m_RecvErrors;
	std::atomic<uint32> m_MaxQueueDepth;
};

extern CNetReceiveThread g_NetReceiveThread;

void NetReceiveThread_Init();
