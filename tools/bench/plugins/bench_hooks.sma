// Benchmark helper: ReHLDS hooks that a plugin may register, to check that the
// engine still behaves with them (needs ReAPI).
//
// bench_hook_pings  registers a pass-through SV_EmitPings hook: snapshots with
//                   pings must then be encoded on the main thread (sv_parallel_send)
#include <amxmodx>
#include <reapi>

new g_Calls

public plugin_init()
{
	register_plugin("Bench hooks", "1.0", "ReHLDS bench")
	register_srvcmd("bench_hook_pings", "SrvCmd_HookPings")
	register_srvcmd("bench_hook_stats", "SrvCmd_Stats")
}

public SrvCmd_HookPings()
{
	RegisterHookChain(RH_SV_EmitPings, "SV_EmitPings", .post = false)
	server_print("bench_hook_pings: SV_EmitPings hooked")
	return PLUGIN_HANDLED
}

public SV_EmitPings(const client)
{
	g_Calls++
	return HC_CONTINUE
}

public SrvCmd_Stats()
{
	server_print("bench_hook_stats: SV_EmitPings called %d times", g_Calls)
	return PLUGIN_HANDLED
}
