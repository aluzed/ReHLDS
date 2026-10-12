#include "precompiled.h"

ENGINE_THREAD_LOCAL engine_thread_state_t *t_pThreadState = nullptr;

namespace
{
	engine_thread_state_t s_MainThreadState;

	// The engine library is loaded by the main thread, which runs this constructor
	struct MainThreadStateInit
	{
		MainThreadStateInit()
		{
			ThreadState_Init(&s_MainThreadState, true);
			ThreadState_SetCurrent(&s_MainThreadState);
		}
	} s_MainThreadStateInit;
}

void ThreadState_Init(engine_thread_state_t *state, bool isMainThread)
{
	Q_memset(state, 0, sizeof(*state));
	state->timeWindowBase = -1.0;
	state->isMainThread = isMainThread;
}

void ThreadState_SetCurrent(engine_thread_state_t *state)
{
	t_pThreadState = state;
}

void ThreadState_DeferConsole(const char *text)
{
	engine_thread_state_t *state = ThreadState();

	int length = Q_strlen(text);
	int room = (int)sizeof(state->deferredConsole) - 1 - state->deferredConsoleSize;
	if (length > room)
	{
		length = room;
		state->deferredConsoleTruncated = true;
	}

	Q_memcpy(state->deferredConsole + state->deferredConsoleSize, text, length);
	state->deferredConsoleSize += length;
	state->deferredConsole[state->deferredConsoleSize] = '\0';
}

void ThreadState_FlushConsole(engine_thread_state_t *state)
{
	if (state->deferredConsoleSize)
		Con_Printf("%s", state->deferredConsole);

	if (state->deferredConsoleTruncated)
		Con_Printf("WARNING: console output of a worker thread was truncated\n");

	state->deferredConsoleSize = 0;
	state->deferredConsole[0] = '\0';
	state->deferredConsoleTruncated = false;
}
