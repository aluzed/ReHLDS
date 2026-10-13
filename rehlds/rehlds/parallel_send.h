#pragma once

// Parallel snapshot encoding (MT-403, MT-404). With sv_parallel_send 1,
// SV_SendClientMessages gathers the snapshots on the main thread, client by client,
// and the worker threads (-sys_threads N) encode them. The datagrams are transmitted
// afterwards by the main thread, in client order.
//
// The workers encode each snapshot as soon as it is gathered, while the main thread
// gathers the next ones. Game DLL conditional encoders (Counter-Strike has some) never
// run on the workers: an encoder is game DLL code that calls the engine back through
// the game DLL's engine function table, which Metamod wraps with code that is not
// thread-safe (its JIT callbacks save and restore global state), and the main thread
// runs game DLL code at the same time. The main thread runs the encoders while it
// gathers the snapshot and records what they mark (SV_RecordEntityEncoders,
// encoder_record_t); the workers replay that. A snapshot whose encoders were not
// recorded (full update) or do not replay is encoded by the main thread, once the
// gather is done.
//
// Falls back to encoding on the main thread when a plugin hooks the encode path,
// whose hooks must run on the main thread: SV_CreatePacketEntities or SV_EmitEvents
// for all the snapshots, SV_EmitPings for the snapshots with pings.

void ParallelSend_Init();
void ParallelSend_Shutdown();

// Called by SV_SendClientMessages with sv_parallel_send 1 before the first snapshot of
// a frame: true when this frame's snapshots are encoded by the worker threads
bool ParallelSend_Begin(client_snapshot_t *snapshots);
// Whether the main thread records the conditional encoders of this frame's snapshots
bool ParallelSend_RecordsEncoders();
// Snapshots [0, count) are gathered and can be encoded
void ParallelSend_Publish(int count);
// Encodes what is left on the main thread too, waits for the workers
void ParallelSend_Finish();
