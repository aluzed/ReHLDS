#pragma once

#include "archtypes.h"

#ifndef _WIN32
#include <time.h>
#endif

// Monotonic clock in nanoseconds, cheap enough to be read many times per frame.
// Independent from Sys_FloatTime, which takes a lock on Windows.
inline uint64 Sys_MonotonicNs()
{
#ifdef _WIN32
	static LARGE_INTEGER freq = {};
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);

	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);

	// Split the conversion so that counter * 1e9 cannot overflow
	uint64 ticks = (uint64)counter.QuadPart;
	uint64 rate = (uint64)freq.QuadPart;
	return (ticks / rate) * 1000000000ull + (ticks % rate) * 1000000000ull / rate;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64)ts.tv_sec * 1000000000ull + (uint64)ts.tv_nsec;
#endif
}
