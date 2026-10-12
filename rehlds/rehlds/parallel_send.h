#pragma once

// Parallel snapshot encoding (MT-403, MT-404). With sv_parallel_send 1,
// SV_SendClientMessages gathers the snapshots on the main thread, client by client,
// and the worker threads (-sys_threads N) encode each one as soon as it is gathered.
// The datagrams are transmitted afterwards by the main thread, in client order.
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
