#pragma once

// Asynchronous log writer (MT-301). With sv_log_async 1, the server log file
// (Log_Printf) and qconsole.log (-condebug) are written by a dedicated thread: the
// main thread formats the line, timestamp included, and queues it, so that a slow disk,
// a network file system or a full page cache never stalls a frame.
//
// The queue is bounded (4 MB). When it is full, sv_log_async_overflow "block" (default)
// makes the main thread wait for room, "drop" drops the line and counts it.
//
// Lines are written in the order they were queued, per file and across files. The
// queue is written out and the files flushed whenever it empties, on Log_Close (map
// change, "log off", quit) and on Sys_Error.
//
// The log file is opened by the main thread as before (name, creation, path); the
// writer thread appends to it through its own handle. Whether a log file is written
// asynchronously is decided when it is opened: changing sv_log_async takes effect at
// the next log file (map change or "log on").

extern cvar_t sv_log_async;
extern cvar_t sv_log_async_overflow;

void AsyncLog_Init();
void AsyncLog_Shutdown();

// Log file. Begin returns false when the file cannot be written asynchronously (its
// path cannot be found), the caller then writes it synchronously
bool AsyncLog_BeginFile(const char *relativePath);
void AsyncLog_WriteFile(const char *text);
// Writes out what is queued and closes the writer's handle
void AsyncLog_EndFile();

// The current log file is written by the writer thread
bool AsyncLog_IsFileAsync();

// Appends text to a file, opened and closed around the write as Con_DebugLog does
void AsyncLog_Append(const char *relativePath, const char *text);

// Waits until everything queued is written and flushed, at most timeoutMs
// milliseconds (the disk may be stuck). Returns whether it was
bool AsyncLog_Flush(int timeoutMs);

// The same with a full path, already resolved (unit tests)
bool AsyncLog_BeginFileAt(const char *fullPath);
void AsyncLog_AppendAt(const char *fullPath, const char *text);
