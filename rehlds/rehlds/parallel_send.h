#pragma once

// Parallel snapshot encoding (MT-403, MT-404). With sv_parallel_send 1,
// SV_SendClientMessages gathers the snapshots on the main thread, client by client,
// and the worker threads (-sys_threads N) encode them. The datagrams are transmitted
// afterwards by the main thread, in client order.
//
// Without game DLL conditional encoders, the workers encode each snapshot as soon as it
// is gathered, while the main thread gathers the next ones. With them (Counter-Strike
// has some), the workers only start once every snapshot is gathered: an encoder is game
// DLL code that calls the engine back through the game DLL's engine function table,
// which Metamod wraps with code that is not thread-safe (its JIT callbacks save and
// restore global state). The encoders then never run while the main thread runs game
// DLL or Metamod code, and run one at a time on the encoding threads.
//
// Falls back to encoding on the main thread when a plugin hooks the encode path,
// whose hooks must run on the main thread: SV_CreatePacketEntities or SV_EmitEvents
// for all the snapshots, SV_EmitPings for the snapshots with pings.

void ParallelSend_Init();
void ParallelSend_Shutdown();

// Called by SV_SendClientMessages with sv_parallel_send 1 before the first snapshot of
// a frame: true when this frame's snapshots are encoded by the worker threads
bool ParallelSend_Begin(client_snapshot_t *snapshots);
// Snapshots [0, count) are gathered and can be encoded
void ParallelSend_Publish(int count);
// Encodes what is left on the main thread too, waits for the workers
void ParallelSend_Finish();
