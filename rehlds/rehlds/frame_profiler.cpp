#include "precompiled.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Per-phase server frame profiler.
//
// Timings use Begin/End calls instead of RAII scopes on purpose: the engine
// leaves frames with longjmp (Host_Error -> host_abortserver), which would skip
// C++ destructors. A frame that never reaches EndFrame is simply discarded.
//
// All samples go into a ring buffer allocated when profiling is switched on,
// so nothing is allocated while frames are running.

cvar_t sv_profile_enable = { "sv_profile_enable", "0", 0, 0.0f, nullptr };
cvar_t sv_profile_frames = { "sv_profile_frames", "60000", 0, 60000.0f, nullptr };

CFrameProfiler g_FrameProfiler;

namespace
{
	const int MIN_CAPACITY = 1000;
	const int MAX_CAPACITY = 600000;

	struct PhaseInfo
	{
		const char *name;
		int depth;
	};

	// Indexed by FrameProfilerPhase, depth shows nesting in the report
	const PhaseInfo s_PhaseInfo[FPP_COUNT] =
	{
		{ "frame",              0 },
		{ "cbuf",               1 },
		{ "sv_frame",           1 },
		{ "read_packets",       2 },
		{ "connectionless",     3 },
		{ "netchan_process",    3 },
		{ "execute_client_msg", 3 },
		{ "physics",            2 },
		{ "check_timeouts",     2 },
		{ "send_messages",      2 },
		{ "datagram",           3 },
		{ "gather",             4 },
		{ "encode",             4 },
		{ "transmit",           3 },
		{ "steam",              2 },
		{ "log_write",          1 },
	};

	const char *const s_CounterNames[FPC_COUNT] =
	{
		"filter_rejects",
		"packets_in",
		"bytes_in",
		"packets_out",
		"bytes_out",
		"clients_sent",
		"entities_sent",
	};

	struct ColumnStats
	{
		double avg;
		float p50;
		float p95;
		float p99;
		float max;
		double stddev;
	};

	void FrameProfiler_Profile_f() { g_FrameProfiler.Cmd_Profile(); }
	void FrameProfiler_DumpCsv_f() { g_FrameProfiler.Cmd_DumpCsv(); }
	void FrameProfiler_Reset_f() { g_FrameProfiler.Cmd_Reset(); }
}

CFrameProfiler::CFrameProfiler()
{
	m_bActive = false;
	m_bInFrame = false;
	m_TicksToUs = 0.001;

	Q_memset(m_PhaseStart, 0, sizeof(m_PhaseStart));
	Q_memset(m_PhaseTicks, 0, sizeof(m_PhaseTicks));
	Q_memset(m_Counters, 0, sizeof(m_Counters));

	m_FrameStart = 0;
	m_PrevFrameStart = 0;
	m_TickInterval = 0;
	m_CaptureEnd = 0;

	m_Rows = nullptr;
	m_Capacity = 0;
	m_Head = 0;
	m_Count = 0;
}

void CFrameProfiler::Init()
{
#ifdef _WIN32
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	m_TicksToUs = 1000000.0 / (double)freq.QuadPart;
#else
	m_TicksToUs = 0.001; // nanoseconds
#endif

	Cvar_RegisterVariable(&sv_profile_enable);
	Cvar_RegisterVariable(&sv_profile_frames);

	Cmd_AddCommand("sv_profile", FrameProfiler_Profile_f);
	Cmd_AddCommand("sv_profile_csv", FrameProfiler_DumpCsv_f);
	Cmd_AddCommand("sv_profile_reset", FrameProfiler_Reset_f);
}

void CFrameProfiler::Shutdown()
{
	m_bActive = false;
	m_CaptureEnd = 0;
	Free();
}

bool CFrameProfiler::Allocate()
{
	if (m_Rows)
		return true;

	int capacity = (int)sv_profile_frames.value;
	if (capacity < MIN_CAPACITY)
		capacity = MIN_CAPACITY;
	else if (capacity > MAX_CAPACITY)
		capacity = MAX_CAPACITY;

	m_Rows = (float *)Mem_Malloc(sizeof(float) * COL_COUNT * capacity);
	if (!m_Rows)
	{
		Con_Printf("sv_profile: unable to allocate %d frames\n", capacity);
		return false;
	}

	m_Capacity = capacity;
	ClearRows();
	return true;
}

void CFrameProfiler::Free()
{
	if (m_Rows)
		Mem_Free(m_Rows);

	m_Rows = nullptr;
	m_Capacity = 0;
	ClearRows();
}

void CFrameProfiler::ClearRows()
{
	m_Head = 0;
	m_Count = 0;
}

float *CFrameProfiler::Row(int index) const
{
	// index 0 is the oldest stored frame
	int pos = (m_Head - m_Count + index + m_Capacity) % m_Capacity;
	return &m_Rows[pos * COL_COUNT];
}

void CFrameProfiler::BeginFrame()
{
	bool wantActive = sv_profile_enable.value != 0.0f || m_CaptureEnd != 0;
	if (wantActive && !m_Rows && !Allocate())
	{
		wantActive = false;
		m_CaptureEnd = 0;
	}

	if (wantActive != m_bActive)
	{
		m_bActive = wantActive;
		m_PrevFrameStart = 0;
		m_bInFrame = false;
		Q_memset(m_PhaseTicks, 0, sizeof(m_PhaseTicks));
		Q_memset(m_Counters, 0, sizeof(m_Counters));
	}

	if (!m_bActive)
		return;

	// The previous frame was aborted by a longjmp: drop what it accumulated
	if (m_bInFrame)
		Q_memset(m_PhaseTicks, 0, sizeof(m_PhaseTicks));

	Q_memset(m_PhaseStart, 0, sizeof(m_PhaseStart));

	m_FrameStart = Now();
	m_TickInterval = m_PrevFrameStart ? m_FrameStart - m_PrevFrameStart : 0;
	m_PrevFrameStart = m_FrameStart;
	m_bInFrame = true;

	m_PhaseStart[FPP_FRAME] = m_FrameStart;
}

void CFrameProfiler::EndFrame()
{
	if (!m_bActive || !m_bInFrame)
		return;

	EndPhase(FPP_FRAME);
	m_bInFrame = false;

	// Encode time is everything spent building datagrams outside game DLL calls
	m_PhaseTicks[FPP_SEND_ENCODE] = m_PhaseTicks[FPP_SEND_DATAGRAM] > m_PhaseTicks[FPP_SEND_GATHER]
		? m_PhaseTicks[FPP_SEND_DATAGRAM] - m_PhaseTicks[FPP_SEND_GATHER]
		: 0;

	// The first frame after activation has no previous frame to measure the tick from
	if (m_TickInterval)
	{
		float *row = &m_Rows[m_Head * COL_COUNT];
		row[COL_TICK_INTERVAL] = (float)(m_TickInterval * m_TicksToUs);

		for (int i = 0; i < FPP_COUNT; i++)
			row[COL_PHASES + i] = (float)(m_PhaseTicks[i] * m_TicksToUs);

		for (int i = 0; i < FPC_COUNT; i++)
			row[COL_COUNTERS + i] = (float)m_Counters[i];

		m_Head = (m_Head + 1) % m_Capacity;
		if (m_Count < m_Capacity)
			m_Count++;
	}

	Q_memset(m_PhaseTicks, 0, sizeof(m_PhaseTicks));
	Q_memset(m_Counters, 0, sizeof(m_Counters));

	if (m_CaptureEnd && m_FrameStart >= m_CaptureEnd)
	{
		m_CaptureEnd = 0;
		PrintReport();
	}
}

void CFrameProfiler::FrameRejected()
{
	AddCounter(FPC_FILTER_REJECTS, 1);
}

static ColumnStats ComputeStats(std::vector<float> &values)
{
	ColumnStats stats = {};
	if (values.empty())
		return stats;

	double sum = 0.0;
	for (float v : values)
		sum += v;

	stats.avg = sum / values.size();

	double variance = 0.0;
	for (float v : values)
		variance += (v - stats.avg) * (v - stats.avg);

	stats.stddev = std::sqrt(variance / values.size());

	std::sort(values.begin(), values.end());

	auto percentile = [&values](double p) {
		size_t index = (size_t)(p * (values.size() - 1) + 0.5);
		return values[index];
	};

	stats.p50 = percentile(0.50);
	stats.p95 = percentile(0.95);
	stats.p99 = percentile(0.99);
	stats.max = values.back();
	return stats;
}

void CFrameProfiler::PrintReport() const
{
	if (!m_Rows || m_Count == 0)
	{
		Con_Printf("sv_profile: no frames recorded\n");
		return;
	}

	std::vector<float> values(m_Count);
	auto columnStats = [&](int column) {
		for (int i = 0; i < m_Count; i++)
			values[i] = Row(i)[column];

		return ComputeStats(values);
	};

	ColumnStats tick = columnStats(COL_TICK_INTERVAL);
	ColumnStats frame = columnStats(COL_PHASES + FPP_FRAME);

	double seconds = tick.avg * m_Count / 1000000.0;
	double fps = tick.avg > 0.0 ? 1000000.0 / tick.avg : 0.0;

	Con_Printf("sv_profile: %d frames over %.2f s, %.1f fps (sys_ticrate %.0f), busy %.1f%%\n",
		m_Count, seconds, fps, sys_ticrate.value, tick.avg > 0.0 ? 100.0 * frame.avg / tick.avg : 0.0);

	Con_Printf("%-24s %9s %9s %9s %9s %9s %9s\n", "time (us)", "avg", "p50", "p95", "p99", "max", "stddev");
	Con_Printf("%-24s %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f\n", "tick_interval",
		tick.avg, tick.p50, tick.p95, tick.p99, tick.max, tick.stddev);

	for (int phase = 0; phase < FPP_COUNT; phase++)
	{
		ColumnStats stats = columnStats(COL_PHASES + phase);

		char label[64];
		Q_snprintf(label, sizeof(label), "%*s%s", s_PhaseInfo[phase].depth * 2, "", s_PhaseInfo[phase].name);

		Con_Printf("%-24s %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f\n", label,
			stats.avg, stats.p50, stats.p95, stats.p99, stats.max, stats.stddev);
	}

	Con_Printf("%-24s %9s %9s %9s %9s %9s\n", "per frame", "avg", "p50", "p95", "p99", "max");
	for (int counter = 0; counter < FPC_COUNT; counter++)
	{
		ColumnStats stats = columnStats(COL_COUNTERS + counter);
		Con_Printf("%-24s %9.1f %9.0f %9.0f %9.0f %9.0f\n", s_CounterNames[counter],
			stats.avg, stats.p50, stats.p95, stats.p99, stats.max);
	}
}

void CFrameProfiler::Cmd_Profile()
{
	if (Cmd_Argc() < 2)
	{
		if (m_Count == 0)
		{
			Con_Printf("Usage: sv_profile <seconds>  - record frames for <seconds>, then print a report\n");
			Con_Printf("       sv_profile            - print a report of the recorded frames\n");
			Con_Printf("Set sv_profile_enable 1 to record continuously (last sv_profile_frames frames).\n");
			return;
		}

		PrintReport();
		return;
	}

	double seconds = Q_atof(Cmd_Argv(1));
	if (seconds <= 0.0)
	{
		Con_Printf("sv_profile: duration must be positive\n");
		return;
	}

	if (!Allocate())
		return;

	ClearRows();
	m_CaptureEnd = Now() + (uint64)(seconds * 1000000.0 / m_TicksToUs);
	Con_Printf("sv_profile: recording %.1f s\n", seconds);
}

void CFrameProfiler::Cmd_DumpCsv()
{
	if (Cmd_Argc() < 2)
	{
		Con_Printf("Usage: sv_profile_csv <file>  - write recorded frames to <file> in the game directory\n");
		return;
	}

	const char *fileName = Cmd_Argv(1);
	if (fileName[0] == '/' || fileName[0] == '\\' || Q_strstr(fileName, "..") || Q_strstr(fileName, ":"))
	{
		Con_Printf("sv_profile_csv: invalid file name\n");
		return;
	}

	if (!m_Rows || m_Count == 0)
	{
		Con_Printf("sv_profile_csv: no frames recorded\n");
		return;
	}

	FileHandle_t file = FS_Open(fileName, "wt");
	if (!file)
	{
		Con_Printf("sv_profile_csv: unable to open %s\n", fileName);
		return;
	}

	char line[1024];
	int len = Q_snprintf(line, sizeof(line), "tick_interval_us");
	for (int i = 0; i < FPP_COUNT; i++)
		len += Q_snprintf(&line[len], sizeof(line) - len, ",%s_us", s_PhaseInfo[i].name);

	for (int i = 0; i < FPC_COUNT; i++)
		len += Q_snprintf(&line[len], sizeof(line) - len, ",%s", s_CounterNames[i]);

	len += Q_snprintf(&line[len], sizeof(line) - len, "\n");
	FS_Write(line, len, 1, file);

	for (int r = 0; r < m_Count; r++)
	{
		const float *row = Row(r);

		len = Q_snprintf(line, sizeof(line), "%.1f", row[COL_TICK_INTERVAL]);
		for (int i = 0; i < FPP_COUNT; i++)
			len += Q_snprintf(&line[len], sizeof(line) - len, ",%.1f", row[COL_PHASES + i]);

		for (int i = 0; i < FPC_COUNT; i++)
			len += Q_snprintf(&line[len], sizeof(line) - len, ",%.0f", row[COL_COUNTERS + i]);

		len += Q_snprintf(&line[len], sizeof(line) - len, "\n");
		FS_Write(line, len, 1, file);
	}

	FS_Close(file);
	Con_Printf("sv_profile_csv: wrote %d frames to %s\n", m_Count, fileName);
}

void CFrameProfiler::Cmd_Reset()
{
	m_CaptureEnd = 0;

	// Release the buffer so that a new sv_profile_frames value is applied
	Free();
	m_bActive = false;
	Con_Printf("sv_profile: reset\n");
}

void FrameProfiler_Init()
{
	g_FrameProfiler.Init();
}

void FrameProfiler_Shutdown()
{
	g_FrameProfiler.Shutdown();
}
