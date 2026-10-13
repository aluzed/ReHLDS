#pragma once

#include "maintypes.h"
#include "threading/spsc_ring.h"

#include <atomic>

// Network send thread (MT-304). With -netsend, the datagrams the server sends on its
// IP socket are queued by the main thread and handed to the kernel by this thread, so
// that the frame no longer waits for sendto: about 32 system calls per tick with 32
// players, more with split packets.
//
// The packets are queued once built and split (NET_SendLong), so what goes out on
// the wire is unchanged; the thread sends them in queue order, which keeps the order
// per destination. On Linux it hands every queued packet to one sendmmsg call.
//
// Only the main thread queues: other threads (the network receive thread replying to
// banned addresses) still send themselves. When the queue is full, the main thread
// waits for room rather than dropping or reordering packets.
//
// Waking the thread costs a system call too: once the queue is empty, the thread
// spins a little before sleeping, so that the packets of one tick, built a few
// microseconds apart, wake it about once.

class CNetSendThread
{
public:
	CNetSendThread();

	void Init();

	// The server socket was opened, or is about to be closed: the thread sends what
	// is queued before Stop returns
	void Start(SOCKET serverSocket);
	void Stop();

	bool IsRunning() const { return m_bRunning; }

	// Main thread: queues a datagram (at most MAX_ROUTEABLE_PACKET bytes) to send on the
	// server socket. Returns false when the caller must send it itself.
	bool Send(SOCKET s, const void *data, int size, const struct sockaddr *to, int tolen);

	// Main thread: prints the send errors the thread met since the last call
	void ReportErrors();

	void Cmd_Stats();

private:
	struct Packet
	{
		struct sockaddr to;
		int tolen;
		int size;
		unsigned char data[MAX_ROUTEABLE_PACKET];
	};

#ifdef _WIN32
	static DWORD WINAPI ThreadEntry(LPVOID param);
#else
	static void *ThreadEntry(void *param);
#endif

	void Run();
	// Sends the queued packets, returns how many
	int SendQueued();
	void Sleep();
	void Wake();
	void CountError(int err);

	// Main thread state
	bool m_bEnabled;
	bool m_bRunning;
	int m_QueueSize;
	uint32 m_ReportedErrors;

#ifdef _WIN32
	HANDLE m_hThread;
	HANDLE m_hWakeEvent;
#else
	pthread_t m_Thread;
	int m_WakeFd;
#endif
	SOCKET m_Socket;

	SpscRing<Packet> m_Queue;

	// Set by the thread before it sleeps: the main thread then wakes it after queuing
	std::atomic<bool> m_bSleeping;
	std::atomic<bool> m_bStopRequested;

	// Statistics written by the thread
	std::atomic<uint32> m_Sent;
	std::atomic<uint32> m_SendCalls;
	std::atomic<uint32> m_Errors;
	std::atomic<int> m_LastError;
	std::atomic<uint32> m_LargestBatch;

	// Statistics written by the main thread
	std::atomic<uint32> m_Queued;
	std::atomic<uint32> m_Wakes;
	std::atomic<uint32> m_QueueFullWaits;
	std::atomic<uint32> m_SentByMainThread;
};

extern CNetSendThread g_NetSendThread;

void NetSendThread_Init();
