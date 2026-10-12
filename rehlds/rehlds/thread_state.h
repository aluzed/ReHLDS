#pragma once

#include "archtypes.h"

// Engine state that used to live in globals and that the snapshot encoder (epic 4)
// needs per thread: the encode phase of SV_SendClientMessages runs on worker threads
// (sv_parallel_send). The main thread uses its own instance, set when the engine is
// loaded; each worker sets one when it starts (ThreadState_SetCurrent).
struct engine_thread_state_t
{
	// MSG_*Bit* writing (bfwrite, common.h)
	ALIGN16 bf_write_t bitWriter;

	// SV_SetCallback / SV_InvokeCallback (sv_main.cpp)
	deltacallback_t deltaCallback;

	// DELTA_SetTimeBaseOverride (delta.cpp), -1 when not overridden
	double timeWindowBase;
	double timeWindowOffset;

	// Client whose snapshot this thread encodes: PF_GetCurrentPlayer answers it to the
	// game DLL's conditional encoders
	client_t *encodingClient;

	// Snapshot being encoded: SV_EmitPings_internal and SV_EmitEvents_internal take
	// the pings and the events gathered for it
	client_snapshot_t *encodingSnapshot;

	// Delta fields marked by the last check, per delta description (delta_jit.cpp)
	delta_jit_marks_t deltaMarks[MAX_DELTA_JITS];

	bool isMainThread;

	// Console output of a worker thread, printed by the main thread after the
	// parallel section (ThreadState_FlushConsole)
	char deferredConsole[4096];
	int deferredConsoleSize;
	bool deferredConsoleTruncated;
};

#ifdef _WIN32
#define ENGINE_THREAD_LOCAL __declspec(thread)
#else
// The engine is a shared library: initial-exec keeps the access to one instruction,
// the variable is a single pointer
#define ENGINE_THREAD_LOCAL __thread __attribute__((tls_model("initial-exec")))
#endif

extern ENGINE_THREAD_LOCAL engine_thread_state_t *t_pThreadState;

inline engine_thread_state_t *ThreadState()
{
	return t_pThreadState;
}

// Threads the engine did not create (none calls into the engine today) count as the
// main thread
inline bool IsMainThread()
{
	return !t_pThreadState || t_pThreadState->isMainThread;
}

void ThreadState_Init(engine_thread_state_t *state, bool isMainThread);
void ThreadState_SetCurrent(engine_thread_state_t *state);

// Queues console text printed by a worker thread
void ThreadState_DeferConsole(const char *text);
// Prints the console text deferred by a worker thread, on the main thread
void ThreadState_FlushConsole(engine_thread_state_t *state);
