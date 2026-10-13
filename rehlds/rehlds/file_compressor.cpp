#include "precompiled.h"
#include "threading/native_lock.h"
#include "monotonic_clock.h"

#include <atomic>

cvar_t sv_filetransfer_async = { "sv_filetransfer_async", "0", 0, 0.0f, nullptr };

namespace
{
	// Files compressed during a server's life: the maps and resources clients download
	const int MAX_FILES = 64;

	struct compression_t
	{
		bool used;
		bool taken;				// the thread works on it
		FileCompression state;
		int sourceTime;
		unsigned int maxSize;
		uint64 lastUse;
		char filename[MAX_PATH];
		char sourcePath[MAX_PATH];
		char partPath[MAX_PATH];
		char finalPath[MAX_PATH];
	};

	class CFileCompressor
	{
	public:
		CFileCompressor() :
			m_bStarted(false),
			m_bStop(false),
			m_Pending(0),
			m_UseCounter(0),
			m_Compressed(0),
			m_Failed(0),
			m_TotalMs(0),
			m_MaxMs(0)
		{
			Q_memset(m_Files, 0, sizeof(m_Files));
		}

		bool IsPending(const char *filename);
		FileCompression Request(const char *filename, const char *compressedFilename, int sourceTime, unsigned int maxSize);
		void Stop();
		void PrintStats();

	private:
		bool Start();
		void Run();
		compression_t *Find(const char *filename);
		compression_t *Allocate();
		static bool Compress(const compression_t &job);

#ifdef _WIN32
		static DWORD WINAPI ThreadEntry(LPVOID param);
		HANDLE m_hThread;
#else
		static void *ThreadEntry(void *param);
		pthread_t m_Thread;
#endif

		CNativeLock m_Lock;
		compression_t m_Files[MAX_FILES];
		bool m_bStarted;
		bool m_bStop;

		// Files waiting or being compressed, read without the lock by IsPending
		std::atomic<int> m_Pending;
		uint64 m_UseCounter;

		// Statistics, under the lock
		uint32 m_Compressed;
		uint32 m_Failed;
		uint64 m_TotalMs;
		uint32 m_MaxMs;
	};

	CFileCompressor s_Compressor;

	// The path the file system resolves for writing, the file created if needed
	bool ResolveWritePath(const char *relativePath, char *fullPath, int size)
	{
		FileHandle_t file = FS_Open(relativePath, "wb");
		if (!file)
			return false;

		FS_Close(file);
		return FS_GetLocalPath(relativePath, fullPath, size) != nullptr;
	}

	void Cmd_FileTransferAsyncStats()
	{
		s_Compressor.PrintStats();
	}
}

compression_t *CFileCompressor::Find(const char *filename)
{
	for (auto &file : m_Files)
	{
		if (file.used && !Q_stricmp(file.filename, filename))
			return &file;
	}

	return nullptr;
}

// A free slot, or the least recently used finished one
compression_t *CFileCompressor::Allocate()
{
	compression_t *oldest = nullptr;
	for (auto &file : m_Files)
	{
		if (!file.used)
			return &file;

		if (file.state != FILE_COMPRESSION_PENDING && (!oldest || file.lastUse < oldest->lastUse))
			oldest = &file;
	}

	return oldest;
}

bool CFileCompressor::IsPending(const char *filename)
{
	if (m_Pending.load(std::memory_order_acquire) == 0)
		return false;

	m_Lock.Lock();
	compression_t *file = Find(filename);
	bool pending = file && file->state == FILE_COMPRESSION_PENDING;
	m_Lock.Unlock();
	return pending;
}

FileCompression CFileCompressor::Request(const char *filename, const char *compressedFilename, int sourceTime, unsigned int maxSize)
{
	m_Lock.Lock();
	compression_t *file = Find(filename);
	if (file && file->sourceTime == sourceTime)
	{
		FileCompression state = file->state;
		file->lastUse = ++m_UseCounter;
		m_Lock.Unlock();

		if (state == FILE_COMPRESSION_PENDING || state == FILE_COMPRESSION_FAILED)
			return state;

		// Done, unless the .ztmp was deleted since
		if (FS_GetFileTime(compressedFilename) >= sourceTime)
			return FILE_COMPRESSION_DONE;
	}
	else
	{
		m_Lock.Unlock();
	}

	if (!m_bStarted && !Start())
		return FILE_COMPRESSION_SYNC;

	// The thread reads and writes the files directly: they must be plain files
	char sourcePath[MAX_PATH];
	if (!FS_GetLocalPath(filename, sourcePath, sizeof(sourcePath)))
		return FILE_COMPRESSION_SYNC;

	char partName[MAX_PATH];
	Q_snprintf(partName, sizeof(partName), "%s.part", compressedFilename);
	char partPath[MAX_PATH];
	if (!ResolveWritePath(partName, partPath, sizeof(partPath)))
		return FILE_COMPRESSION_SYNC;

	// The .ztmp goes next to the .part
	int partLength = Q_strlen(partPath);
	if (partLength <= 5)
		return FILE_COMPRESSION_SYNC;

	m_Lock.Lock();
	file = Find(filename);
	if (!file)
		file = Allocate();

	// Every slot is being compressed
	if (!file)
	{
		m_Lock.Unlock();
		return FILE_COMPRESSION_SYNC;
	}

	// The file changed while it was being compressed: wait, it is compressed again once done
	if (file->state == FILE_COMPRESSION_PENDING)
	{
		m_Lock.Unlock();
		return FILE_COMPRESSION_PENDING;
	}

	file->used = true;
	file->taken = false;
	file->state = FILE_COMPRESSION_PENDING;
	file->sourceTime = sourceTime;
	file->maxSize = maxSize;
	file->lastUse = ++m_UseCounter;
	Q_strlcpy(file->filename, filename);
	Q_strlcpy(file->sourcePath, sourcePath);
	Q_strlcpy(file->partPath, partPath);
	Q_strlcpy(file->finalPath, partPath);
	file->finalPath[partLength - 5] = '\0';
	m_Pending.fetch_add(1, std::memory_order_release);
	m_Lock.WakeAll();
	m_Lock.Unlock();

	Con_DPrintf("Compressing %s for transfer on the compression thread\n", filename);
	return FILE_COMPRESSION_PENDING;
}

bool CFileCompressor::Start()
{
#ifdef _WIN32
	m_hThread = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
	m_bStarted = m_hThread != nullptr;
#else
	m_bStarted = pthread_create(&m_Thread, nullptr, ThreadEntry, this) == 0;
#endif
	if (!m_bStarted)
		Con_Printf("WARNING: cannot create the file compression thread, compressing on the main thread\n");

	return m_bStarted;
}

void CFileCompressor::Stop()
{
	if (!m_bStarted)
		return;

	m_Lock.Lock();
	m_bStop = true;
	m_Lock.WakeAll();
	m_Lock.Unlock();

	// Waits for the file being compressed, if any (under a second)
#ifdef _WIN32
	WaitForSingleObject(m_hThread, INFINITE);
	CloseHandle(m_hThread);
#else
	pthread_join(m_Thread, nullptr);
#endif

	m_bStarted = false;
	m_bStop = false;
}

#ifdef _WIN32
DWORD WINAPI CFileCompressor::ThreadEntry(LPVOID param)
{
	((CFileCompressor *)param)->Run();
	return 0;
}
#else
void *CFileCompressor::ThreadEntry(void *param)
{
	((CFileCompressor *)param)->Run();
	return nullptr;
}
#endif

void CFileCompressor::Run()
{
	m_Lock.Lock();
	while (!m_bStop)
	{
		compression_t *file = nullptr;
		for (auto &candidate : m_Files)
		{
			if (candidate.used && candidate.state == FILE_COMPRESSION_PENDING && !candidate.taken)
			{
				file = &candidate;
				break;
			}
		}

		if (!file)
		{
			m_Lock.Wait();
			continue;
		}

		// Slots are only reused once finished: the job stays valid without the lock
		file->taken = true;
		m_Lock.Unlock();

		uint64 start = Sys_MonotonicNs();
		bool compressed = Compress(*file);
		uint32 ms = (uint32)((Sys_MonotonicNs() - start) / 1000000);

		m_Lock.Lock();
		file->state = compressed ? FILE_COMPRESSION_DONE : FILE_COMPRESSION_FAILED;
		file->taken = false;
		m_Pending.fetch_sub(1, std::memory_order_release);
		if (compressed)
		{
			m_Compressed++;
			m_TotalMs += ms;
			if (ms > m_MaxMs)
				m_MaxMs = ms;
		}
		else
		{
			m_Failed++;
		}
	}
	m_Lock.Unlock();
}

// Compression thread: the same bzip2 settings as Netchan_CreateFileFragments
bool CFileCompressor::Compress(const compression_t &job)
{
	FILE *source = fopen(job.sourcePath, "rb");
	if (!source)
		return false;

	fseek(source, 0, SEEK_END);
	long size = ftell(source);
	fseek(source, 0, SEEK_SET);
	if (size <= 0 || (unsigned long)size > job.maxSize)
	{
		fclose(source);
		return false;
	}

	char *uncompressed = (char *)malloc(size);
	char *compressed = (char *)malloc(size);
	bool ok = uncompressed && compressed && fread(uncompressed, 1, size, source) == (size_t)size;
	fclose(source);

	// Not smaller: BZ_OUTBUFF_FULL, the file is sent uncompressed
	unsigned int compressedSize = (unsigned int)size;
	ok = ok && BZ2_bzBuffToBuffCompress(compressed, &compressedSize, uncompressed, (unsigned int)size, 9, 0, 30) == BZ_OK;
	free(uncompressed);

	if (ok)
	{
		FILE *part = fopen(job.partPath, "wb");
		ok = part && fwrite(compressed, 1, compressedSize, part) == compressedSize;
		if (part && fclose(part) != 0)
			ok = false;
	}
	free(compressed);

	if (ok)
	{
#ifdef _WIN32
		ok = MoveFileExA(job.partPath, job.finalPath, MOVEFILE_REPLACE_EXISTING) != FALSE;
#else
		ok = rename(job.partPath, job.finalPath) == 0;
#endif
	}

	if (!ok)
		remove(job.partPath);

	return ok;
}

void CFileCompressor::PrintStats()
{
	m_Lock.Lock();
	uint32 compressed = m_Compressed, failed = m_Failed, maxMs = m_MaxMs;
	uint64 totalMs = m_TotalMs;
	int pending = m_Pending.load();
	m_Lock.Unlock();

	Con_Printf("sv_filetransfer_async %d, compression thread %s\n", (int)sv_filetransfer_async.value, m_bStarted ? "running" : "not started");
	Con_Printf("  files compressed %u (average %u ms, max %u ms), not compressible or failed %u, pending %d\n",
		compressed, compressed ? (uint32)(totalMs / compressed) : 0, maxMs, failed, pending);
}

void FileCompressor_Init()
{
	Cvar_RegisterVariable(&sv_filetransfer_async);
	Cmd_AddCommand("sv_filetransfer_async_stats", Cmd_FileTransferAsyncStats);
}

void FileCompressor_Shutdown()
{
	s_Compressor.Stop();
}

bool FileCompressor_IsPending(const char *filename)
{
	return s_Compressor.IsPending(filename);
}

FileCompression FileCompressor_Request(const char *filename, const char *compressedFilename, int sourceTime, unsigned int maxSize)
{
	return s_Compressor.Request(filename, compressedFilename, sourceTime, maxSize);
}
