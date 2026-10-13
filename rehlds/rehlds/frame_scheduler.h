#pragma once

#include "archtypes.h"

// Paces dedicated server frames on a fixed grid of absolute deadlines
// (one every 1 / sys_ticrate seconds), instead of the launcher's
// "sleep 1 ms, then let Host_FilterTime reject frames that are too early".
//
// The launcher sleeps about 1 ms before every RunFrame() call, which alone is
// longer than a 1000 Hz tick. To reach the target rate with any launcher,
// RunFrame() keeps control for a time slice (sys_scheduler_slice) and runs
// several frames, sleeping precisely between them, before returning to the
// launcher for console input:
//
//	g_FrameScheduler.BeginSlice();
//	while (g_FrameScheduler.WaitForNextTick())
//		eng->Frame();
//	g_FrameScheduler.EndSlice();
class CFrameScheduler
{
public:
	CFrameScheduler();

	void Init();
	void Shutdown();

	// True when sys_scheduler is on and the tick rate is limited
	bool IsEnabled() const;

	// True while frames are being paced, between BeginSlice and EndSlice
	bool IsActive() const { return m_bInSlice; }

	void BeginSlice();
	void EndSlice();

	// Waits for the next tick deadline and returns true, or returns false when
	// the slice is over and control must go back to the launcher.
	bool WaitForNextTick();

private:
	void WaitUntil(uint64 deadline);
	// sys_wake_on_packet (MT-102): reads the packets the network thread queues
	// until the deadline, as they arrive, instead of at the next tick
	void ReadPacketsUntil(uint64 deadline);
	void SleepUntil(uint64 deadline);
	void ApplyProcessSettings();

	bool m_bInSlice;
	bool m_bProcessSettingsApplied;
	int m_FramesInSlice;
	uint64 m_SliceEnd;

	// Next tick deadline on the grid (Sys_MonotonicNs time), 0 when not started
	uint64 m_NextTick;
	uint64 m_Period;

#ifdef _WIN32
	HANDLE m_hTimer;
	bool m_bTimerPeriodSet;
#endif
};

extern CFrameScheduler g_FrameScheduler;

void FrameScheduler_Init();
void FrameScheduler_Shutdown();
