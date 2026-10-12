#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#include <string>

namespace
{
	std::string ReadFile(const char *path)
	{
		std::string content;
		FILE *file = fopen(path, "rb");
		if (!file)
			return content;

		char buf[65536];
		size_t read;
		while ((read = fread(buf, 1, sizeof(buf), file)) > 0)
			content.append(buf, read);

		fclose(file);
		return content;
	}

	void TestPath(char *path, int size, const char *name)
	{
#ifdef _WIN32
		char dir[MAX_PATH];
		GetTempPathA(sizeof(dir), dir);
		Q_snprintf(path, size, "%s%s", dir, name);
#else
		Q_snprintf(path, size, "/tmp/%s.%d", name, (int)getpid());
#endif
		remove(path);
	}
}

// More lines than the queue holds, so that it wraps and the main thread waits for
// room: every line is written, in order, and is on disk once the file is closed
TEST(LinesInOrder, AsyncLog, 10000)
{
	EngineInitializer engInitGuard;
	sv_log_async_overflow.string = (char *)"block";

	char logPath[MAX_PATH], appendPath[MAX_PATH];
	TestPath(logPath, sizeof(logPath), "rehlds_async_log_test.log");
	TestPath(appendPath, sizeof(appendPath), "rehlds_async_append_test.log");

	FILE *created = fopen(logPath, "wt");
	CHECK("Cannot create the test log", created != nullptr);
	fclose(created);

	CHECK("Cannot start the log file", AsyncLog_BeginFileAt(logPath));
	CHECK("Log file not asynchronous", AsyncLog_IsFileAsync());

	std::string expectedLog, expectedAppend;
	char line[256];
	for (int i = 0; i < 100000; i++)
	{
		Q_snprintf(line, sizeof(line), "L 10/13/2026 - 12:00:00: line %d with some padding to fill the queue faster\n", i);
		AsyncLog_WriteFile(line);
		expectedLog += line;

		if (i % 1000 == 0)
		{
			Q_snprintf(line, sizeof(line), "console %d\n", i);
			AsyncLog_AppendAt(appendPath, line);
			expectedAppend += line;
		}
	}

	AsyncLog_EndFile();
	CHECK("Log file still asynchronous after its end", !AsyncLog_IsFileAsync());
	CHECK("Log file differs from what was written", ReadFile(logPath) == expectedLog);
	CHECK("Appended file differs from what was written", ReadFile(appendPath) == expectedAppend);

	// Flush waits for the appended lines too
	AsyncLog_AppendAt(appendPath, "last\n");
	CHECK("Flush timed out", AsyncLog_Flush(5000));
	CHECK("Last appended line missing after a flush", ReadFile(appendPath) == expectedAppend + "last\n");

	AsyncLog_Shutdown();
	remove(logPath);
	remove(appendPath);
}
