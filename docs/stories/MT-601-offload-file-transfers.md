# MT-601 — Offload file transfer processing

**Epic:** E6 Background work · **Priority:** P3 · **Size:** M · **Depends on:** MT-202

## Context

Several file-related operations run synchronously on the main thread:

* `SV_ProcessFile` (`engine/sv_main.cpp:3800`) for uploaded customizations (sprays):
  MD5 checks, writing to disk;
* netchan file fragments for downloads (`Netchan_CreateFileFragments`): opening and
  reading files, optional bzip2 compression of the file data;
* consistency (`SV_TransferConsistencyInfo`) and resource hashing on map start.

These are rare but large operations that can produce hitches when many clients connect
at once (map change with 32 players downloading).

## User story

As a player, I want the server not to stutter when other players connect and download
resources, so that gameplay stays smooth during joins and map changes.

## Scope

* Download path: read + compress file fragments on a worker / I/O thread, keep a small
  per-file cache of compressed data for the map (most clients request the same files);
  netchan only queues ready fragments.
* Upload path: hashing and disk writes of customizations on the I/O thread; the main
  thread is notified when the file is ready and then runs the existing logic
  (`SV_ProcessFile` game-visible parts, customization registration, game DLL callbacks).
* Must keep the dlfile flood protections from `550f2d6` intact.

## Acceptance criteria

* [ ] 32 simultaneous clients downloading a map: tick p99 within 10 % of steady state.
* [ ] Spray upload/propagation unchanged for clients.
* [ ] No file handle leaks across map changes.

## Out of scope

* HTTP fast download (`sv_downloadurl`) — already off-server.

## Status

Set aside on `exp/mt601-async-compression`, not merged: production servers send their
files over HTTP (`sv_downloadurl`), which the engine never touches. For servers that
still send files through the game connection:

* `sv_filetransfer_async 1` (default 0): the first download of a file without an up to
  date `.ztmp` no longer compresses it with bzip2 on the main thread; a compression
  thread does, and the download waits in the client's queue meanwhile
  (`rehlds/rehlds/file_compressor.cpp`, `sv_filetransfer_async_stats`).
* Fragment chains are built with an O(1) tail append: building the 4000 fragments of a
  6 MB map took 10 to 20 ms per client, walking the chain for each one.
* With `sv_filetransfer_async 1`, at most 1000 file fragments are built per frame, so
  clients whose file becomes ready together are spread over several frames.
* Split packets of 4 KB or more are compressed once per content: the sign-on messages
  (about 20 KB) are the same for every client of a map. Same bytes on the wire.

`tools/bench/scenarios/clients8-download.env` (4 clients in game, 4 that connect
together and download `maps/de_inferno.bsp`, 6.3 MB, its `.ztmp` deleted first):

| | `sv_filetransfer_async 0` | `sv_filetransfer_async 1` |
|---|---|---|
| Longest frame | 845 to 946 ms (compression) | 14.6 ms (sign-on commands of 4 clients) |
| Next frames | 8 to 10 ms (fragment chains) | 6 to 7 ms (one fragment chain each) |

The files arrive intact (4 clients, `cs_assault.wad`, same MD5 as the server's in both
modes). The sign-on commands themselves (`new`, `spawn`, `dlfile`) cost 1 to 2 ms each
in the benchmark container, mostly file system calls of about 0.25 ms each there.
Not run yet: unit tests, map change check, Windows build.
