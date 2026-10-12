#include "precompiled.h"

CWorkerPool::CWorkerPool() :
	m_NumWorkers(0),
	m_ThreadInit(nullptr),
	m_Name(""),
	m_Func(nullptr),
	m_Context(nullptr),
	m_Generation(0),
	m_Open(false),
	m_Next(0),
	m_Published(0),
	m_Completed(0),
	m_Sleepers(0),
	m_Stop(false),
	m_WorkerItems(0),
	m_SpinNs(0)
{
#ifdef _WIN32
	InitializeSRWLock(&m_Lock);
	InitializeConditionVariable(&m_Wake);
#else
	pthread_mutex_init(&m_Lock, nullptr);
	pthread_cond_init(&m_Wake, nullptr);
#endif

	RACE_BENIGN(&m_SpinNs, sizeof(m_SpinNs), "setting, read with relaxed ordering");
	RACE_BENIGN(&m_WorkerItems, sizeof(m_WorkerItems), "statistics");
}

CWorkerPool::~CWorkerPool()
{
	Stop();

#ifndef _WIN32
	pthread_cond_destroy(&m_Wake);
	pthread_mutex_destroy(&m_Lock);
#endif
}

bool CWorkerPool::Start(int numWorkers, ThreadInitFunc init, const char *name)
{
	if (IsRunning())
		return true;

	if (numWorkers > MAX_WORKERS)
		numWorkers = MAX_WORKERS;

	m_ThreadInit = init;
	m_Name = name;
	m_Stop.store(false);

	for (int i = 0; i < numWorkers; i++)
	{
		Worker *worker = &m_Workers[i];
		worker->pool = this;
		worker->index = i;

#ifdef _WIN32
		worker->thread = CreateThread(nullptr, 0, ThreadEntry, worker, 0, nullptr);
		bool created = worker->thread != nullptr;
#else
		bool created = pthread_create(&worker->thread, nullptr, ThreadEntry, worker) == 0;
#endif
		if (!created)
		{
			Stop();
			return false;
		}

		m_NumWorkers = i + 1;
	}

	return numWorkers > 0;
}

void CWorkerPool::Stop()
{
	if (!IsRunning())
		return;

	m_Stop.store(true);

#ifdef _WIN32
	AcquireSRWLockExclusive(&m_Lock);
	WakeAllConditionVariable(&m_Wake);
	ReleaseSRWLockExclusive(&m_Lock);
#else
	pthread_mutex_lock(&m_Lock);
	pthread_cond_broadcast(&m_Wake);
	pthread_mutex_unlock(&m_Lock);
#endif

	for (int i = 0; i < m_NumWorkers; i++)
	{
#ifdef _WIN32
		WaitForSingleObject(m_Workers[i].thread, INFINITE);
		CloseHandle(m_Workers[i].thread);
#else
		pthread_join(m_Workers[i].thread, nullptr);
#endif
	}

	m_NumWorkers = 0;
}

void CWorkerPool::BeginBatch(ItemFunc func, void *context)
{
	// Reset in this order: a worker late from the previous batch reads m_Next,
	// then m_Published, and claims nothing until items are published again
	m_Published.store(0);
	m_Next.store(0);
	m_Completed.store(0);
	m_Func = func;
	m_Context = context;
	m_Open.store(true);

	// Publishes the batch to the workers
	RACE_HAPPENS_BEFORE(&m_Generation);
	m_Generation.fetch_add(1);

	if (m_Sleepers.load() > 0)
	{
#ifdef _WIN32
		AcquireSRWLockExclusive(&m_Lock);
		WakeAllConditionVariable(&m_Wake);
		ReleaseSRWLockExclusive(&m_Lock);
#else
		pthread_mutex_lock(&m_Lock);
		pthread_cond_broadcast(&m_Wake);
		pthread_mutex_unlock(&m_Lock);
#endif
	}
}

void CWorkerPool::Publish(int count)
{
	RACE_HAPPENS_BEFORE(&m_Published);
	m_Published.store(count);
}

void CWorkerPool::Finish()
{
	while (RunOneItem())
		;

	m_Open.store(false);

	int published = m_Published.load();
	while (m_Completed.load() < published)
		_mm_pause();

	RACE_HAPPENS_AFTER(&m_Completed);
}

bool CWorkerPool::RunOneItem()
{
	int item = m_Next.load();
	while (item < m_Published.load())
	{
		if (m_Next.compare_exchange_weak(item, item + 1))
		{
			RACE_HAPPENS_AFTER(&m_Generation);
			RACE_HAPPENS_AFTER(&m_Published);
			m_Func(m_Context, item);
			RACE_HAPPENS_BEFORE(&m_Completed);
			m_Completed.fetch_add(1);
			return true;
		}
	}

	return false;
}

bool CWorkerPool::WaitForBatch(uint32 lastGeneration)
{
	uint64 spinEnd = Sys_MonotonicNs() + m_SpinNs.load(std::memory_order_relaxed);
	while (m_Generation.load() == lastGeneration && !m_Stop.load())
	{
		if (Sys_MonotonicNs() >= spinEnd)
		{
			// The sleeper count is raised before checking the generation again, and
			// BeginBatch raises the generation before reading it: one of the two
			// sees the other's change, so that no wake-up is missed
#ifdef _WIN32
			AcquireSRWLockExclusive(&m_Lock);
			m_Sleepers.fetch_add(1);
			while (m_Generation.load() == lastGeneration && !m_Stop.load())
				SleepConditionVariableSRW(&m_Wake, &m_Lock, INFINITE, 0);
			m_Sleepers.fetch_sub(1);
			ReleaseSRWLockExclusive(&m_Lock);
#else
			pthread_mutex_lock(&m_Lock);
			m_Sleepers.fetch_add(1);
			while (m_Generation.load() == lastGeneration && !m_Stop.load())
				pthread_cond_wait(&m_Wake, &m_Lock);
			m_Sleepers.fetch_sub(1);
			pthread_mutex_unlock(&m_Lock);
#endif
			break;
		}

		_mm_pause();
	}

	return !m_Stop.load();
}

void CWorkerPool::Run(int index)
{
#ifndef _WIN32
	char threadName[16];
	Q_snprintf(threadName, sizeof(threadName), "%s%d", m_Name, index + 1);
	pthread_setname_np(pthread_self(), threadName);
#endif

	if (m_ThreadInit)
		m_ThreadInit(index);

	uint32 generation = m_Generation.load();
	while (WaitForBatch(generation))
	{
		generation = m_Generation.load();

		// Items can be published until the batch is closed; a batch already
		// finished, or reset by the next one, has nothing left to claim
		for (;;)
		{
			if (RunOneItem())
			{
				m_WorkerItems.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			if (!m_Open.load())
			{
				// Last items may have been published just before the close
				if (!RunOneItem())
					break;

				m_WorkerItems.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			_mm_pause();
		}
	}
}

#ifdef _WIN32
DWORD WINAPI CWorkerPool::ThreadEntry(LPVOID param)
#else
void *CWorkerPool::ThreadEntry(void *param)
#endif
{
	Worker *worker = (Worker *)param;
	worker->pool->Run(worker->index);
	return 0;
}
