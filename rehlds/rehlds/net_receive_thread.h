#pragma once

#include "archtypes.h"
#include "netadr.h"
#include "ipratelimit.h"
#include "threading/spsc_ring.h"
#include "a2s_cache.h"

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
	NTD_UNKNOWN_ADDRESS,      // netchan packet from an address that is not a connected client

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
//
// Packets that are not connectionless are only accepted from the addresses of
// connected clients, the same check as the client loop of SV_ReadPackets. The
// address list is published every frame when it changed, and by SV_ConnectClient
// before it accepts a client, so that the client's first packets are never
// dropped. When a plugin hooks PreprocessPacket, every packet is kept.
//
// A2S queries (sv_a2s_cache 1, MT-305): with the filter on and no plugin hooking
// PreprocessPacket or SV_CheckConnectionLessRateLimits, the thread answers them from the
// copy of Steam's answers the main thread publishes (a2s_cache.h), with its own
// challenges. They get their own rate limits (the max_queries_* values, counted apart
// from the other connectionless packets, so that a query flood does not keep players
// from connecting), then the IP filters, like every connectionless packet.
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

	// Main thread: a client was given a connection, publish its address before
	// it is told so
	void ClientsChanged();

	// Main thread: pops the oldest packet into msg and from
	bool Pop(sizebuf_t *msg, netadr_t *from);

	// Main thread: true when packets are waiting in the queue
	bool HasPackets() { return m_Queue.Front() != nullptr; }

	// Main thread: sleeps until a packet is queued or until deadlineNs (Sys_MonotonicNs
	// time), whichever comes first; returns at once when packets are already waiting.
	// May return early without a packet. sys_wake_on_packet, MT-102.
	void WaitForPacket(uint64 deadlineNs);

	// Main thread: answers to A2S queries, nullptr to stop answering them
	void PublishQueryReplies(std::shared_ptr<const A2SReplies> replies);

	// Main thread: whether a query of this type arrived since the last call
	bool TakeQueryDemand(int queryType);

	void Cmd_Stats();

private:
	struct Packet
	{
		netadr_t from;
		uint64 arrivalNs;
		int size;
		unsigned char data[MAX_UDP_PACKET];
	};

	struct ClientAddress
	{
		uint32 ip;
		uint16 port;	// network byte order, like netadr_t
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

		bool dropUnknownAddresses;
		int numClients;
		ClientAddress clients[MAX_CLIENTS];
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
		bool preprocessHooked;
		int clientsVersion;

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
	// Returns the number of packets queued
	int Drain(SOCKET sock);
	// Wakes the main thread when it waits in WaitForPacket
	void WakeMainThread();
	// Returns NTD_NONE to queue the packet, or the reason for dropping it. Sets
	// queryType when the thread answers the packet itself, with a challenge when
	// sendChallenge is set.
	int Classify(const unsigned char *data, int size, const netadr_t &from, uint64 now, int &queryType, bool &sendChallenge);
	// Whether the published answer to this query can be used
	bool HasReply(int queryType, uint64 now) const;
	bool IsValidChallenge(const netadr_t &from, uint32 challenge, uint64 now) const;
	void SendQueryReply(SOCKET sock, const struct sockaddr *to, int tolen, int queryType);
	void SendChallenge(SOCKET sock, const struct sockaddr *to, int tolen, const netadr_t &from, uint64 now);
	bool IsBanned(const netadr_t &from, uint64 now) const;
	bool IsClientAddress(const netadr_t &from) const;
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

	// Addresses of the connected clients when they were last published
	bool UpdateClientList();
	ClientAddress m_Clients[MAX_CLIENTS];
	int m_NumClients;
	int m_ClientsVersion;

#ifdef _WIN32
	HANDLE m_hThread;
#else
	pthread_t m_Thread;
#endif
	std::atomic<bool> m_bStopRequested;
	SOCKET m_Sockets[2];
	int m_NumSockets;

	SpscRing<Packet> m_Queue;

	// The main thread sleeps in WaitForPacket: the network thread signals the wake
	// handle after queuing packets (see WaitForPacket for the ordering)
	std::atomic<bool> m_bMainWaiting;
#ifdef _WIN32
	HANDLE m_hWakeEvent;
#else
	int m_WakeFd;
#endif

	// Configuration hand-off: the main thread swaps the pointer under the mutex
	// and bumps the version; the thread takes a reference when the version changed.
	std::mutex m_ConfigMutex;
	std::shared_ptr<const FilterConfig> m_SharedConfig;
	std::shared_ptr<const A2SReplies> m_SharedReplies;
	std::atomic<uint32> m_ConfigVersion;

	// Network thread state
	std::shared_ptr<const FilterConfig> m_Config;
	std::shared_ptr<const A2SReplies> m_Replies;
	uint32 m_SeenConfigVersion;
	CIPRateLimit m_RateLimit;
	CIPRateLimit m_QueryRateLimit;
	uint64 m_ChallengeKey[2];	// written before the thread starts
	uint32 m_NextSplitSequence;
	unsigned char m_ReplyPacket[MAX_ROUTEABLE_PACKET];

	// Set by the network thread when a query arrives, taken by the main thread
	std::atomic<bool> m_QueryDemand[A2SQ_COUNT];
	unsigned char m_Scratch[MAX_UDP_PACKET];
	uint64 m_BanReplyWindowStart;
	int m_BanRepliesInWindow;

	// Statistics, written by the network thread only
	std::atomic<uint32> m_Received;
	std::atomic<uint32> m_Dropped[NTD_COUNT];
	std::atomic<uint32> m_BanReplies;
	std::atomic<uint32> m_Answered[A2SQ_COUNT];
	std::atomic<uint32> m_ChallengesSent;
	std::atomic<uint32> m_RecvErrors;
	std::atomic<uint32> m_MaxQueueDepth;
};

extern CNetReceiveThread g_NetReceiveThread;

void NetReceiveThread_Init();
