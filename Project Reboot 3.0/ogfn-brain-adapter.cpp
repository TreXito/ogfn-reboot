// ogfn-brain-adapter.cpp -- server-side wiring of the engine-agnostic ogfn brain
// (vendored under ogfn-brain/) into Reboot's PlayerBot fleet.
//
// Split of responsibility, kept deliberately strict:
//   engine -> ground truth only: positions, health, line traces, zone actor, inventory verbs
//   brain  -> judgment: perception gating (FOV/memory/lock-on), utility scores,
//             human plan cadence, reaction episodes, per-bot persona
//   motor  -> degradation: aim servo with overshoot, trigger discipline, jitter, idle scan
//
// Scope of this pass:
//   observe -> think -> aim/move/jump/fire, weapon grant on join, zone steering,
//   loot density counting, one-shot diagnostics for build-dependent names.
// Not yet wired (slots kept, documented): building (decision.build_wall -> builder.h),
// loot pathing to picked-up items, navmesh steering (movement is straight-line to the
// decision's move_to until ai.h SetNavigationSystem is enabled for 3.5).
//
// Every engine verb goes through FindFunction + ProcessEvent or the fork's existing
// wrappers, and every build-dependent name is resolved at runtime with logged misses,
// because this client's FName table is the only authority that matters.

#include "reboot.h"
#include "globals.h"
#include "bots.h"
#include "FortGameModeAthena.h"
#include "FortGameStateAthena.h"
#include "FortSafeZoneIndicator.h"
#include "FortWorldItemDefinition.h"
#include "KismetSystemLibrary.h"
#ifdef OGFN_HARNESS
namespace OgfnLocker { void AutoApplyOnce(); }  // provided by the harness stub
#else
#include "ogfn_locker.h"
#endif
#include "ogfn/ogfn_bots.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace OgfnBot
{
#ifdef OGFN_HARNESS
// Behavior-sim hook: P6 recreates the post-burst state (pawnless controllers that
// were alive pre-deploy), which only occurs before a bot is ever seen in combat.
void TestClearBurstState(size_t i);
#endif
#ifdef OGFN_HARNESS
// Behavior-sim hook: inject the storm ladder (live builds parse it from the engine log).
void TestSetLadder(float cx, float cy, float r);
#endif

namespace
{
	constexpr double kZoneRefreshSecs = 2.0;
	constexpr float kBotWalkSpeed = 600.f;   // uu/s, human jog; locomotion is kinematic
	                                         // (sweep-teleport steps): CharMoveComp never ticks here.
		constexpr double kWaveCadenceSecs = 2.0;
		constexpr int kWaveMaxAttempts = 30;
	constexpr double kLootRefreshSecs = 0.5;
	constexpr float kMoveMinDist = 2.5f;      // meters: decision points closer than this are "hold here"
	constexpr float kTraceEyeOffset = 60.f;    // trace between heads, not feet

	struct Mind
	{
		ogfn::Persona persona;
		ogfn::Brain brain;
		ogfn::Motor motor;
		ogfn::Decision decision;
		bool bSeeded = false;
		bool bArmed = false;
		bool bAutoLanded = false;   // bus-dump place-once flag (see the auto-land block)
		bool bNeedsReequip = false; // wave respawn: rebind the controller's rifle to the new pawn
		int fireCount = 0;          // motor triggers pulled (evidence of sustained combat)
		bool bSeenAlive = false;    // has this bot been observed with a pawn IN-MATCH?
		                            // burst victims never have (wave rescues them);
		                            // killed bots do (wave must NOT resurrect them)
		bool bElimLogged = false;   // one ELIMINATED line per bot
		bool bStormEvac = false;    // emergency storm extract done
		int stepOk = 0, stepBlk = 0; // kinematic walk: displaced vs blocked ticks
		int moveCount = 0;          // move intents that reached ApplyMoveInput
		FGuid armGuid;              // guid of the granted rifle (equip-only path, no re-add)
		double nextArmTry = 0.0;
		double nextLandTry = 0.0;   // retry clock for a rejected ground probe
		double lastTick = 0.0;

		Mind() : persona(), brain(persona, 1), motor(persona, 1) {}

		void Reseed(uint64_t seed, float skillBias)
		{
			ogfn::Rng rng(seed);
			persona = ogfn::Persona::sample(rng, skillBias);
			brain = ogfn::Brain(persona, seed);
			motor = ogfn::Motor(persona, seed);
		}
	};

	std::vector<Mind> g_minds;                 // index-parallel to AllPlayerBotsToTick
	bool g_zoneTruth = false;      // is the snapshot ring real (ladder or engine field)?
	bool g_ladderParsed = false;   // engine's 9-ring storm ladder read from its log
	float g_ladderCX = 0.f, g_ladderCY = 0.f, g_ladderR = 0.f; // uu, innermost ring >0
	int g_ladderTries = 0;
	bool g_summaryLogged = false;
	UFunction* g_moveCompSetModeFn = nullptr;   // resolved once at combat gate

	// Runtime-resolved engine verbs (names differ per client build; resolve once,
	// log the misses, never guess twice).
	struct EngineVerbs
	{
		bool bResolved = false;
		UFunction* rotateFn = nullptr;
		UFunction* moveFn = nullptr;       // on the pawn itself
		UFunction* moveCompFn = nullptr;   // AddInputVector on the movement component
		UObject* moveComp = nullptr;
		UFunction* jumpFn = nullptr;
		UFunction* landFn = nullptr;   // K2_SetActorLocation (native fallback)
		UFunction* fireFn = nullptr;
		size_t fireParamsSize = 0;
		bool bFireResolved = false;
	};
	EngineVerbs g_verbs;

	// Zone (circle) view: center from the safe-zone indicator actor, radius/phase from
	// GameState fields when present. When nothing resolves, the snapshot reports the
	// bot itself as the circle center (distance edge = negative, large) which keeps the
	// brain from rotating toward a bogus world origin.
	AFortSafeZoneIndicator* g_zoneActor = nullptr;
	double g_zoneNextFind = -1e9;
	bool g_gsOffsetsTried = false;
	int g_gsNextRadiusOff = -1;
	int g_gsPhaseOff = -1;

	// Loot density, refreshed at 2 Hz and shared across bots (one GObjects sweep, not 20).
	std::vector<FVector> g_lootPositions;
	double g_lootNextRefresh = -1e9;
	bool g_lootScanned = false;

	// Weapon candidates, scanned once: whatever assault rifle this build ships.
	std::vector<UFortWorldItemDefinition*> g_weaponCands;
	bool g_weaponsScanned = false;
	bool g_dumpedWids = false;

	struct EnemyPawn
	{
		int id = 0;
		AFortPlayerPawnAthena* pawn = nullptr;
	};

	// UNIT CONTRACT: the brain speaks METERS (perception max_range=90, tests place
	// enemies at 10..500, target_distance 15..20), the engine speaks centimeters.
	// ToVec is the one crossing INTO the brain: engine uu -> meters, always.
	inline ogfn::Vec3 ToVec(const FVector& v)
	{
		return ogfn::Vec3((float)v.X * 0.01f, (float)v.Y * 0.01f, (float)v.Z * 0.01f);
	}
	inline float ToMeters(float uu) { return uu * 0.01f; }
	inline FVector ToF(const ogfn::Vec3& v) { return FVector(v.x, v.y, v.z); }
	inline int PawnId(AFortPlayerPawnAthena* p)
	{
		uintptr_t k = (uintptr_t)p;
		return (int)(k ^ (k >> 32)) & 0x7fffffff;
	}

	// Respawn-window safety: freed, mid-destroy, or not-yet-live pawns must not be
	// touched (the live crashes: ReadBitfield on NULL, then GetActorLocation via a
	// clobbered vtable -> call to 0x0 during RestartPlayerAtPlayerStart).
	inline bool PawnUsable(AFortPlayerPawnAthena* p)
	{
		return p && (uintptr_t)p > 0x10000 && p->IsValidLowLevel() && !p->IsActorBeingDestroyed();
	}

	void ResolveVerbs(AFortPlayerPawnAthena* Pawn)
	{
		if (g_verbs.bResolved)
			return;
		g_verbs.bResolved = true;

		// SetActorRotation is plain native on this chain; the Blueprint-exposed
		// UFUNCTION this build actually carries is K2_SetActorRotation (first live
		// session showed rotate=0 with only the native name).
		g_verbs.rotateFn = Pawn->FindFunction("SetActorRotation");
		if (!g_verbs.rotateFn)
			g_verbs.rotateFn = Pawn->FindFunction("K2_SetActorRotation");
		g_verbs.jumpFn = Pawn->FindFunction("Jump");
		g_verbs.landFn = Pawn->FindFunction("K2_SetActorLocation");
		if (!g_verbs.landFn)
			g_verbs.landFn = Pawn->FindFunction("SetActorLocation");
		g_verbs.moveFn = Pawn->FindFunction("AddMovementInput");
		if (!g_verbs.moveFn)
			g_verbs.moveFn = Pawn->FindFunction("K2_AddMovementInput");

		auto MoveCompOff = Pawn->GetOffset("CharacterMovement");
		g_verbs.moveComp = (MoveCompOff != -1) ? Pawn->Get<UObject*>(MoveCompOff) : nullptr;
		if (g_verbs.moveComp)
			g_verbs.moveCompFn = g_verbs.moveComp->FindFunction("AddInputVector");

		LOG_INFO(LogBots, "[ogfn] verbs: rotate={} jump={} move={} moveComp={} land={}",
			(__int64)g_verbs.rotateFn, (__int64)g_verbs.jumpFn, (__int64)g_verbs.moveFn,
			(__int64)g_verbs.moveCompFn, (__int64)g_verbs.landFn);
	}

	void ResolveFire(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
	{
		if (g_verbs.bFireResolved)
			return;
		g_verbs.bFireResolved = true;

		// FName table of this client has plain StartFire/StopFire; the longer
		// PawnClient*/Server* names are absent, but FindFunction is cheap, so try them.
		const char* candidatesPawn[] = { "StartFire", "ServerStartFire", "PawnClientStartFire" };
		const char* candidatesCtrl[] = { "StartFire", "ServerStartFire" };

		for (auto name : candidatesPawn)
		{
			g_verbs.fireFn = Pawn->FindFunction(name);
			if (g_verbs.fireFn)
				break;
		}
		if (!g_verbs.fireFn)
		{
			for (auto name : candidatesCtrl)
			{
				g_verbs.fireFn = Controller->FindFunction(name);
				if (g_verbs.fireFn)
					break;
			}
		}

		if (g_verbs.fireFn)
			g_verbs.fireParamsSize = g_verbs.fireFn->GetPropertiesSize();
		else
			g_verbs.fireParamsSize = 0;

		LOG_INFO(LogBots, "[ogfn] fire verb: fn={} parms={}", (__int64)g_verbs.fireFn, (int)g_verbs.fireParamsSize);
	}

	void ApplyRotation(AFortPlayerPawnAthena* Pawn, float pitchDeg, float yawDeg)
	{
		if (!g_verbs.rotateFn)
			return;
		if (!(pitchDeg == pitchDeg)) // NaN guard: a servo must never poison the pawn
			pitchDeg = 0.f;
		pitchDeg = ogfn::clampf(pitchDeg, -89.f, 89.f);

		struct { FRotator NewRotation; bool bTeleportPhysics; } p{ { pitchDeg, yawDeg, 0.f }, false };
		Pawn->ProcessEvent(g_verbs.rotateFn, &p);
	}

	void ApplyMoveInput(AFortPlayerPawnAthena* Pawn, const FVector& dir, float scale)
	{
		if (g_verbs.moveFn)
		{
			struct { FVector WorldDirection; float Scale; bool bForce; } p{ dir, scale, false };
			Pawn->ProcessEvent(g_verbs.moveFn, &p);
		}
		else if (g_verbs.moveComp && g_verbs.moveCompFn)
		{
			struct { FVector WorldDir; bool bForce; } p{ dir, false };
			g_verbs.moveComp->ProcessEvent(g_verbs.moveCompFn, &p);
		}
	}

	void ApplyJump(AFortPlayerPawnAthena* Pawn)
	{
		if (!g_verbs.jumpFn)
			return;
		Pawn->ProcessEvent(g_verbs.jumpFn, nullptr);
	}

	void ApplyFire(AFortPlayerPawnAthena* Pawn)
	{
		if (!g_verbs.fireFn)
			return;
		// Zero-filled params, generous size: the common shape here is a no-arg or
		// origin-only StartFire. If a future build needs more, the logged
		// parms size and the "bots never shoot" symptom name the fix.
		alignas(16) unsigned char buf[128] = {};
		Pawn->ProcessEvent(g_verbs.fireFn, buf);
	}

	// Engine-sized params buffer for K2_SetActorLocation. ProcessEvent copies
	// GetPropertiesSize() bytes in AND back out (the FHitResult out-parm lives in
	// there), so a small hand-rolled struct would get out-parms written past its
	// end. This is the same idiom the fork's own Kismet wrapper uses: offsets
	// from FindOffsetStruct, engine-sized buffer, zero-filled.
	// Engine-sized params buffer for K2_SetActorLocation. ProcessEvent copies
	// GetPropertiesSize() bytes in AND back out (the FHitResult out-parm and the
	// ReturnValue live in there), so a small hand-rolled struct would get
	// out-parms written past its end. bSweep=true makes the engine sweep the
	// capsule to the destination: that is what gives kinematic (physics-less)
	// walking its collision behavior, since CharMoveComp never ticks on this
	// build (frozen velocity despite 30/s inputs, mode byte stuck at 6).
	// Returns the engine's ReturnValue: false = the sweep did not get there.
	bool ApplyTeleportEx(AFortPlayerPawnAthena* Pawn, FVector NewLocation, bool bSweep)
	{
		if (!g_verbs.landFn)
			return false;

		static int s_locOff = -2;
		static int s_sweepOff = -2;
		static int s_rvOff = -2;
		static int s_size = 0;
		static std::vector<unsigned char> s_buf;

		if (s_locOff == -2)
		{
			// Both spellings share member names; whichever path exists wins.
			s_locOff = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "NewLocation", false);
			s_sweepOff = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "bSweep", false);
			s_rvOff = FindOffsetStruct("/Script/Engine.Actor.K2_SetActorLocation", "ReturnValue", false);
			if (s_locOff < 0 || s_sweepOff < 0)
			{
				s_locOff = FindOffsetStruct("/Script/Engine.Actor.SetActorLocation", "NewLocation", false);
				s_sweepOff = FindOffsetStruct("/Script/Engine.Actor.SetActorLocation", "bSweep", false);
				s_rvOff = FindOffsetStruct("/Script/Engine.Actor.SetActorLocation", "ReturnValue", false);
			}
			s_size = (int)g_verbs.landFn->GetPropertiesSize();
			if (s_locOff < 0 || s_sweepOff < 0 || s_size <= s_locOff + (int)sizeof(FVector) || s_size > 4096)
			{
				g_verbs.landFn = nullptr;
				LOG_ERROR(LogBots, "[ogfn] teleport layout unusable: loc={} sweep={} size={}", s_locOff, s_sweepOff, s_size);
				return false;
			}
			if (s_rvOff < 0 || s_rvOff >= s_size)
				s_rvOff = -1; // tolerate a build without the return flag
			s_buf.assign((size_t)s_size, 0);
		}

		if (!(NewLocation.X == NewLocation.X) || !(NewLocation.Y == NewLocation.Y) || !(NewLocation.Z == NewLocation.Z))
			return false; // NaN must never enter a transform
		std::memcpy(s_buf.data() + s_locOff, &NewLocation, sizeof(FVector));
		s_buf[(size_t)s_sweepOff] = bSweep ? 1 : 0;
		if (s_rvOff >= 0)
			s_buf[(size_t)s_rvOff] = 0;
		Pawn->ProcessEvent(g_verbs.landFn, s_buf.data());
		if (s_rvOff >= 0)
			return s_buf[(size_t)s_rvOff] != 0;
		return true;
	}

	void ApplyTeleport(AFortPlayerPawnAthena* Pawn, FVector NewLocation)
	{
		ApplyTeleportEx(Pawn, NewLocation, false);
	}

	// The engine prints its FULL storm ladder at match start (nine
	// PickSafeZoneLocation rings, each with Radius + Setting Location). The
	// innermost ring's center lies inside EVERY ring (verified against two
	// matches' numbers), so standing there is safe for the whole match -- that
	// is the destination that keeps bots out of the storm. Called every zone
	// refresh until the picks land in the log (~60s after SafeZones).
#ifndef OGFN_HARNESS
	void TryParseRingLadder()
	{
		if (g_ladderParsed || g_ladderTries >= 40)
			return;
		g_ladderTries++;
		const char* La = getenv("LOCALAPPDATA");
		if (!La)
			return;
		std::string Path = std::string(La) + "\\FortniteGame\\Saved\\Logs\\FortniteGame.log";
		FILE* F = nullptr;
		if (fopen_s(&F, Path.c_str(), "r") != 0 || !F)
			return;
		float Rad[10] = {}, Cx[10] = {}, Cy[10] = {};
		bool bHasR[10] = {}, bHasL[10] = {};
		char Line[768];
		while (fgets(Line, (int)sizeof(Line), F))
		{
			int N = 0;
			char* Ch = strstr(Line, "Changing Pick Radius from");
			if (Ch)
			{
				float A = 0.f, B = 0.f;
				if (sscanf_s(Line, "PickSafeZoneLocation[%d]", &N) == 1 && N >= 1 && N <= 9
					&& sscanf_s(Ch, "Changing Pick Radius from %f to %f", &A, &B) == 2)
				{
					Rad[N] = B;
					bHasR[N] = true;
				}
			}
			char* Loc = strstr(Line, "Setting Location:");
			if (Loc)
			{
				float X = 0.f, Y = 0.f, Z = 0.f;
				if (sscanf_s(Line, "PickSafeZoneLocation[%d]", &N) == 1 && N >= 1 && N <= 9
					&& sscanf_s(Loc, "Setting Location: %f, %f, %f", &X, &Y, &Z) == 3)
				{
					Cx[N] = X;
					Cy[N] = Y;
					bHasL[N] = true;
				}
			}
		}
		fclose(F);

		// Innermost ring with a real radius (>0): the final safe point.
		float BestR = 0.f;
		int BestN = 0;
		for (int N = 1; N <= 9; N++)
		{
			if (bHasR[N] && bHasL[N] && Rad[N] > 0.f && (BestN == 0 || Rad[N] < BestR))
			{
				BestR = Rad[N];
				BestN = N;
			}
		}
		if (BestN)
		{
			g_ladderCX = Cx[BestN];
			g_ladderCY = Cy[BestN];
			g_ladderR = BestR;
			g_ladderParsed = true;
			LOG_INFO(LogBots, "[ogfn] ring ladder parsed: finalC=({:.0f},{:.0f}) finalR={:.0f}uu (ring{})",
				g_ladderCX, g_ladderCY, g_ladderR, BestN);
		}
		else if (g_ladderTries == 40)
		{
			LOG_ERROR(LogBots, "[ogfn] ring ladder NEVER parsed after {} attempts: no zone truth, bots hold position (storm risk)", g_ladderTries);
		}
	}
#endif

	void RefreshZone(double Now)
	{
		if (Now < g_zoneNextFind)
			return;
		g_zoneNextFind = Now + kZoneRefreshSecs;

		auto World = GetWorld();
		if (!World)
			return;

#ifndef OGFN_HARNESS
		TryParseRingLadder();
#endif
		TArray<AActor*> Found = UGameplayStatics::GetAllActorsOfClass(World, AFortSafeZoneIndicator::StaticClass());
		auto Zone = Found.Num() > 0 ? Cast<AFortSafeZoneIndicator>(Found.at(0)) : nullptr;
		g_zoneActor = (Zone && Zone->IsValidLowLevel() && !Zone->IsActorBeingDestroyed()) ? Zone : nullptr;
	}

	void RefreshLoot(double Now)
	{
		if (!g_lootScanned || Now < g_lootNextRefresh)
			return;
		g_lootScanned = true;
		g_lootNextRefresh = Now + kLootRefreshSecs;

		auto World = GetWorld();
		if (!World)
			return;

		auto PickupClass = FindObject<UClass>(L"/Script/FortniteGame.FortPickup");
		if (!PickupClass)
			return;

		TArray<AActor*> Found = UGameplayStatics::GetAllActorsOfClass(World, PickupClass);
		g_lootPositions.clear();
		g_lootPositions.reserve((size_t)Found.Num());
		for (int i = 0; i < Found.Num(); i++)
			g_lootPositions.push_back(Found.at(i)->GetActorLocation());
	}

	void ScanWeaponsOnce()
	{
		if (g_weaponsScanned)
			return;
		g_weaponsScanned = true;

		auto WidClass = FindObject<UClass>(L"/Script/FortniteGame.FortWorldItemDefinition");
		if (!WidClass)
		{
			LOG_ERROR(LogBots, "[ogfn] FortWorldItemDefinition class missing, bots will fight bare-handed");
			return;
		}

		// NOTE: GetAllObjectsOfClass<T> upstream has a dead-broken template body
		// (push_back(UObject*) into vector<T*>) that had never been instantiated
		// until this code; use the proven non-template overload and cast ourselves.
		auto AllObjects = GetAllObjectsOfClass(WidClass);
		std::vector<UFortWorldItemDefinition*> AllWids;
		AllWids.reserve(AllObjects.size());
		for (auto* Object : AllObjects)
			if (auto* Wid = Cast<UFortWorldItemDefinition>(Object))
				AllWids.push_back(Wid);

		std::string dump;
		for (auto Wid : AllWids)
		{
			auto Path = Wid->GetPathName();
			if (Path.find("WID_") == std::string::npos)
				continue;

			dump += Path + "\n";
			if (Path.find("Assault") != std::string::npos)
				g_weaponCands.push_back(Wid);
		}

		if (g_weaponCands.empty())
		{
			// No recognizable AR: fall back to any weapon WID so bots still arm.
			for (auto Wid : AllWids)
			{
				auto Path = Wid->GetPathName();
				if (Path.find("WID_") != std::string::npos && Path.find("Pickaxe") == std::string::npos)
					g_weaponCands.push_back(Wid);
			}
		}

		if (!g_dumpedWids)
		{
			g_dumpedWids = true;
			FILE* f = nullptr;
			if (fopen_s(&f, "C:\\ogfn\\widlist.txt", "w") == 0 && f)
			{
				fputs(dump.c_str(), f);
				fclose(f);
			}
		}

		LOG_INFO(LogBots, "[ogfn] weapon candidates: {}", (int)g_weaponCands.size());
	}

	bool TryArm(Mind& M, AFortPlayerControllerAthena* Controller)
	{
		ScanWeaponsOnce();
		if (g_weaponCands.empty())
			return false;

		auto WorldInventory = Controller->GetWorldInventory();
		if (!WorldInventory)
			return false;

		// Stable per-controller pick so the lobby ends up with a mix of rifles.
		size_t idx = ((uintptr_t)Controller) % g_weaponCands.size();
		bool bShouldUpdate = false;
		auto Res = WorldInventory->AddItem(g_weaponCands[idx], &bShouldUpdate, 1);
		if (bShouldUpdate)
			WorldInventory->Update();

		if (Res.first.empty())
			return false;

		auto Instance = Res.first.front();
		AFortPlayerController::ServerExecuteInventoryItemHook(Controller, Instance->GetItemEntry()->GetItemGuid());
		M.armGuid = Instance->GetItemEntry()->GetItemGuid(); // stored for equip-only rebinds (pawn swaps)

		LOG_INFO(LogBots, "[ogfn] armed: {}", g_weaponCands[idx]->GetPathName());
		M.bArmed = true;
		return true;
	}

	// Deployable-ground probe: straight down first, then march toward world origin
	// (island center) until z >= 0. The bus-end dump column is wherever the forced
	// flight stopped, and one live run put ALL 50 bots on the ocean floor at
	// z=-400uu, where every horizontal trace is blocked and nobody ever fights.
	// Returns true when it found real land (z >= 0) inside the march; the fallback
	// column is the island center, land by construction.
	bool ProbeLand(UObject* Ctx, const FVector& Top, FVector& OutLand)
	{
		static const float kMarch[5] = { 0.f, 0.3f, 0.6f, 0.85f, 1.f };
		FVector best(0.f, 0.f, -1e9f);
		bool bBest = false;
		for (float t : kMarch)
		{
			float x = Top.X + (0.f - Top.X) * t;
			float y = Top.Y + (0.f - Top.Y) * t;
			FHitResult* Hit = nullptr;
			UKismetSystemLibrary::LineTraceSingle(Ctx,
				FVector(x, y, Top.Z), FVector(x, y, Top.Z - 400000.f),
				ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
				EDrawDebugTrace::None, true, FLinearColor(), FLinearColor(), 0.f, &Hit);
			if (Hit && Hit->IsBlockingHit())
			{
				FVector L = Hit->GetLocation();
				if (L.Z >= 0.f && L.Z < 25000.f)
				{
					OutLand = FVector(L.X, L.Y, L.Z + 100.f);
					return true;
				}
				if (L.Z > best.Z)
				{
					best = L;
					bBest = true;
				}
			}
		}
		if (bBest)
		{
			OutLand = FVector(best.X, best.Y, best.Z + 100.f);
			return false;
		}
		OutLand = FVector(0.f, 0.f, 1000.f);
		return false;
	}

	// Real line of sight between eye heights. The brain's perception is the only consumer,
	// so this is the single place wallhack could leak in -- it must stay a trace.
	// CONTEXT MATTERS: the WorldContextObject must be the observing PAWN, not the world.
	// With GetWorld() as context, bIgnoreSelf has no actor to ignore and the trace starts
	// inside our own capsule: every LOS check self-hits and returns false (live evidence:
	// bots in open sky with los=0 forever, so no target ever became shootable). The fork's
	// own trace call (BGA.h) passes the acting actor for exactly this reason.
	bool CanSee(AFortPlayerPawnAthena* Self, FVector A, FVector B)
	{
		FVector S(A.X, A.Y, A.Z + kTraceEyeOffset);
		FVector E(B.X, B.Y, B.Z + kTraceEyeOffset);
		float dx = E.X - S.X, dy = E.Y - S.Y, dz = E.Z - S.Z;
		float L = std::sqrt(dx * dx + dy * dy + dz * dz);
		// STOP SHORT of the target: B is the neighbor's capsule CENTER, so an
		// eye-to-eye trace ENDS inside their body and reports blocked for every
		// proximate pawn. That was the entire live los=0 history (stacked pawns at
		// the bus, ring neighbors on the mountain: every candidate is by definition
		// a nearby pawn, so every trace ended in a capsule). 150uu short of center
		// clears their capsule; for nose-to-nose range the probe stops at half
		// distance instead (a wall between two adjacent pawns still crosses it,
		// where an outright "visible" shortcut fired through the wall).
		float t = (L > 200.f) ? (L - 150.f) / L : 0.5f;
		FVector End(S.X + dx * t, S.Y + dy * t, S.Z + dz * t);
		FHitResult* Hit = nullptr;
		UKismetSystemLibrary::LineTraceSingle(Self, S, End,
			ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
			EDrawDebugTrace::None, /*bIgnoreSelf*/ true, FLinearColor(), FLinearColor(),
			0.f, /*OutHit*/ &Hit);
		return !Hit || !Hit->IsBlockingHit();
	}

	ogfn::WorldSnapshot BuildSnapshot(Mind& M, AFortPlayerPawnAthena* Pawn,
	                                  int enemiesSeenWithin30, double Now)
	{
		ogfn::WorldSnapshot W;
		W.time = Now;
		W.self_pos = ToVec(Pawn->GetActorLocation());
		W.self_health = Pawn->GetHealth();
		W.self_shield = Pawn->GetShield();
		W.self_yaw_deg = Pawn->GetActorRotation().Yaw;
		W.has_weapon = M.bArmed;
		W.shield_potions = 0; // stock bots carry none yet; engine grant is next pass

		// ZONE TRUTH HIERARCHY: (1) the engine's own storm ladder parsed from its
		// log -- its innermost center lies inside every ring, so it is always a
		// safe destination; (2) a resolvable engine radius field (>10000uu); (3)
		// JUNK: hold position. Matches 1/2: the 3.5 indicator sits at (0,0,0)
		// with no radius field, and walking at that phantom origin marched the
		// fleet west into terrain/water and then the storm killed them all --
		// user directive: bots must NOT die to the storm.
		auto World = GetWorld();
		auto GS = World ? Cast<AFortGameStateAthena>(World->GetGameState()) : nullptr;
		if (GS && !g_gsOffsetsTried)
		{
			g_gsOffsetsTried = true;
			g_gsNextRadiusOff = GS->GetOffset("NextRadius", false);
			g_gsPhaseOff = GS->GetOffset("SafeZonePhase", false);
			LOG_INFO(LogBots, "[ogfn] zone fields: NextRadius={} SafeZonePhase={}",
			         g_gsNextRadiusOff, g_gsPhaseOff);
		}

		float Radius = 0.f;
		if (GS && g_gsNextRadiusOff != -1)
			Radius = GS->Get<float>(g_gsNextRadiusOff);

		FVector CenterUu(0.f, 0.f, 0.f);
		bool bHaveCenter = false;
		if (g_zoneActor)
		{
			CenterUu = g_zoneActor->GetActorLocation();
			bHaveCenter = true;

			static int s_indRadiusOff = -2; // resolve once against the actor's own class
			if (s_indRadiusOff == -2)
				s_indRadiusOff = g_zoneActor->GetOffset("NextRadius", false);
			if (Radius <= 0.f && s_indRadiusOff != -1)
				Radius = g_zoneActor->Get<float>(s_indRadiusOff);

			float StartShrink = g_zoneActor->GetSafeZoneStartShrinkTime();
			float FinishShrink = g_zoneActor->GetSafeZoneFinishShrinkTime();
			// Seconds until the zone next does something: time-to-shrink while holding,
			// remaining time while shrinking, 0 once settled.
			if (Now < StartShrink)
				W.circle_shrink_in_secs = ogfn::clampf((float)(StartShrink - Now), 0.f, 600.f);
			else if (Now < FinishShrink)
				W.circle_shrink_in_secs = ogfn::clampf((float)(FinishShrink - Now), 0.f, 600.f);
			else
				W.circle_shrink_in_secs = 0.f;
		}

		float FinalRadiusUu = Radius;
		FVector FinalCenterUu = bHaveCenter ? CenterUu
			: FVector(W.self_pos.x * 100.f, W.self_pos.y * 100.f, 0.f);
		bool bTruth = Radius > 10000.f; // engine field or nothing (4000uu = junk fallback)
		if (g_ladderParsed)
		{
			FinalCenterUu = FVector(g_ladderCX, g_ladderCY, 0.f);
			FinalRadiusUu = g_ladderR;
			bTruth = true;
		}
		g_zoneTruth = bTruth;

		if (bTruth)
		{
			W.circle_center_x = ToMeters(FinalCenterUu.X);
			W.circle_center_y = ToMeters(FinalCenterUu.Y);
			W.circle_next_center_x = W.circle_center_x;
			W.circle_next_center_y = W.circle_center_y;
			W.circle_radius = ToMeters(FinalRadiusUu);
			float dx = W.self_pos.x - W.circle_center_x;
			float dy = W.self_pos.y - W.circle_center_y;
			W.distance_to_circle_edge = std::sqrt(dx * dx + dy * dy) - W.circle_radius;
		}
		else
		{
			// No truth: read as safely inside so nothing walks at a phantom target.
			W.circle_center_x = W.self_pos.x;
			W.circle_center_y = W.self_pos.y;
			W.circle_next_center_x = W.self_pos.x;
			W.circle_next_center_y = W.self_pos.y;
			W.circle_radius = 40.f;
			W.distance_to_circle_edge = -50.f;
		}

		int loot30 = 0;
		for (auto& LootPos : g_lootPositions)
		{
			float dx = ToMeters((float)LootPos.X) - W.self_pos.x;
			float dy = ToMeters((float)LootPos.Y) - W.self_pos.y;
			if (dx * dx + dy * dy <= 30.f * 30.f)
				loot30++;
		}
		W.loot_within_30m = loot30;
		W.enemies_within_30m = enemiesSeenWithin30;
		W.enemies_currently_fighting_each_other = 0; // needs damage observation, next pass

		auto* Target = M.brain.perception().nearest(true);
		if (Target)
		{
			W.target_is_visible = Target->visible;
			ogfn::Vec3 D = Target->last_seen_pos - W.self_pos;
			W.target_distance = ogfn::length(D);
			W.target_health = Target->health;
			W.target_is_reloading = false; // engine exposes no reload state for others
		}

		return W;
	}
} // namespace (anonymous)

// Called from Bots::Tick every net tick while Globals::bEnableOgfnBrain is set.
// The stock tick keeps only what the brain deliberately does not own yet: bus deploy,
// thank-the-driver, and its warmup hop (suppressed while brains are on).
void TickAll()
{
	// The ReadyToStartMatch hook stops firing the moment the match commits (which is
	// when a player actually joins), so the old call-site never saw a connected pawn.
	// This runs every net tick; the call is a cheap gated no-op after success.
	OgfnLocker::AutoApplyOnce();

	if (!Globals::bInfiniteAmmo)
	{
		Globals::bInfiniteAmmo = true;
		Globals::bInfiniteMaterials = true;
		LOG_INFO(LogBots, "[ogfn] infinite ammo/materials enabled for the bot arena");
	}

	auto World = GetWorld();
	if (!World)
		return;

	auto& bots = AllPlayerBotsToTick;
	if (bots.empty())
		return;

	if (g_minds.size() > bots.size()) // fleet rebuilt (match restart): drop stale minds
		g_minds.clear();
	g_minds.resize(bots.size());

	double Now = UGameplayStatics::GetTimeSeconds(World);

	// External match control: drop a C:\ogfn\FORCE_BUS.txt file and the server
	// calls StartAircraftPhase on its next tick (the same verb the upstream
	// 'startaircraft' console command uses -- which is unreachable in this build,
	// hence the file). Used by automated soaks, and by you as a force-bus button.
	{
		static bool s_forced = false;
		if (!s_forced)
		{
			FILE* f = nullptr;
			if (fopen_s(&f, "C:\\ogfn\\FORCE_BUS.txt", "r") == 0 && f)
			{
				std::fclose(f);
				DeleteFileA("C:\\ogfn\\FORCE_BUS.txt"); // Win32: fopen/remove are C4996 errors here
				if (auto GM = Cast<AFortGameModeAthena>(World->GetGameMode()))
				{
					GM->StartAircraftPhase();
					s_forced = true;
					LOG_INFO(LogBots, "[ogfn] FORCE_BUS consumed: StartAircraftPhase issued");
				}
			}
		}
	}

	// Island-lobby gate: while warmup, bots stand like real players do -- no observe,
	// think, servo-aim, or traces. This is also what kills the pre-bus spin/rotation
	// audio storm and most of the lobby CPU. Arming and auto-skin still run (they
	// must: island IS warmup, and that is exactly when your pawn exists).
	bool bWarmup = false;
	EAthenaGamePhase Phase = EAthenaGamePhase::Warmup;
	if (auto GS = Cast<AFortGameStateAthena>(World->GetGameState()))
	{
		Phase = GS->GetGamePhase();
		bWarmup = (Phase == EAthenaGamePhase::Warmup);
	}

	// Combat gate: perception/motor ONLY in SafeZones (post-deploy). Setup is the
	// spawn island: live evidence had bots gunning each other down 1s into Setup,
	// which players cannot do. Arming, skin, zone/loot cache still run all phases.
	bool bCombat = (Phase == EAthenaGamePhase::SafeZones);
	{
		static bool s_actOpen = false;
		if (bCombat && !s_actOpen)
		{
			s_actOpen = true;
			LOG_INFO(LogBots, "[ogfn] combat gate open at SafeZones: perception+motor live");

			// Movement diagnosis (one-shot): frozen vel + frozen pos with inputs
			// flowing means CharacterMovement never integrates. Identify the
			// component, ask it for Walking mode, and remember the verb for
			// per-pawn application after each deploy.
			if (!bots.empty() && bots[0].Controller)
			{
				auto P0 = Cast<AFortPlayerPawnAthena>(bots[0].Controller->GetPawn());
				auto M0Off = P0 ? P0->GetOffset("CharacterMovement", false) : -1;
				auto Move = (M0Off != -1) ? P0->Get<UObject*>(M0Off) : nullptr;
				if (Move)
				{
					g_moveCompSetModeFn = Move->FindFunction("SetMovementMode");
					LOG_INFO(LogBots, "[ogfn] movecomp class={} SetMovementModeFn={}",
						Move->GetPathName(), (__int64)g_moveCompSetModeFn);
					if (g_moveCompSetModeFn)
					{
						// TEnumAsByte<EMovementMode> at 0 (Walking=1), custom mode at 1.
						alignas(16) unsigned char p[8] = {};
						p[0] = 1;
						Move->ProcessEvent(g_moveCompSetModeFn, p);
						LOG_INFO(LogBots, "[ogfn] SetMovementMode(Walking) sent to bot0");
					}

				// Zone resolver probe (one-shot): the engine log picked ring1
				// Radius=80000 at (69368,-76410) while our snapshot reads a 40m
				// ring at origin. Dump which candidate property carries radius on
				// this build's indicator and GameState, and where the actor sits.
				auto GSz = Cast<AFortGameStateAthena>(World->GetGameState());
				static const char* kZoneCands[] = { "NextRadius", "Radius", "CurrentRadius",
					"ZoneRadius", "NextLocation", "NextCenter" };
				for (auto Cname : kZoneCands)
				{
					int gOff = GSz ? GSz->GetOffset(Cname, false) : -1;
					int iOff = g_zoneActor ? g_zoneActor->GetOffset(Cname, false) : -1;
					float gVal = (gOff != -1) ? GSz->Get<float>(gOff) : -999999.f;
					float iVal = (iOff != -1) ? g_zoneActor->Get<float>(iOff) : -999999.f;
					LOG_INFO(LogBots, "[ogfn] zone probe {}: gs={:.0f} ind={:.0f}", Cname, gVal, iVal);
				}
				if (g_zoneActor)
				{
					auto ZL = g_zoneActor->GetActorLocation();
					LOG_INFO(LogBots, "[ogfn] zone probe: indicator at ({:.0f},{:.0f},{:.0f})",
						ZL.X, ZL.Y, ZL.Z);
				}
				else
				{
					LOG_ERROR(LogBots, "[ogfn] zone probe: NO indicator actor found");
				}
				}
			}
		}
	}

	if (!bWarmup)
	{
		RefreshZone(Now);
		RefreshLoot(Now);
	}

	// Respawn wave (post-burst fix): at SafeZones entry the engine restarts EVERY
	// bot at ONE transform (the bus-end column). The first ~5 spawn, the rest die
	// on spawn collision ("failed to spawn a pawn", live Enter=50/Exit=5) and sit
	// pawnless for the whole match, which is why nobody fights. Re-run the engine's
	// own restart for the pawnless ones with per-bot golden-angle ring transforms
	// probed to the ground, every 2s while any remain, radius jittering on retries.
	// A missed ground probe spawns high on purpose: the auto-land block below then
	// places that pawn (bAutoLanded only latches for ground spawns).
	if (Phase == EAthenaGamePhase::SafeZones)
	{
		static double s_nextWave = 0.0;
		static int s_waveAttempt = 0;
		static bool s_gaveUpLogged = false;
		if (Now >= s_nextWave)
		{
			s_nextWave = Now + kWaveCadenceSecs;

			// Ring anchor: prefer a pawn still at bus altitude (the dump column),
			// else any usable fleet pawn, else the observed default column.
			FVector Anchor(137200.f, -92794.f, 81536.f);
			bool bAnchor = false;
			for (auto& AB : bots)
			{
				if (!AB.Controller || !AB.Controller->IsValidLowLevel())
					continue;
				auto AP = Cast<AFortPlayerPawnAthena>(AB.Controller->GetPawn());
				if (!PawnUsable(AP))
					continue;
				auto L = AP->GetActorLocation();
				if (!bAnchor)
				{
					Anchor = L;
					bAnchor = true;
				}
				if (L.Z > 20000.f)
				{
					Anchor = L;
					break;
				}
			}

			auto GMWave = Cast<AFortGameModeAthena>(World->GetGameMode());
			if (GMWave)
			{
				int pawnless = 0, spawned = 0;

				FVector WaveLand(0.f, 0.f, 1000.f);
				ProbeLand(GMWave, FVector(Anchor.X, Anchor.Y, Anchor.Z + 3000.f), WaveLand);
				static bool s_anchorLogged = false;
				if (!s_anchorLogged)
				{
					s_anchorLogged = true;
					LOG_INFO(LogBots, "[ogfn] wave anchor=({:.0f},{:.0f},{:.0f}) land=({:.0f},{:.0f},{:.0f})",
						Anchor.X, Anchor.Y, Anchor.Z, WaveLand.X, WaveLand.Y, WaveLand.Z);
				}

				for (size_t wi = 0; wi < bots.size(); wi++)
				{
					auto& WB = bots[wi];
					if (!WB.Controller || !WB.Controller->IsValidLowLevel())
						continue;
					auto WC = Cast<AFortPlayerControllerAthena>(WB.Controller);
					if (!WC)
						continue;
					if (PawnUsable(Cast<AFortPlayerPawnAthena>(WC->GetPawn())))
						continue;
					if (g_minds[wi].bSeenAlive)
						continue; // died mid-match: elimination is final, no resurrection
					pawnless++;
					if (s_waveAttempt >= kWaveMaxAttempts)
						continue;

					float WAngle = (float)wi * 2.399963f; // golden angle, same family as auto-land
					float WRadius = 500.f + (float)(wi % 5) * 230.f + (float)s_waveAttempt * 137.f;
					FVector Ring(Anchor.X + std::cos(WAngle) * WRadius,
					             Anchor.Y + std::sin(WAngle) * WRadius, Anchor.Z);

					// Probe THE SPAWN'S OWN ring column (local floor under this exact
					// point), with the ocean march as fallback.
					FVector SpawnAt(0.f, 0.f, 0.f);
					FVector RingCol(Ring.X, Ring.Y, Anchor.Z + 3000.f);
					ProbeLand(WC, RingCol, SpawnAt);

					FTransform Xf{};
					Xf.Rotation = FQuat(0.f, 0.f, 0.f, 1.f);
					Xf.Translation = SpawnAt;
					Xf.Scale3D = FVector(1.f, 1.f, 1.f);
					GMWave->RestartPlayerAtTransform(WC, Xf);

					auto Fresh = Cast<AFortPlayerPawnAthena>(WC->GetPawn());
					if (PawnUsable(Fresh))
					{
						spawned++;
						if (SpawnAt.Z <= 20000.f)
							g_minds[wi].bAutoLanded = true; // on the ground already; else auto-land picks it up
						g_minds[wi].bNeedsReequip = true;  // controller keeps the island rifle; bind it to this pawn
						g_minds[wi].bElimLogged = false;
						LOG_INFO(LogBots, "[ogfn] wave respawned bot#{} at ({:.0f},{:.0f},{:.0f})",
							(int)wi, SpawnAt.X, SpawnAt.Y, SpawnAt.Z);
					}
				}
				if (pawnless > 0)
				{
					s_waveAttempt++;
					LOG_INFO(LogBots, "[ogfn] respawn wave: pawnless={} spawned={} attempt={}",
						pawnless, spawned, s_waveAttempt);
					if (spawned == 0 && s_waveAttempt >= kWaveMaxAttempts && !s_gaveUpLogged)
					{
						s_gaveUpLogged = true;
						LOG_ERROR(LogBots, "[ogfn] respawn wave GAVE UP: {} pawns still missing after {} attempts",
							pawnless, s_waveAttempt);
					}
				}
				else
				{
					s_waveAttempt = 0;
				}
			}
		}
	}

	// Jump probe verdict (per tick, NOT on the 15s track cadence): 60 ticks
	// after sending Jump to wave pawn#5, did its Z ever move? Answers whether
	// CharacterMovement integrates anything at all.
	{
		static bool s_jumpProbe = false;
		static int s_jumpFollow = 0;
		static float s_jumpZ0 = 0.f;
		static float s_jumpDzMax = 0.f;
		if (bCombat && bots.size() > 5 && bots[5].Controller)
		{
			auto P5 = Cast<AFortPlayerPawnAthena>(bots[5].Controller->GetPawn());
			if (PawnUsable(P5))
			{
				float Z5 = P5->GetActorLocation().Z;
				if (!s_jumpProbe)
				{
					s_jumpProbe = true;
					s_jumpFollow = 60;
					s_jumpZ0 = Z5;
					ApplyJump(P5);
					LOG_INFO(LogBots, "[ogfn] jump probe: sent Jump verb to wave pawn#5");
				}
				else if (s_jumpFollow > 0)
				{
					s_jumpFollow--;
					float dz = Z5 - s_jumpZ0;
					if (dz > s_jumpDzMax)
						s_jumpDzMax = dz;
					if (s_jumpFollow == 0)
						LOG_INFO(LogBots, "[ogfn] jump probe outcome: dzmax={:.1f}uu over 2s", s_jumpDzMax);
				}
			}
		}
	}

	// Fleet-wide match summary every 15s: the line the match report is built from
	// (alive vs deployed vs eliminated, cumulative fire+move, live bbox spread).
	{
		static double s_nextFleet = 0.0;
		if (Now >= s_nextFleet)
		{
			s_nextFleet = Now + 15.0;
			int alive = 0, deployed = 0, elim = 0, evac = 0;
			long long firedSum = 0, movSum = 0, walkOk = 0, walkBlk = 0;
			float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
			for (size_t fi = 0; fi < bots.size(); fi++)
			{
				auto& FB = bots[fi];
				if (!FB.Controller || !FB.Controller->IsValidLowLevel())
					continue;
				auto FP = Cast<AFortPlayerPawnAthena>(FB.Controller->GetPawn());
				if (PawnUsable(FP))
				{
					alive++;
					auto FL = FP->GetActorLocation();
					float mx = (float)FL.X * 0.01f, my = (float)FL.Y * 0.01f;
					if (mx < minx) minx = mx;
					if (mx > maxx) maxx = mx;
					if (my < miny) miny = my;
					if (my > maxy) maxy = my;
				}
				if (fi < g_minds.size())
				{
					if (g_minds[fi].bAutoLanded) deployed++;
					if (g_minds[fi].bElimLogged) elim++;
					firedSum += g_minds[fi].fireCount;
					movSum += g_minds[fi].moveCount;
					walkOk += g_minds[fi].stepOk;
					walkBlk += g_minds[fi].stepBlk;
					if (g_minds[fi].bStormEvac) evac++;
				}
			}
			if (alive == 0)
				minx = maxx = miny = maxy = 0.f;
			LOG_INFO(LogBots, "[ogfn] fleet t={:.0f} alive={} deployed={} elim={} evac={} fired={} mov={} walk_ok={} walk_blk={} truth={} ladder={} bbox=({:.0f}..{:.0f},{:.0f}..{:.0f})m",
				Now, alive, deployed, elim, evac, firedSum, movSum, walkOk, walkBlk,
				(int)g_zoneTruth, (int)g_ladderParsed, minx, maxx, miny, maxy);
		}
	}

	// Enemy pawn set for this pass: every other fleet bot plus every human client
	// pawn, read FRESH from the controller each tick (respawns replace the cached
	// PlayerBot.Pawn pointer, which never gets refreshed upstream).
	static std::vector<EnemyPawn> Enemies;
	Enemies.clear();
	Enemies.reserve(bots.size() + 4);
	if (bCombat)
	{
		for (auto& Bot : bots)
		{
			if (!Bot.Controller || !Bot.Controller->IsValidLowLevel())
				continue;
			auto P = Cast<AFortPlayerPawnAthena>(Bot.Controller->GetPawn());
			if (PawnUsable(P))
				Enemies.push_back({ PawnId(P), P });
		}
		auto NetDriver = World->GetNetDriver();
		if (NetDriver)
		{
			auto& Connections = NetDriver->GetClientConnections();
			for (int i = 0; i < Connections.Num(); i++)
			{
				auto PC = Cast<AFortPlayerController>(Connections.At(i)->GetPlayerController());
				if (!PC || !PC->IsValidLowLevel())
					continue;
				auto HumanPawn = Cast<AFortPlayerPawnAthena>(PC->GetMyFortPawn());
				if (PawnUsable(HumanPawn))
					Enemies.push_back({ PawnId(HumanPawn), HumanPawn });
			}
		}

		static EAthenaGamePhase s_gatherPhase = (EAthenaGamePhase)-1;
		if (Phase != s_gatherPhase)
		{
			s_gatherPhase = Phase;
			if (Enemies.empty())
				LOG_ERROR(LogBots, "[ogfn] no usable enemy pawns for perception (phase change, empty)");
		}
	}

	for (size_t i = 0; i < bots.size(); i++)
	{
		auto& Bot = bots[i];
		if (!Bot.Controller || !Bot.Controller->IsValidLowLevel())
			continue;
		auto Controller = Cast<AFortPlayerControllerAthena>(Bot.Controller);
		if (!Controller)
			continue;
		auto Pawn = Cast<AFortPlayerPawnAthena>(Controller->GetPawn()); // fresh: respawns replace the cached pointer
		if (!PawnUsable(Pawn))
		{
			// Deployed bot lost its pawn: in Athena that is elimination (no
			// respawn mid-match). One line per bot for the match report. The
			// wave runs earlier in this same tick, so a burst victim is already
			// rescued before this gate is reached; only real deaths land here.
			if (Phase == EAthenaGamePhase::SafeZones && i < g_minds.size()
				&& g_minds[i].bAutoLanded && !g_minds[i].bElimLogged)
			{
				g_minds[i].bElimLogged = true;
				LOG_INFO(LogBots, "[ogfn] ELIMINATED bot#{} t={:.0f}", (int)i, Now);
			}
			continue;
		}

		Mind& M = g_minds[i];
		// COMBAT-PHASE only: on the island every bot is alive, and marking them
		// seen-alive there made the death gate treat all 45 burst victims as
		// killed bots -- the rescue wave skipped every one of them (live match 1:
		// deployed=5, respaw wave lines=0). Burst victims are by definition never
		// seen alive in combat, killed bots are.
		if (bCombat)
			M.bSeenAlive = true;
		if (!M.bSeeded)
		{
			uintptr_t key = (uintptr_t)Pawn;
			uint64_t seed = (uint64_t)key * 2654435761ull ^ (uint64_t)(i + 1);
#ifdef OGFN_HARNESS
			// Deterministic seeds in the behavior sim: ASLR pointers made personas
			// differ per run and P2 occasionally rolled a persona whose trigger
			// discipline never opened inside the 6s window (intermittent P2 FAIL).
			seed = 0x9E3779B97F4A7C15ull + (uint64_t)(i + 1) * 0x100000001B3ull;
#endif
			float skill = (float)((key >> 3) % 40) / 39.f - 0.5f; // spread over [-0.5, 0.5]
			M.Reseed(seed, skill);
			M.bSeeded = true;
			M.lastTick = Now;
			if (!g_summaryLogged)
			{
				g_summaryLogged = true;
				LOG_INFO(LogBots, "[ogfn] brains online: first fleet size {}", (int)bots.size());
			}
		}

		if (!g_verbs.bResolved)
			ResolveVerbs(Pawn);
		if (!g_verbs.bFireResolved)
			ResolveFire(Controller, Pawn);

		// Auto-land (bus-dump fix): on this build the engine's forced bus-end
		// exit leaves server-only pawns frozen at the bus's last transform
		// (live evidence: pos == bus loc to the millimeter, velZ=0, no attach,
		// never descends, so combat never happens). Place the pawn on the ground
		// under the drop column ourselves, once per pawn, spread on a golden-angle
		// ring so a single-point dump does not become one stacked column of
		// mutually occluded capsules. The probe starts 3000uu above the pawn so an
		// early roof/actor hit cannot pass as ground; a near-start hit just
		// retries after the aircraft actor tears down.
		if (!bWarmup && Phase == EAthenaGamePhase::SafeZones && !M.bAutoLanded)
		{
			auto P = Pawn->GetActorLocation();
			if (P.Z > 20000.f)
			{
				// Ring XY FIRST, then probe THE RING'S OWN COLUMN: using the anchor's
				// floor height for an offset point buries the pawn inside a slope
				// (live: z=1636 anchor on a mountain, ring pawn inside rock, every
				// trace started in geometry -> los=0 even "on land").
				float Angle = (float)i * 2.399963f; // golden angle: no stacking
				float Radius = 400.f + (float)(i % 5) * 220.f;
				FVector Col(P.X + std::cos(Angle) * Radius,
				             P.Y + std::sin(Angle) * Radius, P.Z + 3000.f);
				FVector Land(0.f, 0.f, 0.f);
				bool bRealLand = ProbeLand(Pawn, Col, Land);
				ApplyTeleport(Pawn, Land);
				M.bAutoLanded = true;
				LOG_INFO(LogBots, "[ogfn] auto-landed bot#{} land={} z={:.0f} -> ({:.0f},{:.0f},{:.0f})",
					(int)i, (int)bRealLand, P.Z, Land.X, Land.Y, Land.Z);
			}
			// Ground pawns need no latch: the z>20000 gate alone self-terminates
			// (probe only ever runs for high pawns, and a teleport ends that).
		}

		// STORM-EVAC (hard guarantee for "bots must not die to the storm"): once
		// hurt while outside the FINAL ring, extract to its center. The ladder's
		// innermost center is inside every ring, so the destination is safe in
		// every phase. Fires only when the ladder parsed and hp drops outside --
		// with the truth-walk working this should almost never trigger.
		if (Phase == EAthenaGamePhase::SafeZones && g_ladderParsed && !M.bStormEvac
			&& Pawn->GetHealth() > 0.f && Pawn->GetHealth() < 75.f)
		{
			auto P = Pawn->GetActorLocation();
			float ex = (float)P.X - g_ladderCX;
			float ey = (float)P.Y - g_ladderCY;
			float limit = g_ladderR + 300.f;
			if (ex * ex + ey * ey > limit * limit)
			{
				float ang = (float)i * 2.399963f;
				float rr = 120.f + (float)(i % 5) * 90.f;
				FVector Dest(g_ladderCX + std::cos(ang) * rr,
				             g_ladderCY + std::sin(ang) * rr, (float)P.Z);
				FHitResult* Gh = nullptr;
				UKismetSystemLibrary::LineTraceSingle(Pawn,
					FVector(Dest.X, Dest.Y, Dest.Z + 3000.f),
					FVector(Dest.X, Dest.Y, Dest.Z - 40000.f),
					ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
					EDrawDebugTrace::None, true, FLinearColor(), FLinearColor(), 0.f, &Gh);
				if (Gh && Gh->IsBlockingHit())
				{
					FVector FL = Gh->GetLocation();
					if (FL.Z >= Dest.Z - 40000.f && FL.Z <= Dest.Z + 3000.f)
						Dest.Z = FL.Z + 100.f;
				}
				ApplyTeleport(Pawn, Dest);
				M.bStormEvac = true;
				LOG_INFO(LogBots, "[ogfn] STORM-EVAC bot#{} hp={:.0f} -> ({:.0f},{:.0f},{:.0f})",
					(int)i, Pawn->GetHealth(), Dest.X, Dest.Y, Dest.Z);
			}
		}
		if (M.bNeedsReequip)
		{
			M.bNeedsReequip = false;
			AFortPlayerController::ServerExecuteInventoryItemHook(Controller, M.armGuid);
			LOG_INFO(LogBots, "[ogfn] re-equipped rifle on new pawn (bot#{})", (int)i);
		}
		if (!M.bArmed && Now >= M.nextArmTry)
		{
			if (!TryArm(M, Controller))
				M.nextArmTry = Now + 2.0;
		}

		if (!bCombat)
		{
			M.lastTick = Now; // keep dt sane for the first combat tick
			continue;         // island (Warmup/Setup): stand still, no servo, no fire
		}

		float dt = (float)(Now - M.lastTick);
		M.lastTick = Now;
		if (dt < 1.f / 240.f)
			dt = 1.f / 240.f;
		if (dt > 0.1f)
			dt = 0.1f;

		// Observation: range + FOV prefilter first (no traces), sort by range, then
		// a HARD trace budget. At 50 bots an unbounded 50x50 LOS matrix at 30 Hz is
		// what ate the gaming rig (user measured 50 -> 200 fps when the server died).
		// Policy: nearest 2 always, anything the mind already tracks, and at most
		// 6 total traces/bot/tick with stale candidates re-probed on a 3-tick rotation.
		ogfn::Observation Obs;
		Obs.self_pos = ToVec(Pawn->GetActorLocation());
		Obs.self_yaw_deg = Pawn->GetActorRotation().Yaw;

		auto& Cfg = M.brain.perception().config();
		const float CosHalfFov = std::cos(ogfn::deg2rad(Cfg.fov_half_deg));
		const float YawRad = ogfn::deg2rad(Obs.self_yaw_deg);
		const float FwdX = std::cos(YawRad), FwdY = std::sin(YawRad);
		FVector SelfLoc = Pawn->GetActorLocation();
		int enemiesSeenWithin30 = 0;

		struct Cand { float dist = 0.f; FVector loc; int id = 0; AFortPlayerPawnAthena* pawn = nullptr; };
		std::vector<Cand> Cands;
		Cands.reserve(Enemies.size());
		for (auto& E : Enemies)
		{
			if (E.pawn == Pawn || !PawnUsable(E.pawn))
				continue;
			FVector Loc = E.pawn->GetActorLocation();
			FVector D = Loc - SelfLoc;
			float dxh = (float)D.X, dyh = (float)D.Y;
			float hlen = std::sqrt(dxh * dxh + dyh * dyh); // engine uu (traces and sort)
			if (hlen < 1.f || hlen * 0.01f > Cfg.max_range)
				continue; // range gate is METERS: raw uu comparison gated at 0.9m and
			              // silently hid every enemy outside arm's reach (the live n=0).
			float facing = (dxh / hlen) * FwdX + (dyh / hlen) * FwdY;
			if (facing < CosHalfFov)
				continue;
			Cands.push_back({ hlen, Loc, E.id, E.pawn });
		}
		std::sort(Cands.begin(), Cands.end(),
			[](const Cand& x, const Cand& y) { return x.dist < y.dist; });

		static int s_probeTick = 0;
		s_probeTick++;
		int traces = 0;
		constexpr int kMaxTraces = 6;
		for (size_t ci = 0; ci < Cands.size() && traces < kMaxTraces; ci++)
		{
			auto& C = Cands[ci];
			bool bTracked = M.brain.perception().by_id(C.id) != nullptr;
			bool bNearest2 = ci < 2;
			bool bStaggered = ((s_probeTick + (int)ci) % 3) == 0;
			if (!bTracked && !bNearest2 && !bStaggered)
				continue;

			bool bLos = CanSee(Pawn, SelfLoc, C.loc);
			traces++;

			ogfn::EnemySample Sample;
			Sample.id = C.id;
			Sample.pos = ToVec(C.loc);
			Sample.health = C.pawn->GetHealth();
			Sample.line_of_sight = bLos;
			Obs.enemies.push_back(Sample);
			if (bLos && C.dist <= 3000.f) // 30 m in engine uu
				enemiesSeenWithin30++;
		}

		M.brain.observe(Obs, Now);

		ogfn::WorldSnapshot W = BuildSnapshot(M, Pawn, enemiesSeenWithin30, Now);
		M.decision = M.brain.think(W);

		{
			static EAthenaGamePhase s_obsPhase = (EAthenaGamePhase)-1;
			static size_t s_obsBotIdx = 0;
			static double s_nextTrack = 0.0;
			if (&M == &g_minds[0] && Now >= s_nextTrack)
			{
				s_nextTrack = Now + 15.0;

				// Why is bot0 frozen at bus altitude? Three facts per sample:
				// root-scene-component AttachParent (parked on the bus?), the movement
				// component's Z velocity (physics ticking?), and the aircraft actor's
				// own location (is the pawn == the bus?).
				bool bAttached = false;
				float velZ = -999999.f;
				auto RootOff = Pawn->GetOffset("RootComponent", false);
				if (RootOff != -1)
				{
					if (auto Root = Pawn->Get<UObject*>(RootOff))
					{
						auto ParentOff = Root->GetOffset("AttachParent", false);
						if (ParentOff != -1)
							bAttached = Root->Get<UObject*>(ParentOff) != nullptr;
					}
				}
				FVector VelAll;
				int MoveMode = -1;
				UObject* MoveComp = nullptr;
				auto MoveOff = Pawn->GetOffset("CharacterMovement", false);
				if (MoveOff != -1)
					MoveComp = Pawn->Get<UObject*>(MoveOff);
				if (MoveComp)
				{
					auto VelOff = MoveComp->GetOffset("Velocity", false);
					if (VelOff != -1)
					{
						VelAll = MoveComp->Get<FVector>(VelOff);
						velZ = VelAll.Z;
					}
					auto ModeOff = MoveComp->GetOffset("MovementMode", false);
					if (ModeOff != -1)
						MoveMode = (int)*(unsigned char*)((char*)MoveComp + ModeOff);
				}

				// Second tracked pawn: a WAVE spawn (bot#5). bot0 is teleported by
				// auto-land; if only bot0 is frozen, the teleport is the suspect; if
				// both are frozen, movement never simulates for server pawns at all.
				FVector P5Loc(0.f, 0.f, 0.f);
				bool bGotP5 = false;
				float P5Z = 0.f;
				if (bots.size() > 5 && bots[5].Controller)
				{
					auto P5 = Cast<AFortPlayerPawnAthena>(bots[5].Controller->GetPawn());
					if (PawnUsable(P5))
					{
						P5Loc = P5->GetActorLocation();
						P5Z = P5Loc.Z;
						bGotP5 = true;

					}
				}

				FVector BusLoc;
				bool bGotBus = false;
				if (auto GS = Cast<AFortGameStateAthena>(World->GetGameState()))
				{
					int airOff = GS->GetOffset("Aircrafts", false);
					if (airOff != -1)
					{
						auto Aircrafts = GS->GetPtr<TArray<AActor*>>(airOff);
						if (Aircrafts && Aircrafts->Num() > 0)
						{
							auto Bus = Aircrafts->at(0);
							if (Bus)
							{
								BusLoc = Bus->GetActorLocation();
								bGotBus = true;
							}
						}
					}
					else
					{
						int oneOff = GS->GetOffset("Aircraft", false);
						if (oneOff != -1)
						{
							if (auto Bus = GS->Get<AActor*>(oneOff))
							{
								BusLoc = Bus->GetActorLocation();
								bGotBus = true;
							}
						}
					}
				}

				// Synthetic LOS sanity probe: self straight up into empty sky.
				// If THIS reports blocked, the trace pipeline itself is broken.
				FHitResult* SkyHit = nullptr;
				UKismetSystemLibrary::LineTraceSingle(Pawn,
					SelfLoc + FVector(0, 0, 200.f), SelfLoc + FVector(0, 0, 1500.f),
					ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
					EDrawDebugTrace::None, true, FLinearColor(), FLinearColor(), 0.f, &SkyHit);
				bool bSkyLos = !SkyHit || !SkyHit->IsBlockingHit();

				FVector ZTrk(0.f, 0.f, 0.f);
				if (g_zoneActor)
					ZTrk = g_zoneActor->GetActorLocation();
				LOG_INFO(LogBots, "[ogfn] track z={:.0f} pos=({:.0f},{:.0f}) yaw={:.0f} n={} los={} shootable={} fired={} mov={} mode={} vel=({:.0f},{:.0f},{:.0f}) p5=({:.0f},{:.0f},{:.0f}) zone=({:.0f},{:.0f}) skyLOS={}",
					W.self_pos.z, W.self_pos.x, W.self_pos.y, Obs.self_yaw_deg,
					(int)Obs.enemies.size(), (int)W.target_is_visible, (int)M.decision.fire,
					M.fireCount, M.moveCount, MoveMode, VelAll.X, VelAll.Y, VelAll.Z,
					bGotP5 ? P5Loc.X * 0.01f : -1.f, bGotP5 ? P5Loc.Y * 0.01f : -1.f, P5Z * 0.01f,
					ZTrk.X, ZTrk.Y, (int)bSkyLos);
			}
			if (&M == &g_minds[0] && Phase != s_obsPhase)
			{
				s_obsPhase = Phase;
				int losCount = 0;
				for (auto& S : Obs.enemies)
					if (S.line_of_sight)
						losCount++;
				LOG_INFO(LogBots, "[ogfn] obs@phase: n={} los={} pos=({:.0f},{:.0f},{:.0f}) edge={:.0f} action={} shootable={}",
					(int)Obs.enemies.size(), losCount, W.self_pos.x, W.self_pos.y, W.self_pos.z,
					W.distance_to_circle_edge, ogfn::to_string(M.decision.action), (int)W.target_is_visible);
				(void)s_obsBotIdx;
			}
		}

#ifdef OGFN_SIM_TRACE
		if (&M == &g_minds[0] && (int)(Now * 10.0) % 6 == 0)
		{
			auto* near = M.brain.perception().nearest(true);
			std::printf("[sim] t=%.2f obs=%d targets=%d near=%d dec=%s move=(%.0f,%.0f,%.0f) fire=%d edge=%.0f vis=%d\n",
				Now, (int)Obs.enemies.size(), (int)M.brain.perception().targets().size(),
				near ? 1 : 0, ogfn::to_string(M.decision.action),
				M.decision.move_to.x, M.decision.move_to.y, M.decision.move_to.z,
				(int)M.decision.fire, W.distance_to_circle_edge, (int)W.target_is_visible);
		}
#endif

		// Motor every tick: aim tracking with servo degradation, or idle scanning.
		auto* Target = M.brain.perception().nearest(true);
		ogfn::MotorOutput Out = Target
			? M.motor.aim_at(W.self_pos, W.self_yaw_deg, Target->last_seen_pos, M.decision.fire, dt)
			: M.motor.idle_look(W.self_yaw_deg, dt);

#ifdef OGFN_SIM_TRACE
		if (&M == &g_minds[0] && (int)(Now * 10.0) % 6 == 0)
		{
			std::printf("[sim] motor t=%.2f out_fire=%d aim_err=%.1f tgt=%d\n",
				Now, (int)Out.firing, M.motor.aim_error_deg(), Target ? 1 : 0);
		}
#endif

		ApplyRotation(Pawn, Out.pitch_deg, Out.yaw_deg);

		// KINEMATIC LOCOMOTION on circle-intent decisions (make_intent fills
		// move_to for Engage/Flee/RotateToCircle). CharacterMovement does not
		// tick on this build (30/s AddMovementInput for minutes with velocity
		// frozen at its spawn-inherited value and pos never integrating), while
		// transform writes DO work (rotation servo, deploy placement). So each
		// tick steps the pawn: ground-follow probe from just above head height
		// (climbs slopes and low obstacles, misses tall walls so the sweep stops
		// us), then a bSweep=true teleport the engine resolves against geometry.
		if (M.decision.action == ogfn::ActionKind::RotateToCircle
			|| M.decision.action == ogfn::ActionKind::Engage
			|| M.decision.action == ogfn::ActionKind::Flee)
		{
			const ogfn::Vec3& To = M.decision.move_to;
			float mx = To.x - W.self_pos.x;
			float my = To.y - W.self_pos.y;
			float mlen = std::sqrt(mx * mx + my * my);
			if (mlen > kMoveMinDist)
			{
				float dirx = mx / mlen, diry = my / mlen;
				float step = kBotWalkSpeed * dt; // engine uu per tick
				FVector Cur = Pawn->GetActorLocation();
				FVector Want(Cur.X + dirx * step, Cur.Y + diry * step, Cur.Z);

				// Ground-follow from 300uu above current height: floor within a
				// reachable drop -> rest on it (uphill lifts us onto it,
				// downhill stops the float after one tick).
				FHitResult* G = nullptr;
				UKismetSystemLibrary::LineTraceSingle(Pawn,
					FVector(Want.X, Want.Y, Want.Z + 300.f),
					FVector(Want.X, Want.Y, Want.Z - 40000.f),
					ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
					EDrawDebugTrace::None, true, FLinearColor(), FLinearColor(), 0.f, &G);
				if (G && G->IsBlockingHit())
				{
					FVector FL = G->GetLocation();
					if (FL.Z >= Want.Z - 400.f && FL.Z < Want.Z + 300.f)
						Want.Z = FL.Z + 100.f; // rest on floor, step-up <= head height
				}

				float StepX0 = Cur.X, StepY0 = Cur.Y;
				if (!ApplyTeleportEx(Pawn, Want, /*bSweep*/ true))
				{
					// Blocked: slide like a real character before giving up the
					// tick. Step-up first (fences, curbs, stairs), then side
					// steps at ~55 degrees around the obstacle (a pawn wall-
					// hugging head-on would otherwise be stuck forever).
					bool bMoved = false;
					FVector Up(Want.X, Want.Y, Want.Z + 45.f);
					bMoved = ApplyTeleportEx(Pawn, Up, true);
					if (!bMoved)
					{
						for (int s = 0; s < 2 && !bMoved; s++)
						{
							float ang = (s == 0 ? 0.96f : -0.96f); // ~55 degrees
							float sx = dirx * std::cos(ang) - diry * std::sin(ang);
							float sy = dirx * std::sin(ang) + diry * std::cos(ang);
							FVector Side(Cur.X + sx * step, Cur.Y + sy * step, Want.Z);
							bMoved = ApplyTeleportEx(Pawn, Side, true);
						}
					}
				}
				M.moveCount++;
				{
					auto NowPos = Pawn->GetActorLocation();
					float moved = std::fabs((float)NowPos.X - StepX0) + std::fabs((float)NowPos.Y - StepY0);
					if (moved > 0.5f)
						M.stepOk++;
					else
						M.stepBlk++;
				}
			}
		}

		if (Out.jumping)
			ApplyJump(Pawn);

		if (Out.firing)
		{
			static bool s_firstFire = false;
			if (!s_firstFire)
			{
				s_firstFire = true;
				LOG_INFO(LogBots, "[ogfn] first fire event (motor trigger pulled)");
			}
			M.fireCount++;
			ApplyFire(Pawn);
		}

		// decision.build_wall intentionally unwired: builder.h is the next pass.
	}
}

#ifdef OGFN_HARNESS
void TestSetLadder(float cx, float cy, float r)
{
	g_ladderCX = cx;
	g_ladderCY = cy;
	g_ladderR = r;
	g_ladderParsed = true;
}

void TestClearBurstState(size_t i)
{
	if (i < g_minds.size())
	{
		g_minds[i].bSeenAlive = false;
		g_minds[i].bElimLogged = false;
	}
}
#endif

} // namespace OgfnBot
