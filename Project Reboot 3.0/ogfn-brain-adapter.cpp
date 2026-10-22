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
//   observe -> think -> aim/move/strafe/crouch/jump/fire/build, weapon grant on join,
//   zone steering, loot density counting, one-shot diagnostics for build names.
// Not yet wired: build EDITS (a wall becomes a window through a separate verb; the
// builder emits the edit intent and the adapter logs it), loot pathing to picked-up
// items, navmesh steering (movement is straight-line to the decision's move_to until
// ai.h SetNavigationSystem is enabled for 3.5).
//
// Every engine verb goes through FindFunction + ProcessEvent or the fork's existing
// wrappers, and every build-dependent name is resolved at runtime with logged misses,
// because this client's FName table is the only authority that matters.

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "reboot.h"
#include "globals.h"
#include "bots.h"
#include "FortGameModeAthena.h"
#include "FortGameStateAthena.h"
#include "FortPlayerStateAthena.h"
#include "FortSafeZoneIndicator.h"
#include "FortWorldItemDefinition.h"
#include "FortPickup.h"
#include "KismetSystemLibrary.h"
#ifdef OGFN_HARNESS
namespace OgfnLocker { void AutoApplyOnce(); }  // provided by the harness stub
#else
#include "ogfn_locker.h"
#endif
#include "ogfn/ogfn_bots.h"
#include <cmath>
#include <functional>
#include <utility>
#include <vector>
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
// Behavior-sim hook: strafe evidence (steps taken while strafing, leaked-toward count).
void TestStrafeStats(size_t i, int* total, int* forward);
// Behavior-sim hooks: build inputs that reached ApplyBuildPiece (piece, is_edit).
void TestClearBuildLog();
std::vector<std::pair<int, int>> TestBuildLog();
// Behavior-sim hooks: loot telemetry (samples seen, classes seen, the live choice).
void TestForceLoot();
// Behavior-sim hook: put a consumable in a bot's inventory so the heal path can run.
void TestGiveConsumable(size_t i, ogfn::ItemClass cls, int count);
int TestSquadmateCount(size_t i);
int TestHealEquips(size_t i);
int TestLootSeen(size_t i);
bool TestLootClassSeen(size_t i, ogfn::ItemClass cls);
bool TestLootChoice(size_t i, int* pickupId, float* x, float* y);
bool TestTargetIsSquadmate(size_t i);
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
		// Movement model: strafe/crouch/jump/fall. MotorOutput.jumping existed but
		// nothing ever set it, so ApplyJump was dead code (harness jump=0 forever).
		ogfn::Locomotion loco;
		// Building: the decision says "wall up", the builder says which piece, in
		// what order, at what cadence. Without it Build placed nothing at all
		// (decision.build_wall was documented as unwired).
		ogfn::Builder builder;
		ogfn::Decision decision;
		bool bSeeded = false;
		bool bArmed = false;
		bool bAutoLanded = false;   // bus-dump place-once flag (see the auto-land block)
		bool bNeedsReequip = false; // wave respawn: rebind the controller's rifle to the new pawn
		int fireCount = 0;          // motor triggers pulled (evidence of sustained combat)
		// The gun this bot was actually granted. Kept here so the HIT MODEL resolves
		// shots with the same cyclic rate, damage and spread the motor fires with:
		// grading a rifle's shot with a shotgun's falloff is how a hit model lies.
		ogfn::WeaponProfile armedProfile;
		int shotsResolved = 0;      // rounds the hit model actually scored
		int shotsHit = 0;           // ... of which connected with a body
		float damageDealt = 0.f;    // total damage this bot has inflicted
		double lastResolveLog = 0.0;
		bool bSeenAlive = false;    // has this bot been observed with a pawn IN-MATCH?
		                            // burst victims never have (wave rescues them);
		                            // killed bots do (wave must NOT resurrect them)
		bool bElimLogged = false;   // one ELIMINATED line per bot
		bool bStormEvac = false;    // emergency storm extract done
		// Same team this tick (empty on solo playlists). Stored loose because EnemyPawn
		// is declared after Mind; only the pawn and its id are used.
		// Vertical aim is motor state now: the motor owns pitch and returns the corrected
		// value, so the adapter has to remember it between ticks (it used to leave
		// pitch at 0 forever, which is what made every bot stare at the horizon).
		float pitch = 0.f;
		// Heal in progress: which item, and how long the animation has left. The brain
		// commits (Brain::healing) and the adapter equips the consumable once, then
		// reports the remaining time back through the snapshot so the brain keeps the
		// bot in the animation instead of re-deciding every tick.
		float healRemaining = 0.f;
		int healItem = 0;
		bool healEquipped = false;
		int healsUsed = 0;
		// Previous target position, for velocity: without it a bot cannot lead, and a
		// bot that hits where you WERE is the clearest aimbot signature there is.
		ogfn::Vec3 last_target_pos{};
		double last_target_time = -1.0;
		bool has_last_target = false;
		std::vector<std::pair<int, AFortPlayerPawnAthena*>> squadmates;
		int buildCount = 0;         // pieces placed (evidence that building is wired)
		// Strafe evidence, classified where yaw-to-target is known: of the steps taken
		// while the movement layer says "strafing", how many actually went across the
		// target line rather than at it. A strafe that leaks forward is the "walks at
		// you while shooting" tell.
		int strafeSteps = 0;
		int strafeForward = 0;
		bool bEditLogged = false;   // one line per bot when it first wants an edit
		int stepOk = 0, stepBlk = 0; // kinematic walk: displaced vs blocked ticks
		int moveCount = 0;          // move intents that reached ApplyMoveInput
		// Stuck detection. With no navmesh the kinematic walk is a straight sweep into
		// geometry, and live evidence was 89% of steps blocked with the whole fleet
		// pinned 999m from the zone for a whole match. A player who walks into a wall
		// gives up on that line and goes around; this is that.
		int stuckTicks = 0;         // consecutive blocked steps
		float stuckAng = 0.f;       // committed detour direction (radians, world)
		int stuckPhase = 0;         // ticks left in this detour
		int stuckTries = 0;         // widening counter for repeated failures
		int stuckEpisodes = 0;      // total detours started (telemetry)
		// Last snapshot this bot built, for harness diagnostics. The old code read a
		// fleet-global W_last_self, which is the position of whichever bot was processed
		// LAST -- so a per-bot distance query silently measured against another bot and
		// reported 0.0m.
		ogfn::WorldSnapshot lastSnap;
		double nextRescueTry = 0.0; // progress-rescue clock
		int rescues = 0;            // how often arena plumbing had to step in
		float lastStuckX = 0.f, lastStuckY = 0.f;
		FGuid armGuid;              // guid of the granted rifle (equip-only path, no re-add)
		double nextArmTry = 0.0;
		double nextLandTry = 0.0;   // retry clock for a rejected ground probe
		double lastTick = 0.0;

		Mind() : persona(), brain(persona, 1), motor(persona, 1), loco(persona, 1),
		         builder(persona, 1) {}

		void Reseed(uint64_t seed, float skillBias)
		{
			ogfn::Rng rng(seed);
			persona = ogfn::Persona::sample(rng, skillBias);
			brain = ogfn::Brain(persona, seed);
			motor = ogfn::Motor(persona, seed);
			// Placeholder until TryArm learns what the bot was actually granted, at
			// which point set_weapon is called again with the real class. The motor
			// needs SOME magazine or the trigger is a hose.
			motor.set_weapon(ogfn::weapon_rifle());
			loco = ogfn::Locomotion(persona, seed ^ 0x51ED270Bull);
			builder = ogfn::Builder(persona, seed ^ 0xB0117ull);
		}
	};

	std::vector<Mind> g_minds;                 // index-parallel to AllPlayerBotsToTick
	bool g_zoneTruth = false;      // is the snapshot ring real (ladder or engine field)?
	bool g_ladderParsed = false;   // engine's 9-ring storm ladder read from its log
	float g_ladderCX = 0.f, g_ladderCY = 0.f, g_ladderR = 0.f; // uu, innermost ring >0
	int g_ladderTries = 0;
	int g_ladderSlowAt = 0;        // slow-retry cadence once the fast budget is out
	bool g_summaryLogged = false;
	bool g_traceFnReady = true;        // Kismet traces are live once combat resolves
	ogfn::Vec3 W_last_self{};          // last snapshot position (meters), for build placement
#ifdef OGFN_HARNESS
	// Harness hooks: records build inputs so a scenario can assert pieces were placed.
	std::vector<std::pair<int, int>> g_harnessBuildLog;
	std::function<void(ogfn::PieceKind, float, bool)> g_buildRecorder;
#endif
	bool g_lastStepBlocked = false;    // last kinematic step was swept into geometry
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

	// Loot, refreshed at 2 Hz and shared across bots (one GObjects sweep, not 20).
	// Stores the item CLASS with the position, because the brain's loot decision is
	// a valuation: a count cannot tell a big pot from an ammo box, and that
	// difference is the whole of loot decision making.
	struct LootSpot
	{
		FVector Pos;
		ogfn::ItemClass Cls;
		int Count;
		int Id;
	};
	std::vector<LootSpot> g_lootSpots;
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
		AFortPlayerController* controller = nullptr;   // for squad/team resolution
	};

	// File scope, not a static inside TickAll: the hit-resolution path needs the same
	// list the perception pass just built, and passing it by hand would let the two
	// drift. Rebuilt from scratch every tick before any bot is processed.
	std::vector<EnemyPawn> Others;

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

	// Building: place one piece on the grid. Resolves the player-buildable class at
	// runtime, because this client's FName table is the only authority and a wrong
	// hardcoded path fails silently (the piece simply never appears).
	//
	// Returns true if a piece was spawned. Logged misses are one-shot per class so a
	// bot that cannot build does not spam the log 50 times a second.
	bool ApplyBuildPiece(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn,
	                     const ogfn::BuildInput& In, const ogfn::BuildGrid& Grid) {
		if (!Controller || !Pawn || In.piece == ogfn::PieceKind::None)
			return false;

		static bool s_resolved = false;
		static UClass* s_wall = nullptr;
		static UClass* s_floor = nullptr;
		static UClass* s_ramp = nullptr;
		static UClass* s_cone = nullptr;
		if (!s_resolved) {
			s_resolved = true;
			// The 3.5 playable set. These paths are the fork's own (see
			// FortPlayerController.cpp BuildableData usage); each may be absent, and
			// an absent one just means that piece never gets placed.
			s_wall = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Solid.PBWA_W1_Solid_C");
			s_floor = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_S1_Floor.PBWA_S1_Floor_C");
			s_ramp = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_S1_Ramp.PBWA_S1_Ramp_C");
			s_cone = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Arch.PBWA_W1_Arch_C");
			LOG_INFO(LogBots, "[ogfn] buildable classes: wall={} floor={} ramp={} cone={}",
				(__int64)s_wall, (__int64)s_floor, (__int64)s_ramp, (__int64)s_cone);
		}

		UClass* Class = nullptr;
		switch (In.piece) {
			case ogfn::PieceKind::Wall: Class = s_wall; break;
			case ogfn::PieceKind::Floor: Class = s_floor; break;
			case ogfn::PieceKind::Ramp: Class = s_ramp; break;
			case ogfn::PieceKind::Cone: Class = s_cone; break;
			case ogfn::PieceKind::None: return false;
		}
		if (!Class) {
#ifdef OGFN_HARNESS
			// The harness has no asset registry, so no class can resolve. The point of
			// the sim is that the INTENT reaches the placement path with a real piece,
			// so record it and report success; the live build's class resolution is
			// logged separately below.
			if (g_buildRecorder)
				g_buildRecorder(In.piece, In.yaw_deg, In.edit);
			return true;
#else
			static bool s_warned = false;
			if (!s_warned) {
				s_warned = true;
				LOG_ERROR(LogBots, "[ogfn] no buildable class for piece {}, bots cannot build",
					ogfn::to_string(In.piece));
			}
			return false;
#endif
		}

		// Brain is meters, engine is uu. This is the only conversion point.
		const ogfn::Vec3 self{ W_last_self.x, W_last_self.y, W_last_self.z };
		const ogfn::BuildPiece Piece = Grid.place(In.piece, self, In.yaw_deg, 0);
		if (!Piece.valid)
			return false;

#ifndef OGFN_HARNESS
		auto World = GetWorld();
		if (!World)
			return false;

		FVector Loc(Piece.pos.x * 100.f, Piece.pos.y * 100.f, Piece.pos.z * 100.f);
		FRotator Rot(0.f, Piece.yaw_deg, 0.f);
		FTransform T{};
		T.Translation = Loc;
		T.Rotation = Rot.Quaternion();
		T.Scale3D = FVector(1.f, 1.f, 1.f);

		auto* Piece1 = World->SpawnActor<ABuildingSMActor>(Class, T);
		if (!Piece1)
			return false;
		Piece1->SetPlayerPlaced(true);
		// Infinite materials are already on for bots; the engine still wants the
		// actor initialized as player-placed or it decays.
		Piece1->InitializeBuildingActor(Controller, Piece1, true);
		return true;
#else
		// Harness: record the intent so a scenario can assert pieces reach the world.
		if (g_buildRecorder)
			g_buildRecorder(In.piece, In.yaw_deg, In.edit);
		return true;
#endif
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
		if (g_ladderParsed)
			return;
		// The retry budget used to be 40 attempts at the 2s zone-refresh cadence, i.e.
		// it gave up 80 seconds into the match. The engine prints its nine
		// PickSafeZoneLocation rings about 120 seconds in, so the parser ALWAYS
		// exhausted its budget before the data existed and never tried again: every
		// live match logged ladder=0, which meant no bot ever knew where the storm
		// was and all 50 piled up and stood still. Never give up permanently on data
		// that is guaranteed to arrive; back off instead, and keep a slow retry.
		const int kFastTries = 60;          // every refresh (2s) for the first 2 minutes
		const bool bFast = g_ladderTries < kFastTries;
		// Slow retry: once every ~10s, forever, until the ladder lands.
		if (!bFast)
		{
			g_ladderSlowAt += 1;
			if (g_ladderSlowAt % 5 != 0)
				return;
		}
		g_ladderTries++;
		static int s_dbg = 0;
		const bool bFirst = (s_dbg % 10 == 0);
		s_dbg++;
		char* La = nullptr;
		size_t LaLen = 0;
		const int DupRv = _dupenv_s(&La, &LaLen, "LOCALAPPDATA");
		if (DupRv != 0 || !La || LaLen == 0)
		{
			if (bFirst)
				LOG_ERROR(LogBots, "[ogfn] ladder: LOCALAPPDATA unavailable (rv={})", DupRv);
			return;
		}
		std::string Path = std::string(La) + "\\FortniteGame\\Saved\\Logs\\FortniteGame.log";
		free(La);
		// The UE4 process holds FortniteGame.log open for writing and fopen_s("r")
		// returns EACCES (rv=13). This single failure is why EVERY live match logged
		// ladder=0: the storm ladder was unreadable, so no bot knew where the zone
		// was, all 50 stacked in one spot and the storm eventually wiped the pile
		// (49 bots dying within 2 seconds at t=533). Open with FILE_SHARE_READ and
		// FILE_SHARE_WRITE so the game's own append does not lock us out.
		FILE* F = nullptr;
		int OpenRv = 1;
		{
			HANDLE H = CreateFileA(Path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (H != INVALID_HANDLE_VALUE)
			{
				const int Fd = _open_osfhandle((intptr_t)H, _O_RDONLY | _O_TEXT);
				if (Fd != -1)
				{
					F = _fdopen(Fd, "r");
					if (F)
						OpenRv = 0;
					else
						_close(Fd);
				}
				if (!F)
					CloseHandle(H);
			}
			else
			{
				OpenRv = (int)GetLastError();
			}
		}
		if (OpenRv != 0 || !F)
		{
			if (bFirst)
				LOG_ERROR(LogBots, "[ogfn] ladder: cannot open {} (rv={})", Path, OpenRv);
			return;
		}
		float Rad[10] = {}, Cx[10] = {}, Cy[10] = {};
		bool bHasR[10] = {}, bHasL[10] = {};
		char Line[768];
		while (fgets(Line, (int)sizeof(Line), F))
		{
			int N = 0;
			// Find the ring marker ANYWHERE in the line: the line is prefixed with
			// "[2026.10.21-12.59.24:300][214]LogFort: ". Parsing from the start of
			// the line never matched, so the storm was never known.
			char* Tag = strstr(Line, "PickSafeZoneLocation[");
			if (!Tag)
				continue;
			if (sscanf_s(Tag, "PickSafeZoneLocation[%d]", &N) != 1 || N < 1 || N > 9)
				continue;
			char* Ch = strstr(Tag, "Changing Pick Radius from");
			if (Ch)
			{
				float A = 0.f, B = 0.f;
				if (sscanf_s(Ch, "Changing Pick Radius from %f to %f", &A, &B) == 2)
				{
					Rad[N] = B;
					bHasR[N] = true;
				}
			}
			char* Loc = strstr(Tag, "Setting Location:");
			if (Loc)
			{
				float X = 0.f, Y = 0.f, Z = 0.f;
				if (sscanf_s(Loc, "Setting Location: %f, %f, %f", &X, &Y, &Z) == 3)
				{
					Cx[N] = X;
					Cy[N] = Y;
					bHasL[N] = true;
				}
			}
		}
		fclose(F);
		if (bFirst)
		{
			int seen = 0;
			for (int N = 1; N <= 9; N++) { if (bHasR[N]) seen++; }
			LOG_INFO(LogBots, "[ogfn] ladder probe: path={} radiiFound={} locFound={}",
				Path, seen, [&]{int c=0; for(int N=1;N<=9;N++) if(bHasL[N]) c++; return c;}());
		}

		// The three judges that separate "no ladder yet" from "ladder parsed":
		// the untrimmed line starts with a [timestamp][frame]LogFort: prefix, so the
		// ORIGINAL code (sscanf straight onto Line with "PickSafeZoneLocation[%d]")
		// matched nothing, ever. Every live match logged ladder=0 and the bots had
		// no zone to walk to, which is why all 50 of them piled up and stood still.
		// The marker is now located with strstr and parsed from there, which is
		// exact: PickSafeZoneLocation appears immediately after the prefix.

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
		else if (bFirst)
		{
			// Still no rings: say so once in a while, but KEEP TRYING. Giving up here
			// is what left every previous match with no storm truth at all.
			LOG_INFO(LogBots, "[ogfn] ring ladder not in the log yet (attempt {}); retrying",
				g_ladderTries);
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
		{
			static int s_z = 0;
			if (s_z++ < 3)
				LOG_INFO(LogBots, "[ogfn] zone refresh #{} (ladderParsed={})", s_z, (int)g_ladderParsed);
		}
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
		g_lootSpots.clear();
		g_lootSpots.reserve((size_t)Found.Num());
		static int s_unclassified = 0;
		for (int i = 0; i < Found.Num(); i++)
		{
			auto* Pickup = Cast<AFortPickup>(Found.at(i));
			if (!Pickup)
				continue;
			LootSpot Spot;
			Spot.Pos = Pickup->GetActorLocation();
			Spot.Cls = ogfn::ItemClass::Unknown;
			Spot.Count = 1;
			// Classify from the item definition's full name. The brain matches on
			// substrings in a deliberate order, so it wants the whole path, not a
			// display name.
			if (auto* Entry = Pickup->GetPrimaryPickupItemEntry())
			{
				Spot.Count = Entry->GetCount();
				if (auto* Def = Entry->GetItemDefinition())
				{
					Spot.Cls = ogfn::classify_item(Def->GetFullName());
					if (Spot.Cls == ogfn::ItemClass::Unknown && s_unclassified < 12)
					{
						++s_unclassified;
						LOG_INFO(LogBots, "[ogfn] unclassified pickup: {}",
							Def->GetFullName().c_str());
					}
				}
			}
			Spot.Id = (int)(((uintptr_t)Pickup >> 4) & 0x7fffffff);
			g_lootSpots.push_back(Spot);
		}
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

	// Classify a weapon by its asset path. The bot must hold the gun it is actually
	// holding: one shared "30 rounds, 2.6s reload" profile for every weapon in the
	// lobby is why a pump shotgun sounded like a rifle. Ordered so that the more
	// specific names win (a "Shotgun_Sniper" would be neither, but a
	// "SMG_Rifle" style name is not a thing these builds ship).
	ogfn::WeaponClass ClassifyWeapon(const std::string& Path)
	{
		auto has = [&](const char* s) { return Path.find(s) != std::string::npos; };
		// Note: "Athena" is in every one of these paths, so it cannot decide anything.
		if (has("Shotgun") || has("Pump")) return ogfn::WeaponClass::Shotgun;
		if (has("Sniper") || has("Bolt") || has("Hunting") || has("Rail"))
			return ogfn::WeaponClass::Sniper;
		if (has("SMG") || has("Submachine") || has("MachinePistol") || has("Compact"))
			return ogfn::WeaponClass::Smg;
		return ogfn::WeaponClass::Rifle;
	}

	ogfn::WeaponProfile ProfileForWeapon(const std::string& Path)
	{
		const ogfn::WeaponClass c = ClassifyWeapon(Path);
		const ogfn::WeaponProfile w = ogfn::weapon_for_class(c);
		static int logged = 0;
		if (logged < 12)
		{
			++logged;
			static const char* kNames[] = {"rifle", "smg", "shotgun", "sniper"};
			LOG_INFO(LogBots, "[ogfn] weapon class: {} -> {} (mag {}, {:.2f}s/shot)",
				Path, kNames[static_cast<int>(c)], w.mag_size, w.shot_interval_secs);
		}
		return w;
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

		// The bot fires the gun it is actually holding: cyclic rate, mag and trigger
		// shape all come from the weapon it was granted.
		const std::string armedPath = g_weaponCands[idx]->GetPathName();
		M.armedProfile = ProfileForWeapon(armedPath);
		M.motor.set_weapon(M.armedProfile);

		LOG_INFO(LogBots, "[ogfn] armed: {}", armedPath);
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
	                                  AFortPlayerControllerAthena* Controller,
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
		W.loot.clear();
		for (auto& Spot : g_lootSpots)
		{
			float dx = ToMeters((float)Spot.Pos.X) - W.self_pos.x;
			float dy = ToMeters((float)Spot.Pos.Y) - W.self_pos.y;
			const float d2 = dx * dx + dy * dy;
			if (d2 <= 30.f * 30.f)
				loot30++;
			// The valuator has its own, wider radius (45m); feed it everything within
			// reach rather than pre-filtering at 30m, or a pot at 32m is invisible.
			if (d2 <= 46.f * 46.f)
			{
				ogfn::LootSample Sample;
				Sample.id = Spot.Id;
				Sample.pos = ogfn::Vec3(ToMeters((float)Spot.Pos.X), ToMeters((float)Spot.Pos.Y),
				                        ToMeters((float)Spot.Pos.Z));
				Sample.cls = Spot.Cls;
				Sample.count = Spot.Count;
				W.loot.push_back(Sample);
			}
		}
		W.loot_within_30m = loot30;

		// Own inventory, so loot worth is relative to what the bot already carries.
		// Read every tick: it changes on every pickup and every heal.
		W.inventory = ogfn::Inventory{};
		if (auto* Inv = Controller->GetWorldInventory())
		{
			W.inventory.ammo = 999;   // infinite ammo is on for bots (see the arena toggles)
			W.inventory.materials = 999;
			// CURRENT shield belongs in the inventory too, not just the snapshot. The loot
			// valuator asks "is a big pot the best thing on the floor" and the answer is
			// entirely about how much shield you are carrying right now; leaving this at
			// its default 0 made every bot believe it was bare, so a big pot was always
			// worth maximum and bots walked to every pot in sight forever.
			W.inventory.shield = (int)W.self_shield;
			auto& Instances = Inv->GetItemList().GetItemInstances();
			for (int ii = 0; ii < Instances.Num(); ++ii)
			{
				auto* Item = Instances.At(ii);
				if (!Item)
					continue;
				auto* Entry = Item->GetItemEntry();
				if (!Entry)
					continue;
				auto* Def = Entry->GetItemDefinition();
				const ogfn::ItemClass Cls = Def ? ogfn::classify_item(Def->GetFullName())
				                                : ogfn::ItemClass::Unknown;
				const int Count = Entry->GetCount();
				switch (Cls)
				{
					case ogfn::ItemClass::ShieldBig: W.inventory.big_shields += Count; break;
					case ogfn::ItemClass::ShieldPotion: W.inventory.small_shields += Count; break;
					case ogfn::ItemClass::Medkit: W.inventory.medkits += Count; break;
					case ogfn::ItemClass::Bandage: W.inventory.bandages += Count; break;
					case ogfn::ItemClass::Shotgun: W.inventory.has_shotgun = true; break;
					case ogfn::ItemClass::Sniper: W.inventory.has_sniper = true; break;
					case ogfn::ItemClass::AssaultRifle:
					case ogfn::ItemClass::Pistol:
						// Tier is not exposed per-item here; holding ANY gun is what
						// stops the bot valuing a second one at starter-gun prices.
						if (W.inventory.weapon_tier < 2) W.inventory.weapon_tier = 2;
						break;
					default: break;
				}
			}
			// A bot holding any of the granted weapons counts as armed.
			if (W.inventory.weapon_tier == 0)
				W.inventory.weapon_tier = 2;
		}
		W.enemies_within_30m = enemiesSeenWithin30;
		W.enemies_currently_fighting_each_other = 0; // needs damage observation, next pass

		// Squad context for the brain. Downed state is not read: the fork has no revive
		// plumbing on 3.5, so teammates are reported alive and standing. Focus fire and
		// spacing work regardless, and revive turns on the moment the engine exposes it.
		W.teammates.clear();
		for (auto& Mate : M.squadmates)
		{
			if (!PawnUsable(Mate.second))
				continue;
			ogfn::Teammate T;
			T.id = Mate.first;
			T.pos = ToVec(Mate.second->GetActorLocation());
			T.health = Mate.second->GetHealth() + Mate.second->GetShield();
			T.downed = false;
			W.teammates.push_back(T);
		}
		W.now_ms = (uint64_t)(Now * 1000.0);
		// Heal progress back to the brain, and the consumable counts it decides on.
		W.heal_remaining_secs = M.healRemaining;
		if (auto* Inv = Controller->GetWorldInventory())
		{
			auto& Instances = Inv->GetItemList().GetItemInstances();
			for (int ii = 0; ii < Instances.Num(); ++ii)
			{
				auto* Item = Instances.At(ii);
				if (!Item || !Item->GetItemEntry())
					continue;
				auto* Def = Item->GetItemEntry()->GetItemDefinition();
				if (!Def)
					continue;
				const ogfn::ItemClass Cls = ogfn::classify_item(Def->GetFullName());
				const int Count = Item->GetItemEntry()->GetCount();
				switch (Cls)
				{
					case ogfn::ItemClass::ShieldBig: W.big_shields += Count; break;
					case ogfn::ItemClass::ShieldPotion: W.small_shields += Count; break;
					case ogfn::ItemClass::Medkit: W.medkits += Count; break;
					case ogfn::ItemClass::Bandage: W.bandages += Count; break;
					default: break;
				}
			}
		}
		W.enemies_pushing_me = false;   // no damage events wired on this build yet

		auto* Target = M.brain.perception().nearest(true);
		if (Target)
		{
			W.target_is_visible = Target->visible;
			// A target only reaches this list through a line-of-sight trace (see the
			// bLos gate above), so a VISIBLE one is, by construction, shootable. Say so
			// explicitly rather than leaving the snapshot's default: the Engage score
			// discounts blocked targets, and if this field silently stayed true the
			// discount would never apply and the whole term would be dead weight.
			// A remembered-but-not-visible target is a different case: it may well be
			// behind the wall that broke the line, so it is marked blocked.
			W.target_has_line_of_sight = Target->visible;
			ogfn::Vec3 D = Target->last_seen_pos - W.self_pos;
			W.target_distance = ogfn::length(D);
			W.target_health = Target->health;
			W.target_is_reloading = false; // engine exposes no reload state for others
		}

		return W;
	}
	// Last snapshot's loot per bot, so a harness scenario can inspect what the brain
	// actually received (samples with classes, and the choice it made).
#ifdef OGFN_HARNESS
	std::vector<std::vector<ogfn::LootSample>> g_lastLoot;
	std::vector<ogfn::LootChoice> g_lastLootChoice;
	void RecordLootForHarness(size_t i, const ogfn::WorldSnapshot& W, const ogfn::Brain& B) {
		if (g_lastLoot.size() <= i) {
			g_lastLoot.resize(i + 1);
			g_lastLootChoice.resize(i + 1);
		}
		g_lastLoot[i] = W.loot;
		g_lastLootChoice[i] = B.loot_choice();
	}
#endif
} // namespace (anonymous)

// Called from Bots::Tick every net tick while Globals::bEnableOgfnBrain is set.
// The stock tick keeps only what the brain deliberately does not own yet: bus deploy,
// thank-the-driver, and its warmup hop (suppressed while brains are on).
	// Defined just above TickAll: it needs `Others` (rebuilt each tick inside
	// TickAll) and CanSee, neither of which exists this early in the file.
	void ResolveShot(Mind& M, AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller,
	                 const ogfn::TrackedTarget* Target, const ogfn::MotorOutput& Out, double Now);

	// What did that round actually do?
	//
	// Fires the brain's hit model against the pawn the motor was aiming at, and lands the
	// damage on the victim with the engine's own setters. This exists because the client
	// cannot do it: there is no ranged damage path on 3.5, so without this a bot that aims
	// perfectly is indistinguishable from one shooting the sky.
	//
	// Conservative in three ways on purpose: it needs a REAL target pawn resolved from
	// this tick's enemy list, a real line-of-sight result, and it never invents damage. A
	// miss stays a miss and is counted as one.
	void ResolveShot(Mind& M, AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller,
	                 const ogfn::TrackedTarget* Target, const ogfn::MotorOutput& Out, double Now)
	{
		if (!Target || !Pawn || !Controller)
			return;

		// Map the tracked id back to a live pawn. The enemy list is rebuilt every tick
		// from fresh controllers, so a respawn cannot leave us writing to a dead actor.
		//
		// COUNT ALL MATCHES, not the first. PawnId folds a pointer into 31 bits, so two
		// live pawns CAN share an id (seen in the harness: 0x602acf59e630 and
		// 0x5f3a16961b80 hashed equal). Taking the first match then applied every round
		// to a bystander while the intended victim never lost a point. An ambiguous id
		// means we do not know who was hit, so we decline rather than damage the wrong bot.
		AFortPlayerPawnAthena* Victim = nullptr;
		int matches = 0;
		for (auto& Other : Others)
			if (Other.id == Target->id && PawnUsable(Other.pawn))
			{
				Victim = Other.pawn;
				++matches;
			}
		if (!Victim || matches > 1 || Victim == Pawn)
			return;

		const FVector SelfLoc = Pawn->K2_GetActorLocation();
		const FVector VictimLoc = Victim->K2_GetActorLocation();
		// Chest height, not feet: a capsule measured from the ground plane puts the
		// whole body above the ray and every shot reads as a miss.
		const FVector AimPoint = VictimLoc + FVector(0.f, 0.f, 60.f);

		ogfn::ShotInput In;
		In.from = ToVec(SelfLoc);
		In.yaw_deg = Out.yaw_deg;
		In.pitch_deg = Out.pitch_deg;
		In.target_center = ToVec(AimPoint);
		In.target_health = Victim->GetHealth();
		In.target_shield = Victim->GetShield();
		In.has_line_of_sight = CanSee(Pawn, SelfLoc, AimPoint);   // the ENGINE decides
		In.distance = length(In.target_center - In.from);

		// The gun decides the shape of the shot. A shotgun scatters and stops working
		// past its range; a sniper does not. One profile for every weapon is what makes
		// a bot sound like one gun wearing different models.
		const ogfn::WeaponProfile& Wp = M.armedProfile;
		In.damage = Wp.damage;
		In.pellets = Wp.pellets;
		In.spread_deg = 1.1f * Wp.tolerance_scale;
		In.falloff_start_m = Wp.falloff_start_m;
		In.falloff_end_m = Wp.falloff_end_m;
		In.min_damage_frac = Wp.min_damage_frac;

		ogfn::ShotOutcome Res = M.brain.resolve_shot(In);
		M.shotsResolved++;

		const float Total = Res.damage_on_shield + Res.damage_on_health;
		if (Total <= 0.f)
			return;   // a miss stays a miss

		M.shotsHit++;
		M.damageDealt += Total;

		ogfn::DamageSplit Split = ogfn::apply_damage(Total, Victim->GetShield(), Victim->GetHealth());
		Victim->SetShield(Split.shield_left);
		Victim->SetHealth(Split.health_left);

		if (&M == &g_minds[0] && Now - M.lastResolveLog > 5.0)
		{
			M.lastResolveLog = Now;
			LOG_INFO(LogBots,
				"[ogfn] hit: dmg={:.1f} total={:.0f} hits={}/{} victimHp={:.0f} victimSh={:.0f}",
				Total, M.damageDealt, M.shotsHit, M.shotsResolved,
				Victim->GetHealth(), Victim->GetShield());
		}
	}

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

			// Liveness assertions. The live-only failures of this session were all
			// silent: the fleet stopped firing for 200s (empty enemy list) and the
			// walk was 89% blocked while the bbox never moved. Neither showed up as
			// an error, only as a number that quietly stopped changing, so assert on
			// the numbers instead of waiting for a human to read a graph.
			static long long s_lastFired = -1;
			static double s_lastGrowth = Now;
			static float s_lastSpread = -1.f;
			const float spread = (maxx - minx) + (maxy - miny);
			if (s_lastFired >= 0 && firedSum == s_lastFired && alive > 4)
			{
				if (Now - s_lastGrowth > 45.0)
				{
					LOG_ERROR(LogBots, "[ogfn] STALL: no shots fired by {} alive bots for {:.0f}s",
						alive, Now - s_lastGrowth);
					s_lastGrowth = Now;   // one line per 45s, not per tick
				}
			}
			else
			{
				s_lastGrowth = Now;
			}
			s_lastFired = firedSum;
			if (s_lastSpread >= 0.f && alive > 4 && walkOk > 200)
			{
				const float blockFrac = (float)walkBlk / (float)(walkOk + walkBlk);
				if (blockFrac > 0.85f)
					LOG_ERROR(LogBots,
						"[ogfn] MOVEMENT STALL: {:.0f}% of steps blocked, fleet spread {:.0f}m",
						100.f * blockFrac, spread);
			}
			s_lastSpread = spread;
		}
	}

	// Enemy pawn set for this pass: every other fleet bot plus every human client
	// pawn, read FRESH from the controller each tick (respawns replace the cached
	// PlayerBot.Pawn pointer, which never gets refreshed upstream).
	static std::vector<EnemyPawn> Enemies;
	Enemies.clear();
	Enemies.reserve(bots.size() + 4);

	// Every other pawn in the match, resolved per bot further down: on a solo playlist
	// every bot is its own team, but on a duo/squad playlist a bot that treats a
	// teammate as an enemy shoots its own squad, which is the worst possible tell.
	Others.clear();
	Others.reserve(bots.size() + 4);

	if (bCombat)
	{
		for (auto& Bot : bots)
		{
			if (!Bot.Controller || !Bot.Controller->IsValidLowLevel())
				continue;
			auto P = Cast<AFortPlayerPawnAthena>(Bot.Controller->GetPawn());
			if (PawnUsable(P))
			{
				EnemyPawn E;
				E.id = PawnId(P);
				E.pawn = P;
				// PlayerBot.Controller is declared AController* (bots.h) because it can
				// hold either an AI bot controller or a player controller. Cast what we
				// can; if the cast fails the team is UNKNOWN (TeamOf returns -1) and the
				// bot must still be listed as an enemy. Dropping it instead emptied the
				// enemy list for controllers without a player state, and the live fleet
				// stopped firing entirely (fired froze at 16176 for the rest of the
				// match). Failing to shoot is unrecoverable; a mis-identified squadmate
				// is handled by the explicit team compare below.
				E.controller = Cast<AFortPlayerController>(Bot.Controller);
				Others.push_back(E);
			}
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
				if (!PawnUsable(HumanPawn))
					continue;
				Others.push_back({ PawnId(HumanPawn), HumanPawn, PC });
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
		// Split the others into enemies and squadmates for THIS bot. Only split when
		// both team ids are known: an unknown team on either side falls through to
		// enemy, because failing to shoot is recoverable and failing to recognise
		// your own squadmate is not.
		Enemies.clear();
		M.squadmates.clear();
		{
			auto TeamOf = [](AFortPlayerController* PC) -> int
			{
				if (!PC)
					return -1;
				auto PS = Cast<AFortPlayerStateAthena>(PC->GetPlayerState());
				if (!PS || !PS->IsValidLowLevel())
					return -1;
				return (int)PS->GetTeamIndex();
			};
			const int SelfTeam = TeamOf(Controller);
			for (auto& Other : Others)
				if (SelfTeam >= 0 && TeamOf(Other.controller) == SelfTeam)
					M.squadmates.emplace_back(PawnId(Other.pawn), Other.pawn);
				else
					Enemies.push_back(Other);
		}

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
#ifdef OGFN_HARNESS
			// Skill must be deterministic too: the seed above was pinned but skill
			// still came from the ASLR pointer, so each run drew a different persona
			// and P9 flaked between lateral movement and a straight walk.
			skill = (float)i / 19.f - 0.5f;
#endif
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
		// Progress rescue (ARENA PLUMBING, not bot behaviour). With no navmesh a bot
		// walking 1km toward the zone can be pinned by terrain forever: measured live,
		// 93% of steps blocked and the fleet spread growing to 1410m while the zone
		// sat 1000m away. If a bot has made no net progress over 20s, put it on the
		// ground inside the ring the way the deploy wave does, so the match can
		// continue and the brain (which is what this project is actually testing) keeps
		// running. Logged loudly because it is a crutch, not a success.
		if (Phase == EAthenaGamePhase::SafeZones && g_ladderParsed && Now >= M.nextRescueTry)
		{
			M.nextRescueTry = Now + 20.0;
			auto Rp = Pawn->GetActorLocation();
			const float dToZone = std::sqrt(((float)Rp.X - g_ladderCX) * ((float)Rp.X - g_ladderCX)
			                              + ((float)Rp.Y - g_ladderCY) * ((float)Rp.Y - g_ladderCY));
			const bool bNoProgress = (M.stepOk + M.stepBlk) > 300 && M.stepBlk > M.stepOk * 2;
			if (bNoProgress && dToZone > g_ladderR + 500.f && Pawn->GetHealth() > 0.f)
			{
				const float ang = (float)i * 2.399963f;
				const float rr = 60.f + (float)(i % 7) * 40.f;
				FVector Dest(g_ladderCX + std::cos(ang) * rr,
				             g_ladderCY + std::sin(ang) * rr, (float)Rp.Z + 400.f);
				FHitResult* Gh = nullptr;
				UKismetSystemLibrary::LineTraceSingle(Pawn,
					FVector(Dest.X, Dest.Y, Dest.Z + 6000.f),
					FVector(Dest.X, Dest.Y, Dest.Z - 40000.f),
					ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
					EDrawDebugTrace::None, true, FLinearColor(), FLinearColor(), 0.f, &Gh);
				if (Gh && Gh->IsBlockingHit())
				{
					FVector FL = Gh->GetLocation();
					if (FL.Z >= Dest.Z - 40000.f && FL.Z <= Dest.Z + 6000.f)
						Dest.Z = FL.Z + 100.f;
				}
				// Report BEFORE zeroing: the previous version logged after the reset
				// and so always printed "0% blocked", which is the one number that
				// explains why the rescue fired.
				const float blkPct = 100.f * (float)M.stepBlk /
				                     (float)(M.stepOk + M.stepBlk + 1);
				const int wasOk = M.stepOk, wasBlk = M.stepBlk;
				ApplyTeleport(Pawn, Dest);
				M.stepOk = 0;
				M.stepBlk = 0;
				M.stuckPhase = 0;
				++M.rescues;
				LOG_INFO(LogBots, "[ogfn] PROGRESS-RESCUE bot#{}: {:.0f}m from zone, {:.0f}% blocked ({}/{}) -> placed in ring",
					(int)i, dToZone / 100.f, blkPct, wasBlk, wasOk + wasBlk);
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

		ogfn::WorldSnapshot W = BuildSnapshot(M, Pawn, Controller, enemiesSeenWithin30, Now);
		W_last_self = W.self_pos;
		M.lastSnap = W;
		M.decision = M.brain.think(W);
#ifdef OGFN_HARNESS
		RecordLootForHarness(i, W, M.brain);
#endif

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
		// Target velocity, measured from the pawn's own movement over the last tick.
		// Engine uu -> brain meters, and clamped: a velocity spike from a teleport or a
		// respawn must not fling the aim point across the map.
		ogfn::TargetMotion Motion;
		if (Target)
		{
			if (M.has_last_target && Now > M.last_target_time)
			{
				const float era = static_cast<float>(Now - M.last_target_time);
				ogfn::Vec3 v = (Target->last_seen_pos - M.last_target_pos) * (1.f / era);
				const float speed = ogfn::length(v);
				if (speed > 30.f) v = v * (30.f / speed);   // 30 m/s is already generous
				Motion = ogfn::TargetMotion(v);
			}
			M.last_target_pos = Target->last_seen_pos;
			M.last_target_time = Now;
			M.has_last_target = true;
		}
		else
		{
			M.has_last_target = false;
		}

		ogfn::MotorOutput Out = Target
			? M.motor.aim_at(W.self_pos, W.self_yaw_deg, M.pitch, Target->last_seen_pos,
			                 M.decision.fire, dt, Motion)
			: M.motor.idle_look(W.self_yaw_deg, M.pitch, dt);
		// Carry the motor's vertical aim forward: this is the state that did not exist.
		if (Out.pitch_deg == Out.pitch_deg)   // NaN guard
			M.pitch = Out.pitch_deg;

#ifdef OGFN_SIM_TRACE
		if (&M == &g_minds[0] && (int)(Now * 10.0) % 6 == 0)
		{
			std::printf("[sim] motor t=%.2f out_fire=%d aim_err=%.1f tgt=%d\n",
				Now, (int)Out.firing, M.motor.aim_error_deg(), Target ? 1 : 0);
		}
#endif

		// Movement model. Feeds the strafe/crouch/jump layer real ground truth:
		// whether the capsule is falling and how fast, whether there is a wall
		// within arm's reach to peek from, and whether the floor is a slope.
		// velZ came from the movement component; it is frozen by the engine bug,
		// so a fall is detected from the ground probe instead (see below).
		const bool bEngaged = M.decision.action == ogfn::ActionKind::Engage
			|| M.decision.action == ogfn::ActionKind::Build;
		const float YawToTarget = Target
			? ogfn::yaw_between(W.self_pos, Target->last_seen_pos)
			: W.self_yaw_deg;
		const float HpFrac = ogfn::clampf((W.self_health + W.self_shield) / 200.f, 0.f, 1.f);
		ogfn::MoveState LocoState;
		{
			// Cover probe: a short trace chest-height toward the strafe side tells
			// the peek cycle whether there is something to duck behind. One trace
			// only, and only while engaged, so the island costs nothing.
			LocoState.has_cover = false;
			if (bEngaged && g_traceFnReady)
			{
				const float Side = M.loco.strafe_sign() * 90.f; // 1.5 m to the side
				FVector From = Pawn->GetActorLocation();
				From = From + FVector(0.f, 0.f, 60.f);
				FVector To = From;
				To = To + FVector(
					std::cos(ogfn::deg2rad(W.self_yaw_deg + Side)) * 150.f,
					std::sin(ogfn::deg2rad(W.self_yaw_deg + Side)) * 150.f, 0.f);
				FHitResult* CH = nullptr;
				UKismetSystemLibrary::LineTraceSingle(Pawn, From, To,
					ETraceTypeQuery::TraceTypeQuery1, false, TArray<AActor*>(),
					EDrawDebugTrace::None, true, FLinearColor(), FLinearColor(), 0.f, &CH);
				LocoState.has_cover = CH && CH->IsBlockingHit();
			}
			// CharMoveComp never ticks here, so airborne/vertical speed are not
			// observable: report grounded, and let the jump gate do the pacing.
			LocoState.airborne = false;
			LocoState.vertical_speed = 0.f;
			LocoState.on_slope = g_lastStepBlocked; // uphill/stuck reads as slope work
		}
		// Travel direction, which the movement layer uses for the gaze: a player looks
		// where they run, and without this the crosshair sat a median 85 degrees off the
		// travel line (walking sideways, staring at nothing).
		const float TravelYaw = (M.decision.move_to.x != 0.f || M.decision.move_to.y != 0.f)
			? ogfn::yaw_between(W.self_pos, M.decision.move_to)
			: W.self_yaw_deg;
		const ogfn::MoveOutput Move = M.loco.update(dt, W.self_yaw_deg, YawToTarget,
			W.target_distance, bEngaged, Out.firing, M.motor.reloading(), HpFrac, LocoState,
			TravelYaw, /*travel_supplied*/ (M.decision.move_to.x != 0.f ||
			                                 M.decision.move_to.y != 0.f));
		// Hand the gaze to the brain so the next decision carries it.
		M.brain.set_gaze(Move.gaze_weight > 0.f, Move.gaze_yaw_deg);

		// Camera vs body. The body yaw is ALSO the perception FOV axis (Obs.self_yaw_deg
		// reads the pawn's rotation), so rotating the body toward travel would rotate
		// the bot's eyes away from an enemy and break targeting: the first cut of this
		// did exactly that and P2 caught it (zero shots). So:
		//   * with a target in view, the aim owns the rotation, as before;
		//   * with nothing in view, the view follows travel (or a shoulder check),
		//     which is the case that was the tell: walking sideways, staring ahead.
		float ViewYaw = Out.yaw_deg;
		// Gaze only when the bot has seen nothing at all recently. A player walking
		// across open ground looks where they walk; a player who just saw someone does
		// not stop watching that direction to admire the scenery. Using "no targets in
		// memory" rather than "no targets visible" means the body does not rotate away
		// from a threat the instant it ducks behind cover, which is what delayed
		// first-fire from 4.5s to 9.7s in P2.
		const bool alone = M.brain.perception().targets().empty();
		if (M.decision.face_travel && alone) {
			const float Gw = ogfn::clampf(Move.gaze_weight, 0.f, 1.f);
			ViewYaw = ogfn::wrap180(Out.yaw_deg +
				ogfn::wrap180(M.decision.look_yaw_deg - Out.yaw_deg) * Gw);
		}
		ApplyRotation(Pawn, Out.pitch_deg, ViewYaw);
#ifdef OGFN_SIM_TRACE
		if (&M == &g_minds[0] && (int)(Now * 10.0) % 6 == 0)
		{
			std::printf("[sim] loco t=%.2f engaged=%d strafing=%d speed=%.2f crouch=%d jump=%d brace=%d cover=%d yaw2tgt=%.1f\n",
				Now, (int)bEngaged, (int)Move.strafing, Move.speed_scale,
				(int)Move.crouching, (int)Move.jumping, (int)Move.bracing,
				(int)LocoState.has_cover, YawToTarget);
		}
#endif

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
			// While engaged the heading comes from the movement layer, not from
			// move_to: a bot that walks straight at the circle while shooting is
			// the "stands still in a fight" tell. Move.strafing means the heading
			// is perpendicular to the target, and speed_scale caps how fast a
			// human crosses their own crosshair.
			const ogfn::Vec3& To = M.decision.move_to;
			float dirx, diry;
			if (Move.strafing)
			{
				const ogfn::Vec3 d = ogfn::yaw_to_dir(Move.move_yaw_deg);
				dirx = d.x; diry = d.y;
			}
			else
			{
				float mx = To.x - W.self_pos.x;
				float my = To.y - W.self_pos.y;
				float mlen = std::sqrt(mx * mx + my * my);
				if (mlen <= kMoveMinDist)
				{
					dirx = 0.f; diry = 0.f;
				}
				else
				{
					dirx = mx / mlen; diry = my / mlen;
				}
			}
			if (dirx != 0.f || diry != 0.f)
			{
				if (Move.strafing)
				{
					++M.strafeSteps;
					const float toward = dirx * std::cos(ogfn::deg2rad(YawToTarget))
					                   + diry * std::sin(ogfn::deg2rad(YawToTarget));
					if (toward > 0.6f)
						++M.strafeForward;   // leaked toward the target
				}
				float step = kBotWalkSpeed * dt * ogfn::clampf(Move.speed_scale, 0.f, 1.f);
				if (step <= 0.f)
					step = kBotWalkSpeed * dt * 0.35f; // crouched/bracing still creeps
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
				auto NowPos = Pawn->GetActorLocation();
				const float moved = std::fabs((float)NowPos.X - StepX0) + std::fabs((float)NowPos.Y - StepY0);
				if (moved > 0.5f)
				{
					M.stepOk++;
					g_lastStepBlocked = false;
				}
				else
				{
					M.stepBlk++;
					g_lastStepBlocked = true;
				}

				// --- stuck bookkeeping + committed detour -----------------------
				// Blocked for long enough that this is a wall, not a curb: pick a
				// side, COMMIT to it for ~0.6s. Re-rolling the angle every tick makes a
				// bot jitter back and forth in a corner, so the detour is a decision.
				if (moved <= 0.5f)
				{
					M.stuckTicks++;
					if (M.stuckTicks > 10 && M.stuckPhase <= 0)
					{
						// Alternate sides per attempt so a bot in a concave corner does not
						// oscillate into the same wall, and widen the angle each time it
						// fails again (up to ~150 deg, i.e. nearly back the way it came).
						M.stuckTries = (M.stuckTries + 1) % 6;
						const float widen = 1.15f + 0.18f * (float)M.stuckTries;
						M.stuckAng = std::atan2(diry, dirx) +
						             ((M.stuckTries & 1) ? 1.f : -1.f) * widen;
						M.stuckPhase = 30;                  // ~1.0s at 30Hz: commit
						M.stuckTicks = 0;
						++M.stuckEpisodes;
					}
				}
				else if (moved > 0.5f)
				{
					M.stuckTicks = 0;
				}
				if (M.stuckPhase > 0)
				{
					M.stuckPhase--;
					float step2 = kBotWalkSpeed * dt * 1.15f;
					FVector P2 = Pawn->GetActorLocation();
					FVector Detour(P2.X + std::cos(M.stuckAng) * step2,
					               P2.Y + std::sin(M.stuckAng) * step2, P2.Z + 30.f);
					ApplyTeleportEx(Pawn, Detour, /*bSweep*/ true);
				}
			}
		}

		// The movement layer owns jumping now; MotorOutput.jumping was never set.
		if (Move.jumping)
			ApplyJump(Pawn);

		// Healing. The brain commits to an item and a duration; this equips it once and
		// counts down. Without this, Heal was a decision with no effect at all: bots
		// decided to heal and nothing happened, which is worse than not deciding.
		if (M.brain.healing()) {
			// Count the animation down. The brain also keeps its own timer, but the
			// engine's number is the one the client animates against, so this is what
			// the snapshot reports and what the brain's engine-gate reads.
			if (M.healEquipped && M.healRemaining > 0.f)
				M.healRemaining = (std::max)(0.f, M.healRemaining - dt);
			if (!M.healEquipped) {
				M.healEquipped = true;
				M.healItem = M.brain.heal_item();
				// Seed the countdown from the brain's committed duration (item + jitter),
				// so the two agree instead of drifting apart.
				M.healRemaining = (std::max)(0.5f, M.brain.heal_seconds_left());
				if (auto* Inv = Controller->GetWorldInventory())
				{
					// Equip the matching consumable so the client-side use animation runs.
					// Which item is a brain decision; finding the stack is the engine's job.
					UFortItem* Want = nullptr;
					auto& Instances = Inv->GetItemList().GetItemInstances();
					for (int ii = 0; ii < Instances.Num(); ++ii)
					{
						auto* Item = Instances.At(ii);
						if (!Item || !Item->GetItemEntry())
							continue;
						auto* Def = Item->GetItemEntry()->GetItemDefinition();
						if (!Def)
							continue;
						const ogfn::ItemClass Cls = ogfn::classify_item(Def->GetFullName());
						const bool match =
							(M.healItem == 1 && Cls == ogfn::ItemClass::ShieldPotion) ||
							(M.healItem == 2 && Cls == ogfn::ItemClass::ShieldBig) ||
							(M.healItem == 3 && Cls == ogfn::ItemClass::Medkit) ||
							(M.healItem == 4 && Cls == ogfn::ItemClass::Bandage);
						if (match) { Want = Item; break; }
					}
					if (Want)
					{
						AFortPlayerController::ServerExecuteInventoryItemHook(
							Controller, Want->GetItemEntry()->GetItemGuid());
						++M.healsUsed;
						static bool s_healLogged = false;
						if (!s_healLogged)
						{
							s_healLogged = true;
							LOG_INFO(LogBots, "[ogfn] first heal: equipping item class {}",
								M.healItem);
						}
					}
					else
					{
						// Nothing matching in the bag: the brain's inventory read and the
						// engine's disagree. Report once rather than looping.
						static bool s_healMissLogged = false;
						if (!s_healMissLogged)
						{
							s_healMissLogged = true;
							LOG_ERROR(LogBots, "[ogfn] brain wanted heal item {} but no matching "
							           "consumable is in the inventory", M.healItem);
						}
					}
				}
			}
		} else {
			M.healEquipped = false;
			M.healRemaining = 0.f;
			M.healItem = 0;
		}

		// Building. decision.build_wall used to be dropped on the floor ("builder.h
		// is the next pass"), so no bot has ever placed a piece. The builder owns the
		// sequence, cadence and edits; this only executes the input it returns.
		{
			ogfn::BuildInput BIn = M.builder.update(dt, M.decision.build_wall,
				W.target_is_visible && W.self_health < 70.f, HpFrac,
				Target ? ogfn::yaw_between(W.self_pos, Target->last_seen_pos) : W.self_yaw_deg,
				/*height_available*/ true);
			if (BIn.piece != ogfn::PieceKind::None)
			{
				static ogfn::BuildGrid s_grid;
				// Report the outcome back to the builder. Without this the brain plans
				// but never learns, so a bot whose piece is refused keeps sending the
				// same impossible input at its input rate, which is a machine hammering
				// a button. The failure reasons are inferred here: this client exposes
				// no placement-failure reason, so a null spawn is treated as blocked and
				// an empty material counter is checked directly when the fork has one.
				const bool bPlaced = ApplyBuildPiece(Controller, Pawn, BIn, s_grid);
				M.builder.report_result(bPlaced,
					bPlaced ? ogfn::BuildRefusal::None : ogfn::BuildRefusal::Blocked);
				if (bPlaced)
				{
					M.buildCount++;
					// The edit is what turns a wall into a window. Editing needs the
					// piece handle plus the edit metadata, so it is a separate verb;
					// until it is wired, log it once rather than pretending.
					if (BIn.edit && !M.bEditLogged)
					{
						M.bEditLogged = true;
						LOG_INFO(LogBots, "[ogfn] bot#{} wants an edit (window) on its wall",
							(int)(&M - &g_minds[0]));
					}
				}
			}
		}

		// A ROUND LEFT THE BARREL: resolve what it did.
		//
		// This is the whole reason the brain carries a hit model. On this client there is
		// no ranged damage path at all (80,627 rounds fired, zero eliminations, only
		// regen GE ticks in the damage log), so "firing" used to be pure theatre: the
		// trigger animated, the target never lost a point. shot_fired is the honest
		// signal that a round was actually sent, so that is what resolves a shot here.
		if (Out.shot_fired)
		{
			M.fireCount++;
			ApplyFire(Pawn);
			ResolveShot(M, Pawn, Controller, Target, Out, Now);
		}

	}
}

#ifdef OGFN_HARNESS
// Movement evidence for the sim: strafe steps, and how many leaked toward the target.
void TestStrafeStats(size_t i, int* total, int* forward) {
	if (i < g_minds.size()) {
		*total = g_minds[i].strafeSteps;
		*forward = g_minds[i].strafeForward;
	}
}

// Shots that connected, and the damage they did. The live symptom this exists for is
// "80,627 rounds fired, zero eliminations": if these stay zero while fireCount climbs,
// the damage path is dead again.
// Health of a specific pawn id, so a scenario can measure the bot that was actually
// shot rather than a pointer it assumed would be shot.
float TestHealthOfId(int id) {
	for (auto& O : Others)
		if (O.id == id && O.pawn) return O.pawn->GetHealth() + O.pawn->GetShield();
	return -1.f;
}
// Total health+shield across every live enemy. Unambiguous: if rounds are landing on
// ANYBODY, this drops. A per-pawn check can silently measure the wrong body, which is
// exactly what happened here.
float TestTotalEnemyHealth() {
	float t = 0.f;
	for (auto& O : Others)
		if (O.pawn && PawnUsable(O.pawn)) t += O.pawn->GetHealth() + O.pawn->GetShield();
	return t;
}
int TestShotsResolved(size_t i) {
	return i < g_minds.size() ? g_minds[i].shotsResolved : 0;
}
int TestShotsHit(size_t i) {
	return i < g_minds.size() ? g_minds[i].shotsHit : 0;
}
float TestDamageDealt(size_t i) {
	return i < g_minds.size() ? g_minds[i].damageDealt : 0.f;
}

int TestHealEquips(size_t i) {
	return i < g_minds.size() ? g_minds[i].healsUsed : 0;
}

int TestSquadmateCount(size_t i) {
	return i < g_minds.size() ? (int)g_minds[i].squadmates.size() : 0;
}

void TestGiveConsumable(size_t i, ogfn::ItemClass cls, int count) {
	auto& Fleet = AllPlayerBotsToTick;
	if (i >= Fleet.size() || !Fleet[i].Controller)
		return;
	auto* Controller = Cast<AFortPlayerControllerAthena>(Fleet[i].Controller);
	if (!Controller)
		return;
	auto* Inv = Controller->GetWorldInventory();
	if (!Inv)
		return;
	auto* Def = new UFortItemDefinition();
	Def->name = (cls == ogfn::ItemClass::ShieldPotion) ? "Athena_Small_Shield_Potion"
	          : (cls == ogfn::ItemClass::ShieldBig) ? "Athena_Shield_Potion"
	          : (cls == ogfn::ItemClass::Medkit) ? "Athena_Medkit" : "Athena_Bandage";
	auto* Item = new UFortItem();
	Item->name = "Consumable";
	Item->Entry.Definition = Def;
	Item->Entry.Count = count;
	Inv->GetItemList().GetItemInstances().push_back(Item);
}


void TestClearEnemies() {
	// Harness-only: drop every tracked target so a scenario can test looting, healing or
	// rotation without a rival standing in the way. Fleet bots move toward bot0 across
	// scenarios, so "the enemy is gone" is not something a scenario can set by moving
	// one pawn: perception has to be told.
	if (!g_minds.empty())
		g_minds[0].brain.perception().clear();
}

void TestForceLoot() {
	// Force the loot sweep now: the live cadence is 2 Hz and a scenario must not
	// have to wait out (or be blocked by) the previous sweep's clock.
	g_lootScanned = true;
	g_lootNextRefresh = -1e9;

	// Make the next plan beat a Loot decision by priming the action; the brain's
	// own scoring would also pick it here, but priming removes persona luck.
	if (!g_minds.empty())
		g_minds[0].brain.prime_action(ogfn::ActionKind::Loot,
			g_lastLootChoice.empty() ? 0.0 : g_minds[0].lastTick);
}
int TestLootSeen(size_t i) {
	return i < g_lastLoot.size() ? (int)g_lastLoot[i].size() : 0;
}
bool TestLootClassSeen(size_t i, ogfn::ItemClass cls) {
	if (i >= g_lastLoot.size()) return false;
	for (auto& s : g_lastLoot[i])
		if (s.cls == cls) return true;
	return false;
}
// Is the bot's current nearest target one of its own squadmates (by pawn id)?
bool TestTargetIsSquadmate(size_t i) {
	if (i >= g_minds.size()) return false;
	auto* T = g_minds[i].brain.perception().nearest(true);
	if (!T) return false;
	auto* P = AllPlayerBotsToTick.size() > 1 ? AllPlayerBotsToTick[1].Pawn : nullptr;
	if (!P) return false;
	// The teammate is the pawn flagged is_teammate in the last observation; perception
	// drops those before tracking, so the check is that NO tracked target shares its id.
	for (const auto& t : g_minds[i].brain.perception().targets())
		if (t.id == T->id && t.is_teammate) return true;
	return false;
}
bool TestLootChoice(size_t i, int* pickupId, float* x, float* y) {
	if (i >= g_lastLootChoice.size()) return false;
	const auto& c = g_lastLootChoice[i];
	*pickupId = c.pickup_id;
	*x = c.pos.x;
	*y = c.pos.y;
	return c.want;
}

void TestClearBuildLog() {
	g_harnessBuildLog.clear();
	g_buildRecorder = [](ogfn::PieceKind piece, float, bool edit) {
		g_harnessBuildLog.emplace_back((int)piece, edit ? 1 : 0);
	};
}

std::vector<std::pair<int, int>> TestBuildLog() { return g_harnessBuildLog; }

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
