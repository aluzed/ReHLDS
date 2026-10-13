#pragma once

#include "maintypes.h"

// File transfer compression on a thread (MT-601). The first time a client downloads a
// file through the game connection, the engine compresses the whole file with bzip2
// (sv_filetransfercompression 1) and keeps the result next to it (<file>.ztmp) for the
// next downloads. On the main thread, that took 0.5 to 0.75 s for a 6 MB map: every
// player froze.
//
// With sv_filetransfer_async 1, the main thread hands the file to a compression thread
// and the download waits in the client's queue until the .ztmp is ready; the other
// players are not held up. The thread reads the file and writes the .ztmp with the C
// library, from full paths the main thread resolved through the file system, and
// renames it into place once it is complete, so a half-written .ztmp is never sent.
//
// A file that does not compress (the result is not smaller) is sent uncompressed, as
// before. A file the thread cannot reach (in a pack file, no free slot) is compressed
// on the main thread, as before.

extern cvar_t sv_filetransfer_async;

enum FileCompression
{
	FILE_COMPRESSION_SYNC,		// compress it on the main thread, as before
	FILE_COMPRESSION_PENDING,	// being compressed: try again later
	FILE_COMPRESSION_DONE,		// the .ztmp is ready
	FILE_COMPRESSION_FAILED,	// send the file uncompressed
};

void FileCompressor_Init();
void FileCompressor_Shutdown();

// Main thread. Whether the file is being compressed, without touching the file system
bool FileCompressor_IsPending(const char *filename);

// Main thread. The state of the file's compression, the compression started when it
// has no up to date .ztmp. sourceTime is the file's time (FS_GetFileTime)
FileCompression FileCompressor_Request(const char *filename, const char *compressedFilename, int sourceTime, unsigned int maxSize);
