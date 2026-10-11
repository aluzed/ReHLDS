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
