// Benchmark only: simulates a slow device for the server log files (MT-301).
// Loaded with LD_PRELOAD, it wraps the files opened with fopen whose path contains
// "/logs/" or ends with "qconsole.log" in a stdio stream that sleeps
// SLOW_LOG_WRITE_MS milliseconds before each write to the file (each time the stdio
// buffer is written out). Whoever writes the log, the main thread or the log writer
// thread, waits as it would on a slow disk or network file system.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ssize_t slow_write(void *cookie, const char *buf, size_t size)
{
	static long delayMs = -1;
	if (delayMs < 0)
	{
		const char *env = getenv("SLOW_LOG_WRITE_MS");
		delayMs = env ? atol(env) : 0;
	}

	if (delayMs > 0)
	{
		struct timespec delay = { delayMs / 1000, (delayMs % 1000) * 1000000L };
		nanosleep(&delay, NULL);
	}

	if (getenv("SLOW_LOG_DEBUG"))
		fprintf(stderr, "slow_log_write: %zu bytes\n", size);

	return write((int)(long)cookie, buf, size);
}

static ssize_t slow_read(void *cookie, char *buf, size_t size)
{
	return read((int)(long)cookie, buf, size);
}

static int slow_seek(void *cookie, off64_t *offset, int whence)
{
	off64_t result = lseek64((int)(long)cookie, *offset, whence);
	if (result < 0)
		return -1;

	*offset = result;
	return 0;
}

static int slow_close(void *cookie)
{
	return close((int)(long)cookie);
}

static int is_log_file(const char *path)
{
	size_t length = strlen(path);
	return strstr(path, "/logs/") != NULL
		|| (length >= 12 && !strcmp(path + length - 12, "qconsole.log"));
}

static FILE *open_slow(const char *path, const char *mode)
{
	int flags;
	if (mode[0] == 'w')
		flags = O_WRONLY | O_CREAT | O_TRUNC;
	else if (mode[0] == 'a')
		flags = O_WRONLY | O_CREAT | O_APPEND;
	else
		return NULL;

	if (strchr(mode, '+'))
		flags = (flags & ~O_WRONLY) | O_RDWR;

	int fd = open(path, flags | O_LARGEFILE, 0644);
	if (fd < 0)
		return NULL;

	cookie_io_functions_t functions = { slow_read, slow_write, slow_seek, slow_close };
	FILE *file = fopencookie((void *)(long)fd, mode, functions);
	if (!file)
		close(fd);

	return file;
}

#define WRAP_FOPEN(name)                                                         \
	FILE *name(const char *path, const char *mode)                               \
	{                                                                            \
		static FILE *(*real)(const char *, const char *);                        \
		if (!real)                                                               \
			real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, #name); \
		if (path && getenv("SLOW_LOG_DEBUG"))                                    \
			fprintf(stderr, "slow_log_write: " #name " %s %s\n", path, mode);   \
		if (path && mode && mode[0] != 'r' && is_log_file(path))                 \
			return open_slow(path, mode);                                        \
		return real(path, mode);                                                 \
	}

WRAP_FOPEN(fopen)
WRAP_FOPEN(fopen64)
