#pragma once

#include <atomic>

#include "race_annotations.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

// Fixed set of worker threads that run batches of items together with the main
// thread, for short per-frame fan-outs such as snapshot encoding (MT-403).
//
// A batch is opened by the main thread, which then publishes its items as they
// become ready (Publish), so that the workers start on the first ones while the main
// thread prepares the next. Finish runs what is left on the main thread too and
// returns once every item is done:
//
//	pool.BeginBatch(EncodeItem, context);
//	for (each item) { prepare it; pool.Publish(++ready); }
//	pool.Finish();
//
// Workers spin for a short time between batches, then sleep until the next one.
class CWorkerPool
{
public:
	typedef void (*ItemFunc)(void *context, int item);
	// Called once on each worker thread when it starts, with its index
	typedef void (*ThreadInitFunc)(int worker);

	CWorkerPool();
	~CWorkerPool();

	bool Start(int numWorkers, ThreadInitFunc init, const char *name);
	void Stop();

	bool IsRunning() const { return m_NumWorkers > 0; }

	// How long a worker spins for the next batch before sleeping
	void SetSpinTime(uint32 microseconds) { m_SpinNs.store((uint64)microseconds * 1000, std::memory_order_relaxed); }
	int NumWorkers() const { return m_NumWorkers; }

	// Main thread only, one batch at a time
	void BeginBatch(ItemFunc func, void *context);
	void Publish(int count);	// items [0, count) can run; count never decreases
	void Finish();

	// Items of the last batches run by the workers (the rest ran on the main thread)
	uint32 TakeWorkerItems() { return m_WorkerItems.exchange(0, std::memory_order_relaxed); }

private:
	struct Worker
	{
		CWorkerPool *pool;
		int index;
#ifdef _WIN32
		HANDLE thread;
#else
		pthread_t thread;
#endif
	};

	static const int MAX_WORKERS = 16;

#ifdef _WIN32
	static DWORD WINAPI ThreadEntry(LPVOID param);
#else
	static void *ThreadEntry(void *param);
#endif
	void Run(int index);
	bool WaitForBatch(uint32 lastGeneration);
	bool RunOneItem();

	int m_NumWorkers;
	Worker m_Workers[MAX_WORKERS];
	ThreadInitFunc m_ThreadInit;
	const char *m_Name;

	ItemFunc m_Func;
	void *m_Context;

	std::atomic<uint32> m_Generation;	// one per batch
	std::atomic<bool> m_Open;			// items can still be published
	std::atomic<int> m_Next;			// next item to claim
	std::atomic<int> m_Published;
	std::atomic<int> m_Completed;
	std::atomic<int> m_InBatch;		// workers between seeing a batch and leaving it
	std::atomic<int> m_Sleepers;
	std::atomic<bool> m_Stop;
	std::atomic<uint32> m_WorkerItems;
	std::atomic<uint64> m_SpinNs;

#ifdef _WIN32
	SRWLOCK m_Lock;
	CONDITION_VARIABLE m_Wake;
#else
	pthread_mutex_t m_Lock;
	pthread_cond_t m_Wake;
#endif
};
