#pragma once
// STUB of the fork's KismetSystemLibrary.h -- exact LineTraceSingle signature the
// adapter calls, backed by the SIM's line-of-sight function (walls included), so
// the brain's no-wallhack rule is testable end to end.
#include "reboot.h"
#include <functional>

namespace Harness
{
	extern std::function<bool(const FVector&, const FVector&)> g_losFn;
	extern bool g_wallUp;   // wall plane also COLLIDES movement
	extern float g_wallX;   // plane position (P3 sets it between fleet and enemy)
}

struct UKismetSystemLibrary
{
	static bool LineTraceSingle(UObject* /*world*/, FVector Start, FVector End,
	                            ETraceTypeQuery /*channel*/, bool /*complex*/,
	                            TArray<AActor*> /*ignore*/, EDrawDebugTrace /*debug*/, bool /*ignoreSelf*/,
	                            FLinearColor, FLinearColor, float,
	                            FHitResult** OutHit)
	{
		// Down-probe (dz beyond 5000uu): ground plane at z=0 under the start
		// column. Everything else: the sim's LOS function (walls included).
		bool bClear;
		FVector HitLoc;
		if (End.Z < Start.Z - 5000.f)
		{
			HitLoc = FVector(Start.X, Start.Y, 0.f);
			bClear = false;
		}
		else
		{
			bClear = Harness::g_losFn ? Harness::g_losFn(Start, End) : true;
			HitLoc = End;
		}
		if (OutHit)
		{
			auto* Hit = new FHitResult();
			Hit->bBlocking = !bClear;
			Hit->Location = HitLoc;
			*OutHit = Hit;
		}
		return !bClear;
	}
};
