#pragma once

// Run-time checks of the rule that keeps parallel snapshot encoding safe for the game
// DLL and its plugins (MT-404): game DLL code, and the Metamod code wrapped around it,
// runs on one thread at a time. With sv_parallel_send 1 the workers enter game DLL code
// through its conditional encoders only, and the encode path calls no plugin hook off
// the main thread.
//
// Between ParallelSend_Begin and the end of ParallelSend_Finish (the parallel window),
// one thread owns game DLL code: the main thread while it gathers the snapshots, then,
// once it hands it over, the thread that runs a conditional encoder. A thread entering
// game DLL code that another thread owns, an engine function called back by game DLL
// code on a worker that does not own it, or a hooked hookchain called on a worker are
// violations.
//
// sys_thread_checks 0 turns the checks off, 1 (default) reports the violations on the
// console, 2 also aborts the server on the first one, for a core dump.

extern cvar_t sys_thread_checks;

void ThreadChecks_Init();

// Main thread. Opens the parallel window, the main thread owns game DLL code
void ThreadChecks_BeginWindow();
// Main thread. Game DLL code can run on the encoding threads from now on
void ThreadChecks_ReleaseMainThread();
// Main thread, once the workers are done. Prints what was found
void ThreadChecks_EndWindow();

// Around a call into game DLL code that can run off the main thread (a conditional
// encoder). Returns whether this thread took game DLL code, to pass to Leave
bool ThreadChecks_EnterGameCode(const char *what);
void ThreadChecks_LeaveGameCode(bool entered);

// Engine function exported to the game DLL, called back while encoding
void ThreadChecks_EngineCallback(const char *name);
// A ReHLDS hookchain with subscribers is called (hookchains_impl.h)
void ThreadChecks_HookChainCalled();

// Violations found since the start
uint32 ThreadChecks_Violations();
