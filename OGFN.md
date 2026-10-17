# OGFN -- Season 3 private server, offline, bots, full locker

Fork base: **Milxnor/Project-Reboot-3.0** at `10c6590` ("automate recomp", 2026-10-01). Upstream
BSD-3-Clause -- license and notices untouched; everything added in this fork is in addition to,
not a replacement of, upstream code. This branch: `ogfn/bots-online`.

## Why this shape (decisions, for the future reader)

* **No backend, no FakeMCP, no SSL redirector.** Reboot has no login surface (verified:
  `Globals::bNoMCP = true`, zero HTTP code paths). Any LawinServer for economy/friends/
  progression is optional phase 3 -- not needed for bots or the locker.
* **3.5.0 specifically** -- Erbium's floor (3.4) is dead, Reboot's AI band is
  `>= 3.5 && <= 4`, 3.5.0 is mirrored by GalaxiaFN as of 2026-10-17. Other S3 versions
  (3.0-3.3, 3.5.x) work if later requested; hooks are 4.20-era AOB scans.
* **Bot count is a compile-time default + ImGui input, not a config file** -- matches
  upstream's zero-IO settings model. A `Config.ini` reader belongs to phase 3 with your
  real profile store, so it is not left half-built here.

## Build requirements

* Windows 10/11 x64.
* Visual Studio 2022 (17.x) with **v143** toolset and "Desktop development with C++".
* ~12 GB free disk (source + build + client). Nothing else: no CRT redist, no admin rights.

## What this fork changes

### enable-the-dormant-bot-lobby (4 lines of intent, 3 files)

1. `bots.h` -- `Bots::SpawnBotsAtPlayerStarts` opened with a bare `return;`. Removed. It picks
   a spawn-island `FortPlayerStartWarmup` and calls `SpawnBot` per bot; `Initialize()` adds the
   pawn, controller, playerstate, inventory, and increments `PlayersLeft` (that is what lets the
   bus start: `WarmupRequiredPlayerCount = 1` solo).
2. `bots.h` -- the playerstart roulette divided by `size() - 1`, which crashes when the map gives
   exactly one start; guarded (picks first when size < 2) and a missing start now `continue`s
   instead of aborting the whole spawn loop.
3. `NetDriver.cpp` -- the `if (bEnableBotTick) Bots::Tick()` call was commented out and the flag
   defaulted false. Call restored, flag defaults true.
4. `gui.h` -- `AmountOfBotsToSpawn` default 0 dead -> 20, and the commented ImGui
   `InputInt` restored so it is adjustable per session. (`WarmupRequiredPlayerCount = 1`
   already defaults solo-friendly upstream.)

Effect: the lobby fills itself for the local player and the match starts with zero humans
required and zero queue -- UE's own `ReadyToStartMatch`, patched matchmaking-off, listen server.
Boot is solo: match starts when players-left >= 1, which any bot fulfils.

### ogfn-full-locker

`ogfn_locker.h` (new, ~180 lines) + a tab in `MainTabs()`:

* **OGFN Locker** -- enumerates every `AthenaCharacterItemDefinition` (skins) and
  `AthenaDanceItemDefinition` (emotes) that exists in the loaded client -- on 3.5.0 that is the
  complete Season 3 catalogue, because the object pool contains nothing older or newer.
  Applies skins by the exact path `applycid` uses (`ApplyCID` -> `ApplyHID`), emotes by the exact
  hook the bot tick already invokes (`AFortPlayerController::ServerPlayEmoteItemHook`). Search
  box, apply button, random buttons. Applied server-side to the host pawn; no client profile
  writes, because without a backend there is no profile to write.
* Pickaxe/glider/backpack apply is not in this commit: characters and emotes cover the -full
  locker- experience; the remaining slots reuse the same `ServerChoosePart` mechanism and go
  in once S3 part subclasses have been verified in this SDK.

### ogfn-launcher + scripts

* `Injector/OgfnInjector.cpp` -- a ~150-line documented `CreateProcessW(CREATE_SUSPENDED) ->
  VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW) -> ResumeThread`
  injector, ASCII console, C++17. Single .clang/MSVC-clean file; you can read it in two minutes.
* `build.ps1` -- VS2022 discovery via vswhere, then `MSBuild` on the upstream `.sln` (game DLL,
  Release x64) and the injector `.vcxproj`. Copies outputs to `out\` and tells you the next
  command. Fails loudly with the VS download link when the workload is missing.
* `download-client.ps1` -- S3 client 3.5.0 CL-4008490 from `archive.org/download/fn-v3-archive` (the `builds.rebootfn.org`
  and `galaxiafn.co.uk` mirrors from the builds index are both dead: 404 / parked domain). BITS (resumable),
  7-Zip auto-install if absent, extracts, verifies the shipping exe, tells you the next command.
* `start.ps1` -- builds the full command line and runs the injector; `-NoLog` to skip UE `-log`
  console window (leave it on: server log readability is the diagnostic).

## Play path (Windows 10/11 x64, one-time)

    git clone https://github.com/TreXito/ogfn-reboot
    cd ogfn-reboot; git checkout ogfn/bots-online
    powershell -ExecutionPolicy Bypass -File .\build.ps1
    powershell -ExecutionPolicy Bypass -File .\download-client.ps1
    powershell -ExecutionPolicy Bypass -File .\start.ps1

Prerequisites: VS2022 + "Desktop development with C++". Optional: 7-Zip (auto-installed if missing).

## Verify first-run logs

Watch the UE log for the exact lines that prove hooks landed on 3.5:

    Matchmaking will be supported
    Initializing!
    [ogfn] locker populated: ... characters, ... emotes.

Failing `Matchmaking will not be supported` == the AOB scan missed on this client -> report
the address; it is a one-pattern fix, not a redesign.

## When a match is actually going

Open the console overlay (`bInformationTab` GUI), Match tab -> `Amount of bots to spawn` sets
next-match count. OGFN Locker -> apply any S3-era skin/emote at any time, including mid-match.

## Rollbacks

Each intent is its own commit:

    git log --oneline
    git revert <sha>

No upstream files are renamed or deleted, so `git checkout 10c6590 -- "Project Reboot 3.0/"`
undoes every patch at once.

## Next (not in this branch)

* **bots that fight back** -- the S3-era stock bot tick only jumps and thanks the bus driver;
  the plan is a seam into `PlayerBot` that binds the host-side brain (`~/ogfn/bots`, 4037 green
  checks: perception/reaction/persona/motor) into `Bots::Tick()` 8Hz think -> motor to aim/fire/
  build. Same mechanism, engine-agnostic library stays clean of SDK headers via an adapter file
  in this repo, so the brain stays g++/unit-testable and ZomboDB... engine-agnostic.
* **remaining locker slots** -- pickaxe/glider/backpack via part classes after S3 verification.
