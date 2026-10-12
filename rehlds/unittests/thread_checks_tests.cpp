#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#include <thread>

namespace
{
	engine_thread_state_t s_TestWorkerState;

	// Runs a function on a thread that the engine counts as an encoding thread
	template<typename Func>
	void RunOnWorker(Func func)
	{
		std::thread thread([&func]() {
			ThreadState_Init(&s_TestWorkerState, false);
			ThreadState_SetCurrent(&s_TestWorkerState);
			func();
		});
		thread.join();
	}

	void EnterAndLeave()
	{
		bool entered = ThreadChecks_EnterGameCode("a test encoder");
		ThreadChecks_EngineCallback("a test callback");
		ThreadChecks_LeaveGameCode(entered);
	}

	void TestHook(IVoidHookChain<int> *chain, int value)
	{
		chain->callNext(value);
	}

	void TestOriginal(int value)
	{
	}
}

// The order of ParallelSend_Finish: game DLL code on a worker once the main thread
// released it is fine, before is a violation
TEST(GameCodeOwnership, ThreadChecks, 5000)
{
	EngineInitializer engInitGuard;
	sys_thread_checks.value = 1.0f;

	uint32 before = ThreadChecks_Violations();
	ThreadChecks_BeginWindow();
	EnterAndLeave();	// the main thread owns game DLL code
	ThreadChecks_ReleaseMainThread();
	RunOnWorker(EnterAndLeave);
	EnterAndLeave();	// the main thread encodes too
	RunOnWorker(EnterAndLeave);
	ThreadChecks_EndWindow();
	LONGS_EQUAL("Violation with the main thread out of game DLL code", before, ThreadChecks_Violations());

	ThreadChecks_BeginWindow();
	RunOnWorker(EnterAndLeave);
	ThreadChecks_EndWindow();
	LONGS_EQUAL("Encoder and callback during the gather not reported", before + 2, ThreadChecks_Violations());

	// Game DLL code calling back the engine on a worker outside an encoder
	ThreadChecks_BeginWindow();
	ThreadChecks_ReleaseMainThread();
	RunOnWorker([]() { ThreadChecks_EngineCallback("a test callback"); });
	ThreadChecks_EndWindow();
	LONGS_EQUAL("Callback outside an encoder not reported", before + 3, ThreadChecks_Violations());

	// Off
	sys_thread_checks.value = 0.0f;
	ThreadChecks_BeginWindow();
	RunOnWorker(EnterAndLeave);
	ThreadChecks_EndWindow();
	LONGS_EQUAL("Violation reported with sys_thread_checks 0", before + 3, ThreadChecks_Violations());
	sys_thread_checks.value = 1.0f;
	ThreadChecks_BeginWindow();
	ThreadChecks_EndWindow();
}

// A hooked hookchain on a worker is a violation, an empty one is not
TEST(HookChainOffMainThread, ThreadChecks, 5000)
{
	EngineInitializer engInitGuard;
	sys_thread_checks.value = 1.0f;
	ThreadChecks_BeginWindow();
	ThreadChecks_EndWindow();

	IVoidHookChainRegistryImpl<int> registry;
	uint32 before = ThreadChecks_Violations();

	RunOnWorker([&registry]() { registry.callChain(TestOriginal, 1); });
	LONGS_EQUAL("Empty hookchain reported", before, ThreadChecks_Violations());

	registry.registerHook(TestHook, 0);
	registry.callChain(TestOriginal, 1);
	LONGS_EQUAL("Hookchain on the main thread reported", before, ThreadChecks_Violations());

	RunOnWorker([&registry]() { registry.callChain(TestOriginal, 1); });
	LONGS_EQUAL("Hooked hookchain on a worker not reported", before + 1, ThreadChecks_Violations());
}
