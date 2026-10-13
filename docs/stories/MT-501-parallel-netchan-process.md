# MT-501 — Parallel netchan decoding of incoming packets

**Epic:** E5 Parallel input · **Priority:** P3 · **Size:** L · **Depends on:** MT-302, MT-203

## Context

`SV_ReadPackets` (`engine/sv_main.cpp:3924`) processes packets one by one:
`Netchan_Process` (sequence/ack, reliable handling, fragment reassembly, bzip2
decompression of fragmented data, `Netchan_CopyNormalFragments`) followed immediately by
`SV_ExecuteClientMessage`, which calls the game DLL (`ClientCommand`, `CmdStart`,
`PM_Move`, `PlayerPreThink`...).

Only the netchan layer is engine-owned and independent per client. Its cost is usually
small, except for clients uploading large fragmented data (customizations, voice), which
is where decompression can spike.

## User story

As a server operator, I want heavy per-client input decoding (fragments, decompression)
to happen off the main thread, so that one client's upload cannot slow everybody's tick.

## Scope

* Group packets popped from the receive ring by client (MT-302/MT-104).
* Parallel phase: `Netchan_Process` + fragment reassembly + decompression per client, on
  per-client message buffers (no global `net_message`).
* Serial phase, **in original arrival order**: `SV_ExecuteClientMessage` for each
  processed packet.
* Only enabled when profiling shows benefit; cvar `sv_parallel_recv 0/1`.

## Acceptance criteria

* [ ] Identical game behaviour and replay output.
* [ ] A client uploading a large spray during `cs32` does not change tick p99.

## Risks

* `Netchan_Process` side effects on globals (`net_drop`, `net_from` usage, download
  handling) — must be covered by MT-201 first.
* Gain may be too small to justify complexity: re-evaluate after MT-002 numbers.

## Out of scope

* Parallel `SV_ExecuteClientMessage` / `SV_RunCmd` (game DLL, non-goal).

## Status

Not done: measured first (2026-10-13), the gain does not justify the work.

* `Netchan_Process` takes 0.3 % of `sv_frame` with 32 clients and 400 entities
  (`clients32-ents`): 0.54 µs per frame on average, 5.7 µs p99, 23.5 µs at most. The
  input cost is `SV_ExecuteClientMessage` (42 % of `sv_frame`), game DLL code that must
  stay on the main thread.
* The spike the story aims at, a spray upload decompressed with bzip2 on arrival: 0.35 ms
  for a 12 KB spray, 1.9 ms at the 64 KB limit (`sv_net_incoming_decompression_max_size`),
  once per player and connection, already bounded by the `sv_net_incoming_decompression_*`
  limits.

Moving `Netchan_Process` off the main thread (per-client message buffers instead of the
global `net_message`, side effects on engine globals) would win at most these 0.3 % and
2 ms per upload.
