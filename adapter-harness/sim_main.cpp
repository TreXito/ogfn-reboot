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
#include "stubs/FortSafeZoneIndicator.h"
#include "stubs/FortWorldItemDefinition.h"
#include "stubs/KismetSystemLibrary.h"

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

static int g_failures = 0;
#define EXPECT(cond, msg) do { \
	if (cond) { std::printf("  PASS  %s\n", msg); } \
	else      { std::printf("  FAIL  %s\n", msg); g_failures++; } \
} while (0)

static bool g_wallUp = false;

struct SimWorld : UWorld
{
	AFortGameStateAthena* GS = nullptr;
	UNetDriver* ND = nullptr;
	UObject* GetGameState() override { return GS; }
	UNetDriver* GetNetDriver() override { return ND; }
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
		if (!g_wallUp) return true;
		// Wall plane at x=20 between the fleet (x=0) and the enemy (x=40).
		float lo = std::min(a.X, b.X), hi = std::max(a.X, b.X);
		return !(lo < 12.f && hi > 12.f);
	};

	// GameState with resolvable zone fields.
	world.GS = new AFortGameStateAthena();
	world.GS->offsets["NextRadius"] = 7;
	world.GS->floatSlots[7] = 5000.f;
	world.GS->offsets["SafeZonePhase"] = 8;
	world.GS->floatSlots[8] = 2.f;

	// Network: one human client whose pawn is the phase-2 enemy.
	world.ND = new UNetDriver();
	auto HumanPC = new AFortPlayerControllerAthena();
	HumanPC->Inventory = new AFortInventory();
	auto EnemyPawn = new AFortPlayerPawnAthena();
	EnemyPawn->name = "HumanEnemyPawn";
	EnemyPawn->Location = FVector(5000.f, 0.f, 0.f);
	HumanPC->MyPawn = EnemyPawn;
	auto HumanConn = new UNetConnection();
	HumanConn->PC = HumanPC;
	world.ND->Connections.v.push_back(HumanConn);

	// Zone indicator: 4000 uu east, mid-shrink.
	auto Zone = new AFortSafeZoneIndicator();
	Zone->name = "SafeZoneIndicator";
	Zone->Location = FVector(4000.f, 0.f, 0.f);
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
		Pawn->Location = FVector(0.f, i * 100.f, 0.f);
		Pawn->Rotation = FRotator{ 0.f, 0.f, 0.f };
		RegisterFn(Pawn, "SetActorRotation", 40);
		RegisterFn(Pawn, "Jump", 0);
		RegisterFn(Pawn, "AddMovementInput", 48);
		RegisterFn(Pawn, "StartFire", 0);

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
	EnemyPawn->Location = FVector(25.f, 0.f, 0.f);
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
	g_wallUp = true;
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
	g_wallUp = false;
	world.GS->floatSlots[7] = 3700.f;
	int moveBefore = P0->moveCount;
	TickFor(6.0);
	int moveDelta = P0->moveCount - moveBefore;

	EXPECT(moveDelta >= 8, "bot commits to movement toward the zone");
	// Direction evidence: last movement input should point east (+X).
	EXPECT(P0->lastMoveDir.X > 0.5f, "movement direction faces the zone (+X)");

	// ---------- summary ----------
	std::printf("\n=== summary ===\n");
	std::printf("  pawn0: rot=%d move=%d jump=%d fire=%d firstFire=%.2fs\n",
		P0->rotationCount, P0->moveCount, P0->jumpCount, P0->fireCount,
		P0->fireTimes.empty() ? -1.0 : (P0->fireTimes.front() - 100.0));
	std::printf("  fleet: fire=%d  arms=%d  equip=%d\n", FleetFireTotal(), armAdds, AFortPlayerController::equipCalls);
	std::printf("%s (%d failures)\n", g_failures ? "RESULT: FAIL" : "RESULT: ALL PASS", g_failures);
	return g_failures ? 1 : 0;
}
