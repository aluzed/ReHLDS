#include "precompiled.h"

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <errno.h>
#include <sys/prctl.h>
#endif

cvar_t sys_scheduler = { "sys_scheduler", "1", 0, 0.0f, nullptr };
cvar_t sys_scheduler_slice = { "sys_scheduler_slice", "100", 0, 100.0f, nullptr };
cvar_t sys_spin_us = { "sys_spin_us", "0", 0, 0.0f, nullptr };
cvar_t sys_wake_on_packet = { "sys_wake_on_packet", "0", 0, 0.0f, nullptr };

CFrameScheduler g_FrameScheduler;

namespace
{
	// When the server falls behind the grid by more than this many ticks (map
	// load, stalled process), the backlog is dropped and the grid restarts from
	// now, instead of running the missed frames back to back.
	const uint64 MAX_LATE_TICKS = 2;

	const float MIN_SLICE_MS = 1.0f;
	const float MAX_SLICE_MS = 1000.0f;
	const float MAX_SPIN_US = 2000.0f;

	float ClampValue(float value, float min, float max)
	{
		if (value < min)
			return min;

		if (value > max)
			return max;

		return value;
	}

#ifdef _WIN32
	typedef UINT (WINAPI *TimePeriodFn)(UINT);

	// winmm is loaded dynamically so that the engine does not need to link it
	bool SetTimerPeriod(bool begin)
	{
		HMODULE winmm = LoadLibraryA("winmm.dll");
		if (!winmm)
			return false;

		TimePeriodFn fn = (TimePeriodFn)GetProcAddress(winmm, begin ? "timeBeginPeriod" : "timeEndPeriod");
		bool ok = fn && fn(1) == 0; // TIMERR_NOERROR
		FreeLibrary(winmm);
		return ok;
	}
#endif
}

CFrameScheduler::CFrameScheduler()
{
	m_bInSlice = false;
	m_bProcessSettingsApplied = false;
	m_FramesInSlice = 0;
	m_SliceEnd = 0;
	m_NextTick = 0;
	m_Period = 0;

#ifdef _WIN32
	m_hTimer = nullptr;
	m_bTimerPeriodSet = false;
#endif
}

void CFrameScheduler::Init()
{
	Cvar_RegisterFeatureVariable(&sys_scheduler);
	Cvar_RegisterVariable(&sys_scheduler_slice);
	Cvar_RegisterVariable(&sys_spin_us);
	Cvar_RegisterVariable(&sys_wake_on_packet);
}

void CFrameScheduler::Shutdown()
{
#ifdef _WIN32
	if (m_hTimer)
	{
		CloseHandle(m_hTimer);
		m_hTimer = nullptr;
	}

	if (m_bTimerPeriodSet)
	{
		SetTimerPeriod(false);
		m_bTimerPeriodSet = false;
	}
#endif

	m_bProcessSettingsApplied = false;
}

bool CFrameScheduler::IsEnabled() const
{
	return sys_scheduler.value != 0.0f && Host_GetTicrate() > 0.0f;
}

void CFrameScheduler::ApplyProcessSettings()
{
	if (m_bProcessSettingsApplied)
		return;

	m_bProcessSettingsApplied = true;

#ifdef _WIN32
	m_hTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!m_hTimer)
	{
		// Before Windows 10 1803: raise the system timer resolution to 1 ms instead
		m_bTimerPeriodSet = SetTimerPeriod(true);
		Con_DPrintf("%s: high resolution timer unavailable, timer period 1 ms %s\n",
			__func__, m_bTimerPeriodSet ? "set" : "not set");
	}
#else
	// The default 50 us timer slack would add that much to every sleep
	prctl(PR_SET_TIMERSLACK, 1);
#endif
}

void CFrameScheduler::BeginSlice()
{
	ApplyProcessSettings();

	uint64 period = (uint64)(1000000000.0 / Host_GetTicrate());
	if (period != m_Period)
	{
		// New tick rate: restart the grid
		m_Period = period;
		m_NextTick = 0;
	}

	float sliceMs = ClampValue(sys_scheduler_slice.value, MIN_SLICE_MS, MAX_SLICE_MS);
	m_SliceEnd = Sys_MonotonicNs() + (uint64)(sliceMs * 1000000.0f);
	m_FramesInSlice = 0;
	m_bInSlice = true;
}

void CFrameScheduler::EndSlice()
{
	m_bInSlice = false;
}

bool CFrameScheduler::WaitForNextTick()
{
	// Return to the launcher once the next deadline is past the slice: at tick
	// rates below ~1000 Hz the launcher then sleeps inside the gap between ticks.
	if (m_FramesInSlice > 0 && (m_NextTick >= m_SliceEnd || sys_scheduler.value == 0.0f))
		return false;

	uint64 now = Sys_MonotonicNs();
	if (m_NextTick == 0 || now > m_NextTick + MAX_LATE_TICKS * m_Period)
		m_NextTick = now;

	WaitUntil(m_NextTick);

	m_NextTick += m_Period;
	m_FramesInSlice++;
	return true;
}

void CFrameScheduler::WaitUntil(uint64 deadline)
{
	// Optionally sleep until a bit before the deadline and spin for the rest,
	// for operators who dedicate a core to the server
	uint64 spin = (uint64)(ClampValue(sys_spin_us.value, 0.0f, MAX_SPIN_US) * 1000.0f);

	if (deadline > spin)
	{
		if (sys_wake_on_packet.value != 0.0f && g_NetReceiveThread.IsRunning())
			ReadPacketsUntil(deadline - spin);

		SleepUntil(deadline - spin);
	}

	while (Sys_MonotonicNs() < deadline)
		_mm_pause();
}

void CFrameScheduler::ReadPacketsUntil(uint64 deadline)
{
	for (;;)
	{
		if (g_NetReceiveThread.HasPackets())
		{
			// No server running (between maps): the queue waits for the next frame
			if (!SV_ReadPacketsBetweenFrames())
				return;

			continue;
		}

		uint64 now = Sys_MonotonicNs();
		if (now >= deadline)
			return;

#ifdef _WIN32
		// The wake event has a millisecond timeout: the high resolution timer of
		// SleepUntil takes the last one
		if (deadline - now < 1000000)
			return;
#endif

		g_NetReceiveThread.WaitForPacket(deadline);
	}
}

void CFrameScheduler::SleepUntil(uint64 deadline)
{
#ifdef _WIN32
	uint64 now = Sys_MonotonicNs();
	if (deadline <= now)
		return;

	if (m_hTimer)
	{
		// Relative due time in 100 ns units
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)((deadline - now) / 100);

		if (due.QuadPart < 0 && SetWaitableTimer(m_hTimer, &due, 0, nullptr, nullptr, FALSE))
			WaitForSingleObject(m_hTimer, INFINITE);

		return;
	}

	// Coarse fallback: 1 ms sleeps while far from the deadline, WaitUntil spins the rest
	const uint64 margin = 2000000;
	while (Sys_MonotonicNs() + margin < deadline)
		Sleep(1);
#else
	struct timespec ts;
	ts.tv_sec = (time_t)(deadline / 1000000000ull);
	ts.tv_nsec = (long)(deadline % 1000000000ull);

	// clock_nanosleep returns the error number instead of setting errno
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR)
		;
#endif
}

void FrameScheduler_Init()
{
	g_FrameScheduler.Init();
}

void FrameScheduler_Shutdown()
{
	g_FrameScheduler.Shutdown();
}
