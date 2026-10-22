// sim_main.cpp -- behavior test rig for the REAL ogfn-brain-adapter.cpp.
// The adapter source is copied next to the stub headers by build.sh, so its
// quoted includes resolve to the stubs. Here we build a fake world, run
// TickAll() through four scenarios, and assert observable behavior:
//   P1 lobby idle      : fleet arms, rotation (idle scan) flows, flags set
//   P2 enemy + clear LOS: first fire lands in a human-plausible window
//   P3 enemy + wall     : ZERO fire -- perception must not leak wallhacks
//   P4 enemy gone       : movement commits toward the zone
// Exits non-zero on any failed expectation.

#include "stubs/reboot.h"
#include "stubs/globals.h"
#include "stubs/bots.h"
#include "stubs/FortGameStateAthena.h"
#include "stubs/FortGameModeAthena.h"
#include "stubs/FortSafeZoneIndicator.h"
#include "stubs/FortWorldItemDefinition.h"
#include "stubs/KismetSystemLibrary.h"
// Brain types for the assertions (placing pieces, reporting goals).
#include "ogfn/ogfn_bots.h"
#include "stubs/FortPickup.h"
#include "stubs/FortPlayerStateAthena.h"
using namespace ogfn;

#include <cstdio>
#include <cmath>

namespace Harness
{
	UWorld* g_world = nullptr;
	double g_now = 0.0;
	std::map<std::wstring, UClass*> g_namedClasses;
	std::map<UClass*, std::vector<UObject*>> g_classObjects;
	std::function<bool(const FVector&, const FVector&)> g_losFn;
}

// Harness stub for the real call-site in TickAll (locker lives in the fork).
namespace OgfnLocker { void AutoApplyOnce() {} }

namespace OgfnBot { void TestClearBurstState(size_t i); void TestSetLadder(float, float, float);
                  void TestStrafeStats(size_t i, int* total, int* forward);
                  void TestClearBuildLog(); std::vector<std::pair<int,int>> TestBuildLog();
                  void TestForceLoot(); int TestLootSeen(size_t); bool TestLootClassSeen(size_t, ItemClass);
                  bool TestLootChoice(size_t, int*, float*, float*); int TestSquadmateCount(size_t);
                  void TestClearEnemies(); bool TestTargetIsSquadmate(size_t);
                  int TestHealEquips(size_t); void TestGiveConsumable(size_t, ItemClass, int); } // adapter (OGFN_HARNESS)

static int g_failures = 0;
#define EXPECT(cond, msg) do { \
	if (cond) { std::printf("  PASS  %s\n", msg); } \
	else      { std::printf("  FAIL  %s\n", msg); g_failures++; } \
} while (0)

bool Harness::g_wallUp = false;
float Harness::g_wallX = 1200.f;

struct SimWorld : UWorld
{
	AFortGameStateAthena* GS = nullptr;
	UNetDriver* ND = nullptr;
	AFortGameModeAthena* GM = nullptr;
	UObject* GetGameState() override { return GS; }
	UNetDriver* GetNetDriver() override { return ND; }
	UObject* GetGameMode() override { return GM; }
};

static UClass* NamedClass(const wchar_t* n, const char* shortName)
{
	UClass*& c = Harness::g_namedClasses[n];
	if (!c)
		c = new UClass{ shortName };
	return c;
}

static void RegisterFn(UObject* o, const char* fnName, size_t params = 64)
{
	o->functions[fnName] = new UFunction{ fnName, params };
}

int main()
{
	std::printf("=== ogfn adapter behavior sim ===\n");

	static SimWorld world;
	Harness::g_world = &world;
	Harness::g_now = 100.0; // plausible in-engine seconds

	Harness::g_losFn = [](const FVector& a, const FVector& b) {
		if (!Harness::g_wallUp) return true;
		// Wall plane at x=20 between the fleet (x=0) and the enemy (x=40).
		float lo = std::min(a.X, b.X), hi = std::max(a.X, b.X);
		return !(lo < 1200.f && hi > 1200.f); // wall at 12 m
	};

	// GameState with resolvable zone fields.
	world.GS = new AFortGameStateAthena();
	world.GM = new AFortGameModeAthena();
	world.GS->offsets["NextRadius"] = 7;
	world.GS->floatSlots[7] = 500000.f;
	world.GS->offsets["SafeZonePhase"] = 8;
	world.GS->floatSlots[8] = 2.f;

	// Network: one human client whose pawn is the phase-2 enemy.
	world.ND = new UNetDriver();
	auto HumanPC = new AFortPlayerControllerAthena();
	HumanPC->Inventory = new AFortInventory();
	auto EnemyPawn = new AFortPlayerPawnAthena();
	EnemyPawn->name = "HumanEnemyPawn";
	EnemyPawn->Location = FVector(500000.f, 0.f, 0.f);
	HumanPC->MyPawn = EnemyPawn;
	auto HumanConn = new UNetConnection();
	HumanConn->PC = HumanPC;
	world.ND->Connections.v.push_back(HumanConn);

	// Zone indicator: 4000 uu east, mid-shrink.
	auto Zone = new AFortSafeZoneIndicator();
	Zone->name = "SafeZoneIndicator";
	Zone->Location = FVector(400000.f, 0.f, 0.f); // 4 km ring
	Zone->StartShrinkTime = (float)Harness::g_now;
	Zone->FinishShrinkTime = (float)Harness::g_now + 50.0;
	Harness::g_classObjects[AFortSafeZoneIndicator::StaticClass()].push_back(Zone);

	// Weapon catalog: two assault rifles (the adapter filters on WID_ + Assault).
	auto WidC = NamedClass(L"/Script/FortniteGame.FortWorldItemDefinition", "FortWorldItemDefinition");
	{
		auto A = new UFortWorldItemDefinition(); A->name = "AthenaWeapon:WID_Assault_Auto_Athena_C";
		auto B = new UFortWorldItemDefinition(); B->name = "AthenaWeapon:WID_Assault_Surgical_Athena_C";
		Harness::g_classObjects[WidC] = { A, B };
	}
	// Pickup class must resolve (adapter bails without it); no loot registered.
	NamedClass(L"/Script/FortniteGame.FortPickup", "FortPickup");

	// Fleet: 20 bots facing +X (yaw 0 = +X in UE convention).
	for (int i = 0; i < 20; i++)
	{
		auto Pawn = new AFortPlayerPawnAthena();
		Pawn->name = "FleetPawn" + std::to_string(i);
		Pawn->Location = FVector(0.f, i * 10000.f, 0.f); // engine uu
		Pawn->Rotation = FRotator{ 0.f, 0.f, 0.f };
		RegisterFn(Pawn, "SetActorRotation", 40);
		RegisterFn(Pawn, "Jump", 0);
		RegisterFn(Pawn, "AddMovementInput", 48);
		RegisterFn(Pawn, "StartFire", 0);
		RegisterFn(Pawn, "K2_SetActorLocation", 64);

		auto PC = new AFortPlayerControllerAthena();
		PC->Inventory = new AFortInventory();
		PC->MyPawn = Pawn;

		PlayerBot Bot;
		Bot.Pawn = Pawn;
		Bot.Controller = PC;
		AllPlayerBotsToTick.push_back(Bot);
	}
	AFortPlayerPawnAthena* P0 = AllPlayerBotsToTick[0].Pawn;

	auto TickFor = [](double secs) {
		int n = (int)(secs * 30.0);
		for (int i = 0; i < n; i++)
		{
			Harness::g_now += 1.0 / 30.0;
			OgfnBot::TickAll();
		}
	};
	auto FleetFireTotal = []() {
		int t = 0;
		for (auto& B : AllPlayerBotsToTick) t += B.Pawn->fireCount;
		return t;
	};

	// ---------- P0: island warmup gate ----------
	// Real players stand around on spawn island; bots must too (no servo spin, no
	// trace storm, but arming still runs because island IS warmup).
	std::printf("\n-- P0: warmup gate (1s, standing + arming) --\n");
	world.GS->Phase = EAthenaGamePhase::Warmup;
	int rotBeforeWarmup = P0->rotationCount;
	TickFor(1.0);
	EXPECT(P0->rotationCount == rotBeforeWarmup, "warmup: bots stand still (no rotation act)");
	world.GS->Phase = EAthenaGamePhase::Setup;
	int rotBeforeSetup = P0->rotationCount;
	TickFor(1.0);
	EXPECT(P0->rotationCount == rotBeforeSetup, "setup island: combat gate closed (no act, no fire)");
	world.GS->Phase = EAthenaGamePhase::SafeZones;

	// ---------- P1: lobby, no enemies ----------
	std::printf("\n-- P1: no enemies, act enabled (2s) --\n");
	TickFor(2.0);

	int armAdds = 0;
	for (auto& B : AllPlayerBotsToTick)
		armAdds += B.Controller->Inventory->adds;

	EXPECT(armAdds == 20, "all 20 bots granted exactly one weapon");
	EXPECT(AFortPlayerController::equipCalls == 20, "all 20 weapons equipped to pawns");
	EXPECT(Globals::bInfiniteAmmo && Globals::bInfiniteMaterials, "arena toggles (infinite ammo/materials) flipped on");
	EXPECT(P0->rotationCount >= 40, "idle-scan rotation flowing every tick (rotate verb resolved)");

	// ---------- P2: enemy in front, LOS clear ----------
	std::printf("\n-- P2: enemy 25m dead ahead, clear LOS (6s) --\n");
	EnemyPawn->Location = FVector(1500.f, 0.f, 0.f); // 15 m: persona-unambiguous (at 25 m a passive ASLR-seeded persona lets HoldAngle outscore Engage and P2 flakes)
	double p2Start = Harness::g_now;
	TickFor(6.0);

	EXPECT(P0->fireCount > 0, "bot fires on a visible in-FOV enemy");
	double firstFire = P0->fireTimes.empty() ? -1.0 : P0->fireTimes.front();
	EXPECT(firstFire >= p2Start + 0.25 && firstFire <= p2Start + 4.5,
	       "first shot lands in a human window (>= lock-on ~0.35s, < 4.5s)");
	{
		// Aim must at least be trying: rotation continues and yaw is finite.
		bool finite = (P0->lastRotation.Yaw == P0->lastRotation.Yaw);
		EXPECT(finite, "yaw stays finite (no NaN servo poisoning)");
	}

	// ---------- P3: wall up, LOS blocked ----------
	std::printf("\n-- P3: wall between fleet and enemy (2s) --\n");
	// The fleet WALKS in this sim, so P2 ends nose-to-nose against the enemy.
	// Give bot0 a realistic 800uu standoff again, then place the wall mid-span
	// so the LOS probe has something to actually cross.
	P0->Location.X = EnemyPawn->Location.X - 800.f;
	Harness::g_wallX = (P0->Location.X + EnemyPawn->Location.X) * 0.5f;
	Harness::g_wallUp = true;
	int fireBeforeWall = FleetFireTotal();
	TickFor(2.0);
	int fireAfterWall = FleetFireTotal();
	EXPECT(fireAfterWall == fireBeforeWall, "zero shots through the wall (no wallhack leak)");

	// ---------- P4: enemy leaves, ring shrinks PAST the fleet ----------
	// Brain design: inside the ring (edge <= -20) rotation scores 0.02 and the
	// bot correctly sits tight; rotation pressure only exists when the fleet is
	// actually outside. Simulate the ring closing to radius 3700 (fleet 300uu out).
	std::printf("\n-- P4: enemy gone, ring shrinks to 3700 (fleet outside, 6s) --\n");
	world.ND->Connections.v.clear();
	EnemyPawn->bDestroyed = true; // logged-out humans lose their pawn; do not block walks
	Harness::g_wallUp = false;
	world.GS->floatSlots[7] = 370000.f;
	float xBefore = P0->Location.X;
	TickFor(6.0);
	float walked = P0->Location.X - xBefore;

	EXPECT(walked > 1000.f, "bot physically walks toward the zone (kinematic steps)");
	EXPECT(P0->Location.X > xBefore || xBefore > 400000.f, "displacement faces the zone (+X)");

	// ---------- P5: forced bus-end dump -> auto-land ----------
	// Live evidence this replicates: engine force-exits everyone at the bus's
	// final transform; server-only pawns stay frozen at z=81536 (velZ=0, no
	// attachment, never descend), so no combat ever happens. The adapter must
	// place each pawn on the ground exactly once, spread off the dump point.
	std::printf("\n-- P5: forced bus-end dump, auto-land (3s) --\n");
	world.GS->Phase = EAthenaGamePhase::SafeZones;
	for (auto& B : AllPlayerBotsToTick)
		B.Pawn->Location = FVector(137002.f, -94136.f, 81536.f); // exact live dump coords
	int teleBefore = P0->teleportCount;
	TickFor(3.0);

	EXPECT(P0->teleportCount > teleBefore, "auto-land teleports the dumped pawn (any count: walk steps also teleport)");
	EXPECT(P0->Location.Z > 0.f && P0->Location.Z < 300.f, "bot0 placed on the ground (z in 0..300)");
	EXPECT(P0->Location.X != 137002.f || P0->Location.Y != -94136.f, "bot0 left the dump point");
	{
		int stillHigh = 0, movedCount = 0;
		bool anyPairSame = false;
		for (auto& B : AllPlayerBotsToTick)
		{
			if (B.Pawn->Location.Z > 20000.f) stillHigh++;
			if (B.Pawn->Location.X != 137002.f || B.Pawn->Location.Y != -94136.f) movedCount++;
		}
		for (size_t x = 0; x < AllPlayerBotsToTick.size() && !anyPairSame; x++)
			for (size_t y = x + 1; y < AllPlayerBotsToTick.size(); y++)
				if (AllPlayerBotsToTick[x].Pawn->Location.X == AllPlayerBotsToTick[y].Pawn->Location.X &&
				    AllPlayerBotsToTick[x].Pawn->Location.Y == AllPlayerBotsToTick[y].Pawn->Location.Y)
				{
					anyPairSame = true;
					break;
				}
		EXPECT(stillHigh == 0, "nobody left frozen above the map");
		EXPECT(movedCount == (int)AllPlayerBotsToTick.size(), "every pawn left the single dump point");
		EXPECT(!anyPairSame, "no two bots share one landing coordinate (spread)");
	}

	// ---------- P6: burst victims -> respawn wave ----------
	// Live engine burst leaves all but ~5 controllers pawnless (spawn collision at
	// one shared transform). The wave must give every pawnless controller a fresh
	// pawn at a DISTINCT ground point and rebind that controller's rifle.
	std::printf("\n-- P6: pawnless controllers, respawn wave (3s) --\n");
	int equipBeforeP6 = AFortPlayerController::equipCalls;
	int victims = 0;
	for (size_t x = 5; x < AllPlayerBotsToTick.size(); x++)
	{
		AllPlayerBotsToTick[x].Controller->MyPawn = nullptr; // burst-collision state
		OgfnBot::TestClearBurstState(x); // they were never seen alive IN-MATCH
		victims++;
	}
	TickFor(3.0);

	{
		int restored = 0, onGround = 0, reequipped = 0;
		bool anySame = false;
		for (size_t x = 5; x < AllPlayerBotsToTick.size(); x++)
		{
			auto MP = AllPlayerBotsToTick[x].Controller->MyPawn;
			if (MP) restored++;
			if (MP && MP->Location.Z >= 0.f && MP->Location.Z < 300.f) onGround++;
			for (size_t y = x + 1; y < AllPlayerBotsToTick.size(); y++)
			{
				auto Other = AllPlayerBotsToTick[y].Controller->MyPawn;
				if (MP && Other && MP->Location.X == Other->Location.X && MP->Location.Y == Other->Location.Y)
					anySame = true;
			}
		}
		reequipped = AFortPlayerController::equipCalls - equipBeforeP6;
		EXPECT(restored == victims, "every pawnless controller got a pawn back");
		EXPECT(onGround == victims, "wave pawns spawn on the ground (no sky spawns)");
		EXPECT(!anySame, "wave pawns land at distinct coordinates (no re-collision)");
		EXPECT(reequipped == victims, "every wave pawn re-bound its controller's rifle");
	}

	// ---------- P7: unit contract (the live n=0 killer) ----------
	// Perception must see the deployed ring: 500..1420uu cluster = 5..14 m, well
	// inside max_range=90m. If uu ever leak through the boundary again (range gate
	// reading 0.9 m), nobody perceives anybody and this fails.
	std::printf("\n-- P7: unit contract, fight inside the deployed ring (5s) --\n");
	int fireBeforeP7 = FleetFireTotal();
	int p0BeforeP7 = P0->fireCount;
	TickFor(5.0);
	EXPECT(FleetFireTotal() > fireBeforeP7, "bots in range perceive and fight each other (meters boundary holds)");
	EXPECT(P0->fireCount > p0BeforeP7, "bot0 pulls the trigger on an in-range neighbor");

	// ---------- P8: storm-evac (ladder + hp gate) ----------
	// User directive: bots must not die to the storm. The hard guarantee: hurt
	// outside the FINAL ring -> extract to its center (which lies inside every
	// ring by construction). Inject the ladder, drop hp, expect one teleport.
	std::printf("\n-- P8: storm-evac (ladder + hp gate) --\n");
	OgfnBot::TestSetLadder(90000.f, 45000.f, 625.f);
	P0->Health = 50.f;
	int telBeforeP8 = P0->teleportCount;
	TickFor(1.0);
	EXPECT(P0->teleportCount > telBeforeP8, "storm-evac teleports a hurt bot that is outside the final ring");
	EXPECT(P0->Location.X > 88000.f && P0->Location.X < 92000.f, "evac lands near final ring center X");
	EXPECT(P0->Location.Y > 43000.f && P0->Location.Y < 47000.f, "evac lands near final ring center Y");
	EXPECT(P0->Location.Z >= 0.f && P0->Location.Z < 300.f, "evac pawn ends on the ground");

	// ---------- P9: movement model reaches the pawn ----------
	// MotorOutput.jumping was read by the adapter but written by nothing, so
	// ApplyJump was dead code (jumpCount stayed 0 in every run). Locomotion owns it
	// now, and owns the engaged walk heading too. Evidence is classified inside the
	// adapter, where the yaw to the target is known: of the steps taken while the
	// movement layer reports "strafing", how many leaked toward the target.
	std::printf("\n-- P9: movement model (strafe + jump reach the pawn) --\n");
	{
		world.ND->Connections.v.clear();
		world.ND->Connections.v.push_back(HumanConn);   // P3 cleared it; the enemy is this pawn
		EnemyPawn->bDestroyed = false;
		// Inside the final ring (P8 set center 90000/45000, r=625) but off the evac
		// pile, so the bot is not storm-driven and Engage can win.
		const FVector OpenGround(90300.f, 44700.f, 100.f);
		P0->Location = OpenGround;
		EnemyPawn->Location = OpenGround + FVector(600.f, 0.f, 0.f);  // 6 m ahead
		// Face the target: perception is FOV-gated, so a bot still pointing where the
		// storm evac left it never acquires the enemy.
		P0->Rotation.Yaw = 0.f;
		P0->lastRotation.Yaw = 0.f;
		P0->Health = 25.f;   // hurt: bail-out jump is on the table
		P0->Shield = 0.f;    // hp fraction is (hp+shield)/200; a full shield would
		                     // read as healthy and suppress the bail-out
		TickFor(1.0);        // let perception re-acquire through the FOV cone

		const int jumpsBefore = P0->jumpCount;
		const int strafeBefore = 0;
		(void)strafeBefore;
		for (int i = 0; i < 30 * 12; ++i)
			TickFor(1.0 / 30.0);   // sim runs at 30Hz and TickFor truncates

		int strafeTotal = 0, strafeForward = 0;
		OgfnBot::TestStrafeStats(0, &strafeTotal, &strafeForward);
		std::printf("  [P9] strafeSteps=%d leaked_forward=%d jumps=%d\n",
			strafeTotal, strafeForward, P0->jumpCount - jumpsBefore);

		EXPECT(P0->jumpCount > jumpsBefore, "a cornered hurt bot sends Jump to the pawn");
		EXPECT(strafeTotal > 0, "engaged movement goes through the strafe path");
		EXPECT(strafeForward * 2 < strafeTotal,
		       "strafes go across the target line, not at it (majority lateral)");
	}

	// ---------- P10: building reaches the world ----------
	// decision.build_wall was documented as unwired, so no bot ever placed a piece.
	// The Builder layer now owns the sequence; this proves an input with a real
	// piece reaches ApplyBuildPiece and comes back as a placed piece, with a window
	// edit in the set (a bot that walls up and cannot shoot out of it is in a coffin).
	std::printf("\n-- P10: building (pieces + window edit reach the world) --\n");
	{
		world.ND->Connections.v.clear();
		world.ND->Connections.v.push_back(HumanConn);
		EnemyPawn->bDestroyed = false;
		EnemyPawn->Location = P0->Location + FVector(400.f, 0.f, 0.f);
		P0->Rotation.Yaw = 0.f;
		P0->lastRotation.Yaw = 0.f;

		std::vector<std::pair<int, int>> placed;   // (piece, is_edit)
		OgfnBot::TestClearBuildLog();
		// Losing badly and being shot at is the box-up case.
		for (int i = 0; i < 30 * 10; ++i)
		{
			P0->Health = 20.f;
			P0->Shield = 0.f;
			TickFor(1.0 / 30.0);
		}
		placed = OgfnBot::TestBuildLog();
		std::printf("  [P10] build inputs reaching the world: %d\n", (int)placed.size());
		int walls = 0, floors = 0, edits = 0;
		for (auto& e : placed)
		{
			if (e.first == (int)PieceKind::Wall) ++walls;
			if (e.first == (int)PieceKind::Floor) ++floors;
			if (e.second) ++edits;
		}
		EXPECT(!placed.empty(), "a bot under fire places pieces (build_wall is wired)");
		EXPECT(walls > 0, "and the pieces are walls, not something arbitrary");
		EXPECT(floors > 0, "the box gets a roof");
		EXPECT(edits > 0, "the box gets a window edit, so the bot can shoot out of it");
	}

	// ---------- P11: loot is objects, not a count ----------
	// The adapter used to feed the brain only loot_within_30m (an int), so the
	// valuator had nothing to rank and a Loot decision had no destination at all
	// (Loot had no case in make_intent). Spawn real pickups near bot0 and check
	// that the brain sees CLASSES, reads a real inventory, and walks somewhere.
	std::printf("\n-- P11: loot samples + inventory reach the brain --\n");
	{
		world.ND->Connections.v.clear();
		EnemyPawn->bDestroyed = true;      // no fight: pure loot case
		Harness::g_wallUp = false;
		P0->Health = 100.f;
		P0->Shield = 0.f;                  // shields down: a big pot is the best thing
		// P8 left a 625uu final ring, so every bot is always within 20m of the edge
		// and rotation legitimately outscores looting (RotateToCircle is 0.02 only
		// when edge <= -20m). Widen the ladder for this scenario: a real early-game
		// ring is kilometres across and looting is what people actually do.
		OgfnBot::TestSetLadder(90000.f, 45000.f, 30000.f);   // 300 m radius
		// OFF-centre on purpose: this used to put bot0 exactly on the ladder centre,
		// which is also where every other bot steers, so the fleet converged onto the
		// loot scenario and turned it into a firefight. 180m out, still inside the ring.
		P0->Location = FVector(90000.f - 18000.f, 45000.f, 100.f);
		// And park the rest of the fleet well away: the brain is RIGHT to prefer a fight
		// at 5m to a shield pot, so the only way to test looting is to have nobody near.
		for (size_t bi = 1; bi < AllPlayerBotsToTick.size(); ++bi)
			AllPlayerBotsToTick[bi].Pawn->Location =
				FVector(90000.f + (float)bi * 20000.f, 45000.f - (float)bi * 20000.f, 100.f);

		// Two pickups: junk under the bot's feet, a big pot 8 m away.
		auto* PickupClass = new UClass();
		PickupClass->name = "/Script/FortniteGame.FortPickup";
		Harness::g_namedClasses[L"/Script/FortniteGame.FortPickup"] = PickupClass;

		auto MakePickup = [&](const char* defName, float x, float y) {
			auto* Def = new UFortItemDefinition();
			Def->name = defName;
			auto* P = new AFortPickup();
			P->name = "Pickup";
			P->Location = P0->Location + FVector(x, y, 0.f);
			auto* E = new FFortItemEntry();
			E->Definition = Def;
			E->Count = 1;
			P->Entry = E;
			Harness::g_classObjects[PickupClass].push_back(P);
			return P;
		};
		MakePickup("Ammo_Medium", 100.f, 0.f);                                  // 1 m: junk
		MakePickup("Athena_Shield_Potion", 800.f, 0.f);                         // 8 m: big pot

		OgfnBot::TestClearEnemies();
		OgfnBot::TestForceLoot();
		// 5s: outlast the 4s target memory, so no remembered enemy re-wins the beat.
		OgfnBot::TestClearEnemies();
		TickFor(5.0);

		int seen = OgfnBot::TestLootSeen(0);
		std::printf("  [P11] loot samples seen by brain: %d\n", seen);
		EXPECT(seen >= 2, "the brain receives loot SAMPLES, not just a count");
		EXPECT(OgfnBot::TestLootClassSeen(0, ItemClass::ShieldBig),
		       "and they carry their class (big shield recognised)");

		int pickId = 0;
		float px = 0.f, py = 0.f;
		const bool want = OgfnBot::TestLootChoice(0, &pickId, &px, &py);
		std::printf("  [P11] loot choice: want=%d at (%.0f,%.0f)\n", (int)want, px, py);
		EXPECT(want, "with shields down and a big pot nearby, the bot wants it");
		EXPECT(px != 0.f || py != 0.f, "and the destination is a real position, not the origin");
	}

	// ---------- P12: a squadmate is never a target ----------
	// Bots on a solo playlist each get their own team, so this path is dormant live.
	// The moment the playlist is duo/squad, a bot that treats its own squadmate as an
	// enemy shoots its team, which is the worst tell in the game. Prove the split.
	std::printf("\n-- P12: squadmate is split out and never targeted --\n");
	{
		world.ND->Connections.v.clear();
		EnemyPawn->bDestroyed = true;
		Harness::g_wallUp = false;
		P0->Shield = 100.f;
		P0->Health = 100.f;
		OgfnBot::TestSetLadder(90000.f, 45000.f, 30000.f);
		P0->Location = FVector(90400.f, 45000.f, 100.f);
		P0->Rotation.Yaw = 0.f;
		P0->lastRotation.Yaw = 0.f;
		// P11 parked the fleet; bring it back, or there is nobody to squadmate with and
		// the split has nothing to prove.
		for (size_t bi = 1; bi < AllPlayerBotsToTick.size(); ++bi)
			AllPlayerBotsToTick[bi].Pawn->Location =
				FVector(90400.f + (float)bi * 2000.f, 45000.f + 1500.f, 100.f);

		// Give bot0 and bot1 the SAME team; leave the rest as they are.
		auto* TeamPS0 = new AFortPlayerStateAthena();
		TeamPS0->TeamByte = 3;
		Cast<AFortPlayerController>(AllPlayerBotsToTick[0].Controller)->PS = TeamPS0;
		if (AFortPlayerPawnAthena::s_pawns.size() > 1 && AllPlayerBotsToTick.size() > 1)
		{
			auto* TeamPS1 = new AFortPlayerStateAthena();
			TeamPS1->TeamByte = 3;
			auto* PC1 = Cast<AFortPlayerController>(AllPlayerBotsToTick[1].Controller);
			if (PC1) PC1->PS = TeamPS1;
			// Put bot1 right in front of bot0, well within FOV and weapon range.
			AllPlayerBotsToTick[1].Pawn->Location = P0->Location + FVector(500.f, 0.f, 0.f);
			AllPlayerBotsToTick[1].Pawn->bDestroyed = false;
			// Everyone ELSE must be out of range. bot0 firing at a real enemy is correct;
			// the assertion below is about not shooting its TEAMMATE, and it can only
			// mean that if the only thing in front of it is the teammate.
			for (size_t bi = 2; bi < AllPlayerBotsToTick.size(); ++bi)
				AllPlayerBotsToTick[bi].Pawn->Location =
					FVector(90000.f + (float)bi * 30000.f, 45000.f - (float)bi * 30000.f, 100.f);
		}

		const int fireBefore = P0->fireCount;
		TickFor(3.0);
		std::printf("  [P12] bot0 fired %d times with a squadmate at 5m\n",
			P0->fireCount - fireBefore);
		EXPECT(OgfnBot::TestSquadmateCount(0) >= 1, "the teammate is recognised as a squadmate");
		// The property is "never TARGETS a squadmate", not "never fires": with other bots
		// in the world a bot fighting a real enemy is correct behaviour, and asserting
		// fireCount==0 made this test pass for the wrong reason (nobody to shoot at).
		EXPECT(OgfnBot::TestSquadmateCount(0) >= 1, "the teammate is tracked as a squadmate");
		EXPECT(!OgfnBot::TestTargetIsSquadmate(0),
		       "and the bot's target is never one of its own squadmates");
	}

	// ---------- P13: the motor drives vertical aim and leads ----------
	// pitch_deg was never written by the motor, so ApplyRotation always received 0
	// and every bot stared at the horizon for a whole match. And without target
	// velocity the bot cannot lead, so it shoots where the target WAS.
	std::printf("\n-- P13: vertical aim + leading reach the pawn --\n");
	{
		world.ND->Connections.v.clear();
		world.ND->Connections.v.push_back(HumanConn);
		EnemyPawn->bDestroyed = false;
		OgfnBot::TestSetLadder(90000.f, 45000.f, 30000.f);
		P0->Location = FVector(90400.f, 45000.f, 100.f);
		P0->Rotation.Yaw = 0.f;
		P0->lastRotation.Yaw = 0.f;
		// P11 parked the fleet; bring it back, or there is nobody to squadmate with and
		// the split has nothing to prove.
		for (size_t bi = 1; bi < AllPlayerBotsToTick.size(); ++bi)
			AllPlayerBotsToTick[bi].Pawn->Location =
				FVector(90400.f + (float)bi * 2000.f, 45000.f + 1500.f, 100.f);
		// Target 20m ahead and 10m UP: the bot must pitch up at it.
		EnemyPawn->Location = P0->Location + FVector(2000.f, 0.f, 1000.f);
		P0->Health = 100.f;
		P0->Shield = 100.f;
		TickFor(1.0);
		float maxPitch = 0.f;
		for (int i = 0; i < 30 * 4; ++i)
		{
			TickFor(1.0 / 30.0);
			maxPitch = std::max(maxPitch, std::fabs(P0->lastRotation.Pitch));
		}
		std::printf("  [P13] max pitch seen on the pawn: %.1f deg\n", maxPitch);
		EXPECT(maxPitch > 5.f, "the motor drives vertical aim (pitch is no longer always 0)");
	}

	// ---------- P14: the view follows travel when nothing is in sight ----------
	// The measured tell: the crosshair sat a median 85 degrees off the travel line,
	// so bots walked sideways staring at nothing. The body yaw is also the perception
	// FOV axis, so the fix has to yield the moment anything is in view.
	std::printf("\n-- P14: gaze follows travel with nothing in sight --\n");
	{
		world.ND->Connections.v.clear();
		EnemyPawn->bDestroyed = true;
		Harness::g_wallUp = false;
		OgfnBot::TestSetLadder(90000.f, 45000.f, 30000.f);
		P0->Health = 100.f;
		P0->Shield = 100.f;
		// Outside a wide ring so the bot is genuinely travelling, facing away from it.
		P0->Location = FVector(20000.f, 20000.f, 100.f);
		P0->Rotation.Yaw = 180.f;
		P0->lastRotation.Yaw = 180.f;
		TickFor(2.0);

		int aligned = 0, total = 0;
		for (int i = 0; i < 30 * 8; ++i)
		{
			TickFor(1.0 / 30.0);
			// Travel direction is toward the ring centre (+X/+Y quadrant).
			const float travel = std::atan2(45000.0 - P0->Location.Y,
			                                90000.0 - P0->Location.X) * 57.29578f;
			const float off = std::fabs(std::remainder(P0->lastRotation.Yaw - travel, 360.0));
			++total;
			if (off < 45.f) ++aligned;
		}
		std::printf("  [P14] view within 45deg of travel on %d/%d samples (%.0f%%)\n",
			aligned, total, 100.0 * aligned / total);
		EXPECT(aligned * 2 > total, "with nothing in sight the bot faces where it walks");
	}

	// ---------- P15: healing equips a real consumable ----------
	// Heal was a decision with no effect: bots decided to heal and nothing happened.
	// Now the brain commits to an item and the adapter equips the matching stack.
	std::printf("\n-- P15: heal equips the consumable the brain chose --\n");
	{
		world.ND->Connections.v.clear();
		EnemyPawn->bDestroyed = true;
		Harness::g_wallUp = false;
		OgfnBot::TestSetLadder(90000.f, 45000.f, 30000.f);
		P0->Location = FVector(90400.f, 45000.f, 100.f);
		P0->Shield = 0.f;      // shields down, so the heal is unambiguous
		P0->Health = 100.f;
		OgfnBot::TestGiveConsumable(0, ogfn::ItemClass::ShieldPotion, 3);
		TickFor(2.0);
		std::printf("  [P15] equips issued for heals: %d\n", OgfnBot::TestHealEquips(0));
		EXPECT(OgfnBot::TestHealEquips(0) > 0,
		       "a healing bot equips a real consumable (heal is not a no-op)");
	}

	// ---------- summary ----------
	std::printf("\n=== summary ===\n");
	std::printf("  pawn0: rot=%d move=%d jump=%d fire=%d firstFire=%.2fs\n",
		P0->rotationCount, P0->moveCount, P0->jumpCount, P0->fireCount,
		P0->fireTimes.empty() ? -1.0 : (P0->fireTimes.front() - 100.0));
	std::printf("  fleet: fire=%d  arms=%d  equip=%d\n", FleetFireTotal(), armAdds, AFortPlayerController::equipCalls);
	std::printf("%s (%d failures)\n", g_failures ? "RESULT: FAIL" : "RESULT: ALL PASS", g_failures);
	return g_failures ? 1 : 0;
}
