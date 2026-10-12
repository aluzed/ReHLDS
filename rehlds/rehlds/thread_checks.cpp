#include "precompiled.h"

#include <stdarg.h>

cvar_t sys_thread_checks = { "sys_thread_checks", "1", 0, 0.0f, nullptr };

namespace
{
	// At most one violation printed per interval, the others are counted
	const uint64 REPORT_INTERVAL_NS = 10ull * 1000 * 1000 * 1000;

	std::atomic<int> s_Mode(1);
	std::atomic<bool> s_bWindow(false);
	// Thread that owns game DLL code in the parallel window, null when none
	std::atomic<engine_thread_state_t *> s_Owner(nullptr);

	std::atomic<uint32> s_Violations(0);
	uint32 s_ReportedViolations = 0;
	uint64 s_LastReportNs = 0;

	// First violation since the last report, written by the thread that finds it
	std::atomic<bool> s_bPendingTaken(false);
	std::atomic<bool> s_bPendingReady(false);
	char s_Pending[256];

	const char *ThreadName(const engine_thread_state_t *state)
	{
		if (!state)
			return "no thread";

		return state->isMainThread ? "the main thread" : "an encoding thread";
	}

	void Violation(const char *format, ...)
	{
		s_Violations.fetch_add(1);

		char text[sizeof(s_Pending)];
		va_list args;
		va_start(args, format);
		Q_vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		text[sizeof(text) - 1] = '\0';

		if (s_Mode.load(std::memory_order_relaxed) >= 2)
		{
			// Any thread: the console belongs to the main thread
			fprintf(stderr, "FATAL: thread check: %s\n", text);
			fflush(stderr);
			abort();
		}

		if (!s_bPendingTaken.exchange(true))
		{
			Q_strlcpy(s_Pending, text);
			s_bPendingReady.store(true);
		}
	}

	void PrintPending()
	{
		if (!s_bPendingReady.load())
			return;

		uint64 now = Sys_MonotonicNs();
		if (s_LastReportNs && now - s_LastReportNs < REPORT_INTERVAL_NS)
			return;

		uint32 violations = s_Violations.load();
		Con_Printf("WARNING: thread check: %s (%u violations since the last report, %u in all)\n",
			s_Pending, violations - s_ReportedViolations, violations);

		s_ReportedViolations = violations;
		s_LastReportNs = now;
		s_bPendingReady.store(false);
		s_bPendingTaken.store(false);
	}
}

void ThreadChecks_Init()
{
	Cvar_RegisterVariable(&sys_thread_checks);
}

void ThreadChecks_BeginWindow()
{
	int mode = (int)sys_thread_checks.value;
	s_Mode.store(mode < 0 ? 0 : (mode > 2 ? 2 : mode));
	if (!s_Mode.load(std::memory_order_relaxed))
		return;

	s_Owner.store(ThreadState());
	s_bWindow.store(true);
}

void ThreadChecks_ReleaseMainThread()
{
	engine_thread_state_t *self = ThreadState();
	s_Owner.compare_exchange_strong(self, nullptr);
}

void ThreadChecks_EndWindow()
{
	if (s_bWindow.load())
	{
		s_bWindow.store(false);
		s_Owner.store(nullptr);
	}

	PrintPending();
}

bool ThreadChecks_EnterGameCode(const char *what)
{
	if (!s_bWindow.load(std::memory_order_relaxed) || !s_Mode.load(std::memory_order_relaxed))
	{
		if (!IsMainThread() && s_Mode.load(std::memory_order_relaxed))
			Violation("%s called on an encoding thread outside the parallel window", what);

		return false;
	}

	engine_thread_state_t *self = ThreadState();
	engine_thread_state_t *owner = nullptr;
	if (s_Owner.compare_exchange_strong(owner, self))
		return true;

	if (owner != self)
		Violation("%s called on %s while %s runs game DLL code", what, ThreadName(self), ThreadName(owner));

	return false;
}

void ThreadChecks_LeaveGameCode(bool entered)
{
	if (!entered)
		return;

	engine_thread_state_t *self = ThreadState();
	s_Owner.compare_exchange_strong(self, nullptr);
}

void ThreadChecks_EngineCallback(const char *name)
{
	if (IsMainThread() || !s_Mode.load(std::memory_order_relaxed))
		return;

	engine_thread_state_t *owner = s_Owner.load();
	if (!s_bWindow.load() || owner != ThreadState())
		Violation("engine function %s called back on an encoding thread that does not run game DLL code (%s does)", name, ThreadName(owner));
}

void ThreadChecks_HookChainCalled()
{
	if (!IsMainThread() && s_Mode.load(std::memory_order_relaxed))
		Violation("a hooked ReHLDS hookchain called on an encoding thread");
}

uint32 ThreadChecks_Violations()
{
	return s_Violations.load();
}
