#pragma once

#include "archtypes.h"
#include "monotonic_clock.h"

// Timed sections of a server frame. A phase may be entered several times per
// frame (e.g. once per client): its durations are summed for the frame.
enum FrameProfilerPhase
{
	FPP_FRAME,                // accepted host frame, from Host_FilterTime to the end of _Host_Frame
	FPP_CBUF,                 // Cbuf_Execute
	FPP_SV_FRAME,             // SV_Frame, including SV_Frame hookchain subscribers
	FPP_READ_PACKETS,         // SV_ReadPackets
	FPP_CONNECTIONLESS,       // connectionless packets (queries, challenges, connects)
	FPP_NETCHAN_PROCESS,      // Netchan_Process and fragment reassembly
	FPP_EXECUTE_CLIENT_MSG,   // SV_ExecuteClientMessage (game DLL: commands, movement)
	FPP_PHYSICS,              // SV_Physics (game DLL: StartFrame, Think, Touch)
	FPP_CHECK_TIMEOUTS,       // SV_CheckTimeouts
	FPP_SEND_MESSAGES,        // SV_SendClientMessages
	FPP_SEND_DATAGRAM,        // datagram build for all clients (gather + encode)
	FPP_SEND_GATHER,          // game DLL calls: SetupVisibility, AddToFullPack, UpdateClientData, GetWeaponData
	FPP_SEND_ENCODE,          // derived: FPP_SEND_DATAGRAM - FPP_SEND_GATHER
	FPP_SEND_TRANSMIT,        // Netchan_Transmit for spawned clients
	FPP_STEAM,                // Steam_RunFrame
	FPP_LOG_WRITE,            // log file writes in Log_Printf

	FPP_COUNT
};

// Per-frame counters, summed over the frame (including rejected wake-ups before it).
enum FrameProfilerCounter
{
	FPC_FILTER_REJECTS,       // engine frames rejected by Host_FilterTime since the previous frame
	FPC_PACKETS_IN,           // server socket packets read
	FPC_BYTES_IN,
	FPC_PACKETS_OUT,          // server socket packets sent (before split)
	FPC_BYTES_OUT,
	FPC_CLIENTS_SENT,         // clients that received a snapshot
	FPC_ENTITIES_SENT,        // packet entities, summed over clients

	FPC_COUNT
};

class CFrameProfiler
{
public:
	CFrameProfiler();

	void Init();
	void Shutdown();

	// Main loop hooks
	void BeginFrame();
	void EndFrame();
	void FrameRejected();

	bool IsActive() const { return m_bActive; }

	void BeginPhase(FrameProfilerPhase phase)
	{
		if (m_bActive)
			m_PhaseStart[phase] = Now();
	}

	void EndPhase(FrameProfilerPhase phase)
	{
		if (m_bActive && m_PhaseStart[phase])
		{
			m_PhaseTicks[phase] += Now() - m_PhaseStart[phase];
			m_PhaseStart[phase] = 0;
		}
	}

	void AddCounter(FrameProfilerCounter counter, int value)
	{
		if (m_bActive)
			m_Counters[counter] += value;
	}

	// Console commands
	void Cmd_Profile();
	void Cmd_DumpCsv();
	void Cmd_Reset();

private:
	// Columns of one stored frame row
	enum
	{
		COL_TICK_INTERVAL = 0,
		COL_PHASES = 1,
		COL_COUNTERS = COL_PHASES + FPP_COUNT,
		COL_COUNT = COL_COUNTERS + FPC_COUNT
	};

	static uint64 Now()
	{
		return Sys_MonotonicNs();
	}

	bool Allocate();
	void Free();
	void ClearRows();
	float *Row(int index) const;
	void PrintReport() const;

	bool m_bActive;
	bool m_bInFrame;

	uint64 m_PhaseStart[FPP_COUNT];
	uint64 m_PhaseTicks[FPP_COUNT];
	int64 m_Counters[FPC_COUNT];

	uint64 m_FrameStart;
	uint64 m_PrevFrameStart;
	uint64 m_TickInterval;

	// Timed capture started by "sv_profile <seconds>"
	uint64 m_CaptureEnd;

	// Ring buffer of frame rows, oldest first starting at (m_Head - m_Count)
	float *m_Rows;
	int m_Capacity;
	int m_Head;
	int m_Count;
};

extern CFrameProfiler g_FrameProfiler;

void FrameProfiler_Init();
void FrameProfiler_Shutdown();
