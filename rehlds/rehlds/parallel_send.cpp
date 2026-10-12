#include "precompiled.h"

#ifndef _WIN32
#include <unistd.h>
#endif

// Workers are woken when the first snapshot of a frame is gathered and encode while
// the next ones are gathered, which hides their wake-up: spinning between frames
// only costs CPU by default
cvar_t sys_threads_spin_us = { "sys_threads_spin_us", "0", 0, 0.0f, nullptr };

namespace
{
	const int MAX_THREADS = 8;
	const int MAX_SPIN_US = 2000;

	CWorkerPool s_Workers;
	engine_thread_state_t s_WorkerStates[MAX_THREADS];

	client_snapshot_t *s_pSnapshots = nullptr;
	int s_NumSnapshots = 0;
	bool s_bActive = false;
	// A plugin hooks SV_EmitPings: the snapshots with pings are encoded on the main thread
	bool s_bPingsOnMainThread = false;
	// The workers encode while the main thread gathers, see parallel_send.h
	bool s_bOverlapGather = false;

	// Snapshots encoded by the workers in the current frame, and since the start
	std::atomic<uint32> s_EncodedOnWorkers(0);
	uint64 s_TotalOnWorkers = 0;
	uint64 s_TotalOnMainThread = 0;

	// What was logged last: whether the snapshots were encoded in parallel, else why not
	bool s_bLogged = false;
	char s_LoggedReason[128];

	int CpuCount()
	{
#ifdef _WIN32
		SYSTEM_INFO info;
		GetSystemInfo(&info);
		return (int)info.dwNumberOfProcessors;
#else
		long count = sysconf(_SC_NPROCESSORS_ONLN);
		return count > 0 ? (int)count : 1;
#endif
	}

	// -sys_threads N, or -sys_threads auto: one per CPU but the main thread's
	int ThreadCountFromCommandLine()
	{
		int parm = COM_CheckParm("-sys_threads");
		if (!parm || parm >= com_argc - 1)
			return 0;

		const char *value = com_argv[parm + 1];
		int count = Q_stricmp(value, "auto") ? Q_atoi(value) : CpuCount() - 1;
		if (count < 0)
			count = 0;
		if (count > MAX_THREADS)
			count = MAX_THREADS;

		return count;
	}

	void InitWorkerThread(int worker)
	{
		ThreadState_Init(&s_WorkerStates[worker], false);
		ThreadState_SetCurrent(&s_WorkerStates[worker]);
	}

	// Deltas of the encode path whose encoding calls game DLL code
	bool HasConditionalEncoders()
	{
		const delta_t *deltas[] = { g_pplayerdelta, g_pentitydelta, g_pcustomentitydelta,
			g_pclientdelta, g_pweapondelta, g_peventdelta };

		for (const delta_t *delta : deltas)
		{
			if (delta && delta->conditionalencode)
				return true;
		}

		return false;
	}

	bool EncodeOnMainThread(const client_snapshot_t *snapshot)
	{
		return s_bPingsOnMainThread && snapshot->sendPings;
	}

	void EncodeSnapshot(void *context, int index)
	{
		client_snapshot_t *snapshot = &((client_snapshot_t *)context)[index];
		if (snapshot->datagram && !EncodeOnMainThread(snapshot))
		{
			SV_EncodeClientDatagram(snapshot);
			if (!IsMainThread())
				s_EncodedOnWorkers.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// The encode path calls these hookchains for every snapshot: their subscribers
	// (plugins) must run on the main thread. SV_EmitPings only concerns the snapshots
	// with pings, see EncodeOnMainThread.
	const char *HookedEncodePath()
	{
		if (!g_RehldsHookchains.m_SV_CreatePacketEntities.isEmpty())
			return "SV_CreatePacketEntities";
		if (!g_RehldsHookchains.m_SV_EmitEvents.isEmpty())
			return "SV_EmitEvents";
		return nullptr;
	}

	const char *SerialReason()
	{
#ifndef REHLDS_JIT
		return "the engine is built without the delta JIT";
#else
		if (!s_Workers.IsRunning())
			return "no worker threads, start the server with -sys_threads N";

		static char hooked[128];
		const char *hook = HookedEncodePath();
		if (hook)
		{
			Q_snprintf(hooked, sizeof(hooked), "a plugin hooks %s", hook);
			return hooked;
		}

		return nullptr;
#endif
	}

	void Cmd_ParallelSendStats()
	{
		if (!s_Workers.IsRunning())
		{
			Con_Printf("No worker threads (start the server with -sys_threads N)\n");
			return;
		}

		const char *reason = sv_parallel_send.value != 0.0f ? SerialReason() : "snapshots encoded on the main thread";
		Con_Printf("%d worker threads, sv_parallel_send %s, %s%s%s\n", s_Workers.NumWorkers(),
			sv_parallel_send.string, reason ? reason : "snapshots encoded in parallel",
			reason ? "" : (HasConditionalEncoders() ? " after the gather (game DLL conditional encoders)" : " during the gather"),
			(!reason && !g_RehldsHookchains.m_SV_EmitPings.isEmpty()) ? ", except those with pings (a plugin hooks SV_EmitPings)" : "");
		Con_Printf("  snapshots encoded by the workers %llu, by the main thread %llu\n",
			(unsigned long long)s_TotalOnWorkers, (unsigned long long)s_TotalOnMainThread);
	}
}

void ParallelSend_Init()
{
	Cmd_AddCommand("sv_parallel_send_stats", Cmd_ParallelSendStats);
	RACE_BENIGN(&s_EncodedOnWorkers, sizeof(s_EncodedOnWorkers), "statistics");
	Cvar_RegisterVariable(&sys_threads_spin_us);

	int threads = ThreadCountFromCommandLine();
	if (threads <= 0)
		return;

	if (!s_Workers.Start(threads, InitWorkerThread, "rehlds-enc"))
	{
		Con_Printf("WARNING: cannot create the snapshot worker threads, encoding on the main thread\n");
		return;
	}

	Con_DPrintf("%d snapshot worker threads started\n", s_Workers.NumWorkers());
}

void ParallelSend_Shutdown()
{
	s_Workers.Stop();
}

bool ParallelSend_Begin(client_snapshot_t *snapshots)
{
	const char *reason = SerialReason();
	if (!s_bLogged || Q_strcmp(reason ? reason : "", s_LoggedReason))
	{
		if (reason)
			Con_Printf("sv_parallel_send: encoding snapshots on the main thread, %s\n", reason);
		else
			Con_Printf("sv_parallel_send: encoding snapshots on %d worker threads\n", s_Workers.NumWorkers());

		Q_strlcpy(s_LoggedReason, reason ? reason : "");
		s_bLogged = true;
	}

	if (reason)
		return false;

	int spin = (int)sys_threads_spin_us.value;
	s_Workers.SetSpinTime(spin < 0 ? 0 : (spin > MAX_SPIN_US ? MAX_SPIN_US : spin));

	s_pSnapshots = snapshots;
	s_NumSnapshots = 0;
	s_bPingsOnMainThread = !g_RehldsHookchains.m_SV_EmitPings.isEmpty();
	s_bOverlapGather = !HasConditionalEncoders();
	s_bActive = true;
	DELTAJit_SerialiseConditionalEncoders(true);

	// Wakes the workers now, so that they are running when the snapshots are published:
	// as they are gathered, or else all of them in ParallelSend_Finish
	s_Workers.BeginBatch(EncodeSnapshot, snapshots);
	return true;
}

void ParallelSend_Publish(int count)
{
	s_NumSnapshots = count;
	if (s_bOverlapGather)
		s_Workers.Publish(count);
}

void ParallelSend_Finish()
{
	if (!s_bActive)
		return;

	if (!s_bOverlapGather)
		s_Workers.Publish(s_NumSnapshots);

	s_Workers.Finish();
	DELTAJit_SerialiseConditionalEncoders(false);

	if (s_bPingsOnMainThread)
	{
		for (int i = 0; i < s_NumSnapshots; i++)
		{
			client_snapshot_t *snapshot = &s_pSnapshots[i];
			if (snapshot->datagram && EncodeOnMainThread(snapshot))
			{
				host_client = snapshot->client;
				SV_EncodeClientDatagram(snapshot);
			}
		}
	}

	uint32 onWorkers = s_EncodedOnWorkers.exchange(0, std::memory_order_relaxed);
	int datagrams = 0;
	for (int i = 0; i < s_NumSnapshots; i++)
		datagrams += s_pSnapshots[i].datagram ? 1 : 0;

	g_FrameProfiler.AddCounter(FPC_SEND_WORKER_ITEMS, onWorkers);
	s_TotalOnWorkers += onWorkers;
	s_TotalOnMainThread += datagrams - onWorkers;

	s_bActive = false;
	s_pSnapshots = nullptr;

	for (int i = 0; i < s_Workers.NumWorkers(); i++)
		ThreadState_FlushConsole(&s_WorkerStates[i]);
}
