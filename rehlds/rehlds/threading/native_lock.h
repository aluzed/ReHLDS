#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

// A lock and one condition, on native primitives: the engine is built without
// exceptions, which std::mutex and std::condition_variable may throw
class CNativeLock
{
public:
#ifdef _WIN32
	CNativeLock() { InitializeSRWLock(&m_Lock); InitializeConditionVariable(&m_Cond); }
	void Lock() { AcquireSRWLockExclusive(&m_Lock); }
	void Unlock() { ReleaseSRWLockExclusive(&m_Lock); }
	void Wait() { SleepConditionVariableSRW(&m_Cond, &m_Lock, INFINITE, 0); }
	void WaitMs(int ms) { SleepConditionVariableSRW(&m_Cond, &m_Lock, ms, 0); }
	void WakeAll() { WakeAllConditionVariable(&m_Cond); }
#else
	CNativeLock()
	{
		pthread_mutex_init(&m_Lock, nullptr);
		pthread_condattr_t attr;
		pthread_condattr_init(&attr);
		pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
		pthread_cond_init(&m_Cond, &attr);
		pthread_condattr_destroy(&attr);
	}
	void Lock() { pthread_mutex_lock(&m_Lock); }
	void Unlock() { pthread_mutex_unlock(&m_Lock); }
	void Wait() { pthread_cond_wait(&m_Cond, &m_Lock); }
	void WaitMs(int ms)
	{
		timespec deadline;
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		deadline.tv_sec += ms / 1000;
		deadline.tv_nsec += (ms % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L)
		{
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&m_Cond, &m_Lock, &deadline);
	}
	void WakeAll() { pthread_cond_broadcast(&m_Cond); }
#endif

private:
#ifdef _WIN32
	SRWLOCK m_Lock;
	CONDITION_VARIABLE m_Cond;
#else
	pthread_mutex_t m_Lock;
	pthread_cond_t m_Cond;
#endif
};
