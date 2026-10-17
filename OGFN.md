# OGFN -- Season 3 private server, offline, bots, full locker

Fork base: **Milxnor/Project-Reboot-3.0** at `10c6590` ("automate recomp", 2026-10-01). Upstream
BSD-3-Clause -- license and notices untouched; everything added in this fork is in addition to,
not a replacement of, upstream code. This branch: `ogfn/bots-online`.

## Architecture (settled 2026-10-18)

TWO processes, one law: **the DLL instance is the gameserver, the plain instance is you.**

| piece | how it runs | what it is |
|---|---|---|
| `OGFN-BACKEND` task | LawinV1 (GPL-3, own dir `C:\ogfn\backend`), Node, ports 3551 + XMPP 80 | auth, profiles, storefront, matchmaker -> `ip=127.0.0.1 port=7777` from its `Config/config.ini` |
| `OGFN-PROXY` task | mitmproxy venv `C:\ogfn\mitm`, 127.0.0.1:8980, `ops/ogfn_redirect.py`, **requires `connection_strategy=lazy`** | TLS-terminating rewrite of `*.epicgames.com` to the backend (Lawin's documented Fiddler recipe); `.dev` hosts deliberately untouched so live EGS keeps working |
| `OGFN-PLAY` task | `start.ps1`: health-checks services -> session proxy on -> starts the client -> **waits for the engine-ready log line** -> injects `Reboot.dll` | the SERVER: flips GIsServer, opens Athena_Terrain, listens on **UDP 7777** (TCP probes will lie to you), spawns 20 RebootBots, removes its own local player -- its window is a dead UI BY DESIGN, minimize it |
| `OGFN-CLIENT` task | same client exe, **no DLL**, same auth argv | YOUR game window: menu, PLAY, matchmaker -> 127.0.0.1:7777 -> your own island |
| `OGFN-PROXYGUARD` | at logon, clears stale ProxyEnable | a hard power-off mid-session leaves ProxyEnable=1 pointing at a dead proxy; this makes that unloseable |

Boot argv is load-bearing (the naked 2018 client self-exits at ~2s with code 0 without it):
`-obfuscationid=... -AUTH_LOGIN=TreXito -AUTH_PASSWORD=Rebooted -AUTH_TYPE=exchangecode
-epicapp=Fortnite -epicenv=Prod -EpicPortal -epicusername=TreXito -epiclocale=en
-epicsandboxid=fn -log`.

The client libcurl runs `bVerifyPeer=false` and honors the system proxy, so the redirect needs
no certificate trickery beyond the mitm CA already in Root (`certutil -delstore Root mitmproxy`
to roll back).

## Runbook: cold start to playable (Windows box)

1. `schtasks /run /tn OGFN-BACKEND` ; `schtasks /run /tn OGFN-PROXY` (or just let start.ps1
   health-check them -- it starts both when down).
2. `powershell -ExecutionPolicy Bypass -File C:\ogfn\src\build.ps1` -- if the game windows are
   open, close them first or the linker loses the PDB (LNK1201).
3. `schtasks /run /tn OGFN-PLAY` -- wait ~15 s: `reboot.log` must show `ogfn: pinned client`
   -> `brains online: first fleet size 20` -> `verbs: ...` -> `zone fields: ...` -> `armed: ...`.
   The server window appearing then freezing is CORRECT.
4. `schtasks /run /tn OGFN-CLIENT` -- your window. Pick BR, PLAY, island, bus.
5. Evidence streams if anything misbehaves: `C:\ogfn\backend.log` (every HTTP request),
   `C:\ogfn\requests.log` (every mitm rewrite), `reboot.log` (gameserver + brains),
   `%LOCALAPPDATA%\FortniteGame\Saved\Logs\FortniteGame.log` (engine truth),
   `C:\ogfn\dumps\` (LocalDumps armed: full minidumps + PDB next to the build),
   `C:\ogfn\widlist.txt` / `C:\ogfn\cidlist.txt` (runtime catalogs).

## First-compile risk inventory (adapter never MSVC-checked yet)

Windows was offline when `ogfn-brain-adapter.cpp` was written. The known traps are already
handled (ASCII-only, braced log-macro bodies, initialized extern-inlines, const-ref vs the
fork's non-const FVector operators, PCH=NotUsing, g++ syntax-check of the vendored brain).
If the first build still complains, it will be one of: `<utility>` include for `std::pair` in
brain.h under MSVC, `FRotator` aggregate-init, `TArray::At/Num` constness, or `Rng`
copyability in `Mind::Reseed` -- all one-line fixes, iterate straight from the build log.


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

* `Injector/OgfnInjector.cpp` -- creates the client suspended, **resumes immediately**, waits on `WaitForInputIdle`, settles 1500 ms (raise with `/wait:5000` after the exe path), then does the `VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW)` injection. ASCII console, C++17. Why the wait: see "Packed shipping binary" below.

### Packed shipping binary (why injection must wait)

The S3-era `FortniteClient-Win64-Shipping.exe` is **section-packed**: `.text` entropy is
7.86 at rest (~50 MB), a second `.text` section carries the unpack stub, and the entry
point sits inside the packed region. Every byte-pattern and string scan against the
still-suspended image found nothing: `Reboot.dll` AV'd at fault offset `0x759ff` (twice,
identical), and `StaticFindObject - Base` reduced to `0 - Base` = `0xffff8009151b0000`
in the log, which is the scan-returned-null tell. So the launcher resumes first and lets
the exe's own loader unpack, and the gameserver's finders then read live mapped memory.
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

## Live-service strip (added 2026-10-17 night, after the silent-exit investigation)

The 3.5 shipping client phones real Epic domains at boot; with nothing answering
127.0.0.1:3551 it falls back to spawning the installed Epic Games Launcher and
exits ("opens epic and that's it"). The full offline stack, matching
Reboot-Launcher's own design (bundled LawinV1 backend on port 3551) and
LawinServer's documented Fiddler recipe:

* **C:\ogfn\backend** -- LawinV1 subtype from Reboot-Launcher (GPL-3; kept in its
  own tree, never mixed with the BSD upstream or the fork's code). Node.js venv
  already on the box; `npm ci` then logins, MCP, cloudstorage, matchmaker, XMPP:80.
  Scheduled task `OGFN-BACKEND` -> `ops/backend-start.cmd`.
* **C:\ogfn\mitm** -- mitmproxy venv; TLS-terminating proxy on 127.0.0.1:8980 with
  `ops/ogfn_redirect.py`: requests to `*.epicgames.com` (minus `/v1/item/`) are
  rewritten to `http://127.0.0.1:3551`, mirroring the official Lawin FiddlerScript.
  `.dev` hosts are deliberately NOT rewritten so the user's real EGS keeps working
  during a session. **Requires `--set connection_strategy=lazy`** (guess from
  painful experience: eager mode opens upstream at CONNECT and the rewrite never
  applies). Scheduled task `OGFN-PROXY` -> `ops/proxy-start.cmd`.
* mitmproxy CA added to the Windows Root store (rollback:
  `certutil -delstore Root mitmproxy`). Session-scoped system proxy (HKCU) is
  enabled by `start.ps1` while the game runs and always restored in `finally`.
* Verified end to end before touching the game:
  `https://ol.epicgames.com/lightswitch/api/service/Fortnite/status` via the
  proxy returned `200 "Fortnite is online"` from the local backend.
