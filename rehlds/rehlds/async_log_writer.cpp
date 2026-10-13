#include "precompiled.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

cvar_t sv_log_async = { "sv_log_async", "1", 0, 0.0f, nullptr };
cvar_t sv_log_async_overflow = { "sv_log_async_overflow", "block", 0, 0.0f, nullptr };

namespace
{
	const uint32 QUEUE_SIZE = 4 * 1024 * 1024;

	enum : uint8
	{
		OP_OPEN_LOG,	// payload: full path
		OP_WRITE_LOG,	// payload: text
		OP_CLOSE_LOG,
		OP_APPEND,		// payload: full path, '\0', text
	};

	struct record_header_t
	{
		uint8 op;
		uint8 pad[3];
		uint32 size;	// payload bytes
	};

	// Native threads and locks: the engine is built without exceptions
	class CLock
	{
	public:
#ifdef _WIN32
		CLock() { InitializeSRWLock(&m_Lock); InitializeConditionVariable(&m_Cond); }
		void Lock() { AcquireSRWLockExclusive(&m_Lock); }
		void Unlock() { ReleaseSRWLockExclusive(&m_Lock); }
		void Wait() { SleepConditionVariableSRW(&m_Cond, &m_Lock, INFINITE, 0); }
		void WaitMs(int ms) { SleepConditionVariableSRW(&m_Cond, &m_Lock, ms, 0); }
		void WakeAll() { WakeAllConditionVariable(&m_Cond); }
#else
		CLock()
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

	// One producer (the main thread), one consumer (the writer thread). Positions are
	// byte counts since the start, the buffer index is the position modulo QUEUE_SIZE.
	// One lock and one condition for both directions: the writer waits for data, the
	// main thread for room or for a flush; every change wakes all of them.
	class CAsyncLogWriter
	{
	public:
		CAsyncLogWriter() :
			m_pBuffer(nullptr),
			m_Head(0),
			m_Tail(0),
			m_Flushed(0),
			m_FlushRequest(0),
			m_bStarted(false),
			m_bStop(false),
			m_pLogFile(nullptr),
			m_pScratch(nullptr),
			m_ScratchSize(0),
			m_Records(0),
			m_Dropped(0),
			m_BlockedUs(0),
			m_MaxQueued(0),
			m_WriteErrors(0)
		{
		}

		bool Push(uint8 op, const char *a, uint32 aSize, const char *b, uint32 bSize, bool allowDrop);
		bool Flush(int timeoutMs);
		void Stop();
		void PrintStats();

	private:
		bool Start();
		void Run();
		void CopyIn(uint64 pos, const void *data, uint32 size);
		void CopyOut(uint64 pos, void *data, uint32 size);
		void Process(uint8 op, char *payload, uint32 size);

#ifdef _WIN32
		static DWORD WINAPI ThreadEntry(LPVOID param);
		HANDLE m_hThread;
#else
		static void *ThreadEntry(void *param);
		pthread_t m_Thread;
#endif

		CLock m_Lock;
		byte *m_pBuffer;
		uint64 m_Head;			// queued
		uint64 m_Tail;			// written
		uint64 m_Flushed;		// written and flushed
		uint64 m_FlushRequest;	// a flush is waited for up to this position
		bool m_bStarted;
		bool m_bStop;

		// Writer thread only
		FILE *m_pLogFile;
		char *m_pScratch;
		uint32 m_ScratchSize;

		// Statistics, under the lock
		uint64 m_Records;
		uint64 m_Dropped;
		uint64 m_BlockedUs;
		uint32 m_MaxQueued;
		uint64 m_WriteErrors;
	};

	CAsyncLogWriter s_Writer;

	// The log file is written by the writer thread
	bool s_bLogFileAsync = false;

	// Full path of the last file appended to (qconsole.log)
	char s_AppendRelative[MAX_PATH];
	char s_AppendFull[MAX_PATH];

	void Cmd_AsyncLogStats()
	{
		s_Writer.PrintStats();
	}

	// The path the file system resolves for writing, the file created if needed
	bool ResolveWritePath(const char *relativePath, const char *mode, char *fullPath, int size)
	{
		FileHandle_t file = FS_Open(relativePath, mode);
		if (!file)
			return false;

		FS_Close(file);
		return FS_GetLocalPath(relativePath, fullPath, size) != nullptr;
	}
}

bool CAsyncLogWriter::Start()
{
	m_pBuffer = (byte *)Mem_Malloc(QUEUE_SIZE);
	if (!m_pBuffer)
		return false;

#ifdef _WIN32
	m_hThread = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
	bool created = m_hThread != nullptr;
#else
	bool created = pthread_create(&m_Thread, nullptr, ThreadEntry, this) == 0;
#endif
	if (!created)
	{
		Mem_Free(m_pBuffer);
		m_pBuffer = nullptr;
		return false;
	}

	m_bStarted = true;
	return true;
}

void CAsyncLogWriter::Stop()
{
	if (!m_bStarted)
		return;

	m_Lock.Lock();
	m_bStop = true;
	m_Lock.WakeAll();
	m_Lock.Unlock();

#ifdef _WIN32
	WaitForSingleObject(m_hThread, INFINITE);
	CloseHandle(m_hThread);
#else
	pthread_join(m_Thread, nullptr);
#endif

	m_bStarted = false;
	m_bStop = false;
	Mem_Free(m_pBuffer);
	m_pBuffer = nullptr;
}

void CAsyncLogWriter::CopyIn(uint64 pos, const void *data, uint32 size)
{
	uint32 index = (uint32)(pos % QUEUE_SIZE);
	uint32 first = min(size, QUEUE_SIZE - index);
	Q_memcpy(m_pBuffer + index, data, first);
	Q_memcpy(m_pBuffer, (const byte *)data + first, size - first);
}

void CAsyncLogWriter::CopyOut(uint64 pos, void *data, uint32 size)
{
	uint32 index = (uint32)(pos % QUEUE_SIZE);
	uint32 first = min(size, QUEUE_SIZE - index);
	Q_memcpy(data, m_pBuffer + index, first);
	Q_memcpy((byte *)data + first, m_pBuffer, size - first);
}

bool CAsyncLogWriter::Push(uint8 op, const char *a, uint32 aSize, const char *b, uint32 bSize, bool allowDrop)
{
	record_header_t header;
	Q_memset(&header, 0, sizeof(header));
	header.op = op;
	header.size = aSize + bSize;

	uint32 total = sizeof(header) + header.size;
	if (total > QUEUE_SIZE)
		return false;

	if (!m_bStarted && !Start())
		return false;

	m_Lock.Lock();

	if (QUEUE_SIZE - (uint32)(m_Head - m_Tail) < total)
	{
		if (allowDrop)
		{
			m_Dropped++;
			m_Lock.Unlock();
			return false;
		}

		uint64 start = Sys_MonotonicNs();
		while (QUEUE_SIZE - (uint32)(m_Head - m_Tail) < total)
			m_Lock.Wait();
		m_BlockedUs += (Sys_MonotonicNs() - start) / 1000;
	}

	CopyIn(m_Head, &header, sizeof(header));
	CopyIn(m_Head + sizeof(header), a, aSize);
	if (bSize)
		CopyIn(m_Head + sizeof(header) + aSize, b, bSize);

	m_Head += total;
	m_Records++;
	if ((uint32)(m_Head - m_Tail) > m_MaxQueued)
		m_MaxQueued = (uint32)(m_Head - m_Tail);

	m_Lock.WakeAll();
	m_Lock.Unlock();
	return true;
}

bool CAsyncLogWriter::Flush(int timeoutMs)
{
	if (!m_bStarted)
		return true;

	uint64 deadline = Sys_MonotonicNs() + (uint64)timeoutMs * 1000000;

	m_Lock.Lock();
	uint64 target = m_Head;
	if (m_FlushRequest < target)
		m_FlushRequest = target;
	m_Lock.WakeAll();

	while (m_Flushed < target)
	{
		uint64 now = Sys_MonotonicNs();
		if (now >= deadline)
			break;

		m_Lock.WaitMs((int)((deadline - now) / 1000000) + 1);
	}

	bool flushed = m_Flushed >= target;
	m_Lock.Unlock();
	return flushed;
}

void CAsyncLogWriter::Process(uint8 op, char *payload, uint32 size)
{
	switch (op)
	{
	case OP_OPEN_LOG:
		if (m_pLogFile)
			fclose(m_pLogFile);
		m_pLogFile = fopen(payload, "at");
		if (!m_pLogFile)
			m_WriteErrors++;
		break;

	case OP_WRITE_LOG:
		if (!m_pLogFile || fwrite(payload, 1, size, m_pLogFile) != size)
			m_WriteErrors++;
		break;

	case OP_CLOSE_LOG:
		if (m_pLogFile)
		{
			fclose(m_pLogFile);
			m_pLogFile = nullptr;
		}
		break;

	case OP_APPEND:
	{
		uint32 pathSize = Q_strlen(payload) + 1;
		FILE *file = fopen(payload, "at");
		if (!file || fwrite(payload + pathSize, 1, size - pathSize, file) != size - pathSize)
			m_WriteErrors++;
		if (file)
			fclose(file);
		break;
	}
	}
}

void CAsyncLogWriter::Run()
{
#ifndef _WIN32
	pthread_setname_np(pthread_self(), "rehlds-log");
#endif

	m_Lock.Lock();
	for (;;)
	{
		while (m_Tail == m_Head && m_Flushed >= m_FlushRequest && !m_bStop)
			m_Lock.Wait();

		if (m_Tail == m_Head && m_bStop)
			break;

		uint64 head = m_Head;
		uint64 tail = m_Tail;
		m_Lock.Unlock();

		// The records between tail and head are the writer's until it moves m_Tail
		while (tail < head)
		{
			record_header_t header;
			CopyOut(tail, &header, sizeof(header));

			if (header.size + 1 > m_ScratchSize)
			{
				m_ScratchSize = header.size + 1;
				m_pScratch = (char *)Mem_Realloc(m_pScratch, m_ScratchSize);
			}
			CopyOut(tail + sizeof(header), m_pScratch, header.size);
			m_pScratch[header.size] = '\0';

			Process(header.op, m_pScratch, header.size);
			tail += sizeof(header) + header.size;

			m_Lock.Lock();
			m_Tail = tail;
			m_Lock.WakeAll();
			m_Lock.Unlock();
		}

		// Written out when the queue empties or a flush is waited for, so that the
		// file is up to date whenever the server is quiet
		m_Lock.Lock();
		if (m_Tail == m_Head || m_FlushRequest > m_Flushed)
		{
			m_Lock.Unlock();
			if (m_pLogFile)
				fflush(m_pLogFile);
			m_Lock.Lock();

			m_Flushed = tail;
			m_Lock.WakeAll();
		}
	}
	m_Lock.Unlock();

	if (m_pLogFile)
	{
		fclose(m_pLogFile);
		m_pLogFile = nullptr;
	}
}

void CAsyncLogWriter::PrintStats()
{
	m_Lock.Lock();
	uint32 queued = (uint32)(m_Head - m_Tail);
	uint64 records = m_Records, dropped = m_Dropped, blockedUs = m_BlockedUs, errors = m_WriteErrors;
	uint32 maxQueued = m_MaxQueued;
	m_Lock.Unlock();

	Con_Printf("sv_log_async %s, writer thread %s, log file %s\n", sv_log_async.string,
		m_bStarted ? "running" : "not started", s_bLogFileAsync ? "written asynchronously" : "written synchronously");
	Con_Printf("  records %llu, queued now %u bytes, max %u of %u bytes\n",
		(unsigned long long)records, queued, maxQueued, QUEUE_SIZE);
	Con_Printf("  dropped %llu, main thread blocked %.1f ms in all, write errors %llu\n",
		(unsigned long long)dropped, blockedUs / 1000.0, (unsigned long long)errors);
}

#ifdef _WIN32
DWORD WINAPI CAsyncLogWriter::ThreadEntry(LPVOID param)
#else
void *CAsyncLogWriter::ThreadEntry(void *param)
#endif
{
	((CAsyncLogWriter *)param)->Run();
	return 0;
}

void AsyncLog_Init()
{
	Cvar_RegisterFeatureVariable(&sv_log_async);
	Cvar_RegisterVariable(&sv_log_async_overflow);
	Cmd_AddCommand("sv_log_async_stats", Cmd_AsyncLogStats);
}

void AsyncLog_Shutdown()
{
	AsyncLog_Flush(10000);
	s_Writer.Stop();
	s_bLogFileAsync = false;
}

bool AsyncLog_BeginFile(const char *relativePath)
{
	s_bLogFileAsync = false;
	if (sv_log_async.value == 0.0f)
		return false;

	char fullPath[MAX_PATH];
	if (!FS_GetLocalPath(relativePath, fullPath, sizeof(fullPath)))
	{
		Con_Printf("sv_log_async: cannot find the path of %s, writing it on the main thread\n", relativePath);
		return false;
	}

	return AsyncLog_BeginFileAt(fullPath);
}

bool AsyncLog_BeginFileAt(const char *fullPath)
{
	s_bLogFileAsync = s_Writer.Push(OP_OPEN_LOG, fullPath, Q_strlen(fullPath) + 1, nullptr, 0, false);
	return s_bLogFileAsync;
}

void AsyncLog_WriteFile(const char *text)
{
	bool allowDrop = !Q_stricmp(sv_log_async_overflow.string, "drop");
	s_Writer.Push(OP_WRITE_LOG, text, Q_strlen(text), nullptr, 0, allowDrop);
}

void AsyncLog_EndFile()
{
	if (!s_bLogFileAsync)
		return;

	s_Writer.Push(OP_CLOSE_LOG, "", 0, nullptr, 0, false);
	AsyncLog_Flush(10000);
	s_bLogFileAsync = false;
}

void AsyncLog_Append(const char *relativePath, const char *text)
{
	if (Q_strcmp(relativePath, s_AppendRelative))
	{
		if (!ResolveWritePath(relativePath, "at", s_AppendFull, sizeof(s_AppendFull)))
		{
			s_AppendRelative[0] = '\0';
			return;
		}

		Q_strlcpy(s_AppendRelative, relativePath);
	}

	AsyncLog_AppendAt(s_AppendFull, text);
}

void AsyncLog_AppendAt(const char *fullPath, const char *text)
{
	bool allowDrop = !Q_stricmp(sv_log_async_overflow.string, "drop");
	s_Writer.Push(OP_APPEND, fullPath, Q_strlen(fullPath) + 1, text, Q_strlen(text), allowDrop);
}

bool AsyncLog_Flush(int timeoutMs)
{
	return s_Writer.Flush(timeoutMs);
}

bool AsyncLog_IsFileAsync()
{
	return s_bLogFileAsync;
}
