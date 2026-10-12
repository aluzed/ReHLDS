// Benchmark load: moving entities around the players, so that every snapshot
// carries many changing packet entities (delta encoding work).
//
// bench_spawn <count>  spawns <count> entities; each one follows a player
//                      (entity i follows the i-th alive player) at a random offset
//                      and changes direction twice per second, so that the players
//                      near each other see most of them.
#include <amxmodx>
#include <engine>

new const MODEL[] = "models/w_ak47.mdl"
new const CLASSNAME[] = "bench_entity"

const Float:SPEED = 400.0
const Float:SPREAD = 120.0

new Array:g_Entities

public plugin_precache()
{
	precache_model(MODEL)
}

public plugin_init()
{
	register_plugin("Bench entities", "1.0", "ReHLDS bench")
	register_srvcmd("bench_spawn", "SrvCmd_Spawn")
	g_Entities = ArrayCreate()
	set_task(0.5, "Task_Steer", .flags = "b")
}

public SrvCmd_Spawn()
{
	new arg[16]
	read_argv(1, arg, charsmax(arg))
	new count = str_to_num(arg)

	for (new i = 0; i < count; i++)
	{
		new ent = create_entity("info_target")
		if (!is_valid_ent(ent))
			break

		entity_set_string(ent, EV_SZ_classname, CLASSNAME)
		entity_set_model(ent, MODEL)
		entity_set_int(ent, EV_INT_movetype, MOVETYPE_NOCLIP)
		entity_set_int(ent, EV_INT_solid, SOLID_NOT)

		new Float:avel[3]
		avel[1] = random_float(-180.0, 180.0)
		entity_set_vector(ent, EV_VEC_avelocity, avel)

		ArrayPushCell(g_Entities, ent)
	}

	server_print("bench_spawn: %d entities", ArraySize(g_Entities))
	return PLUGIN_HANDLED
}

public Task_Steer()
{
	new players[MAX_PLAYERS], numPlayers
	get_players(players, numPlayers, "a")
	if (!numPlayers)
		return

	new count = ArraySize(g_Entities)
	for (new i = 0; i < count; i++)
	{
		new ent = ArrayGetCell(g_Entities, i)
		if (!is_valid_ent(ent))
			continue

		new Float:target[3], Float:origin[3], Float:velocity[3]
		entity_get_vector(players[i % numPlayers], EV_VEC_origin, target)
		entity_get_vector(ent, EV_VEC_origin, origin)

		// Spawned at the world origin: jump next to a player first
		if (origin[0] == 0.0 && origin[1] == 0.0 && origin[2] == 0.0)
		{
			target[0] += random_float(-SPREAD, SPREAD)
			target[1] += random_float(-SPREAD, SPREAD)
			entity_set_origin(ent, target)
			continue
		}

		target[0] += random_float(-SPREAD, SPREAD)
		target[1] += random_float(-SPREAD, SPREAD)
		target[2] += random_float(0.0, 100.0)

		new Float:length = get_distance_f(origin, target)
		if (length < 1.0)
			continue

		for (new k = 0; k < 3; k++)
			velocity[k] = (target[k] - origin[k]) * SPEED / length

		entity_set_vector(ent, EV_VEC_velocity, velocity)
	}
}
