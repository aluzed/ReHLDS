#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#ifndef _WIN32

TEST(FloatTimeMonotonic, Sys, 1000)
{
	double prev = Sys_FloatTime();
	double smallestStep = 1.0;

	for (int i = 0; i < 100000; i++)
	{
		double now = Sys_FloatTime();
		CHECK("Sys_FloatTime went backwards", now >= prev);

		if (now > prev && now - prev < smallestStep)
			smallestStep = now - prev;

		prev = now;
	}

	// The frame scheduler and the profiler rely on sub-microsecond resolution
	CHECK("Sys_FloatTime resolution is coarser than 1 us", smallestStep < 0.000001);
}

TEST(FloatTimeMatchesMonotonicClock, Sys, 1000)
{
	double start = Sys_FloatTime();
	uint64 startNs = Sys_MonotonicNs();

	struct timespec delay = { 0, 20000000 }; // 20 ms
	nanosleep(&delay, nullptr);

	double elapsed = Sys_FloatTime() - start;
	double elapsedNs = (double)(Sys_MonotonicNs() - startNs) * 0.000000001;

	DOUBLES_EQUAL("Sys_FloatTime and Sys_MonotonicNs disagree", elapsedNs, elapsed, 0.0005);
	CHECK("Sys_FloatTime did not advance by the sleep duration", elapsed >= 0.020);
}

#endif // _WIN32
