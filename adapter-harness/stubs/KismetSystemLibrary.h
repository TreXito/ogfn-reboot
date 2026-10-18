#pragma once
// STUB of the fork's KismetSystemLibrary.h -- exact LineTraceSingle signature the
// adapter calls, backed by the SIM's line-of-sight function (walls included), so
// the brain's no-wallhack rule is testable end to end.
#include "reboot.h"
#include <functional>

namespace Harness
{
	extern std::function<bool(const FVector&, const FVector&)> g_losFn;
}

struct UKismetSystemLibrary
{
	static bool LineTraceSingle(UObject* /*world*/, FVector Start, FVector End,
	                            ETraceTypeQuery /*channel*/, bool /*complex*/,
	                            TArray<AActor*> /*ignore*/, EDrawDebugTrace /*debug*/,
	                            FHitResult** OutHit, bool /*ignoreSelf*/,
	                            FLinearColor, FLinearColor, float)
	{
		bool bClear = Harness::g_losFn ? Harness::g_losFn(Start, End) : true;
		if (OutHit)
		{
			auto* Hit = new FHitResult();
			Hit->bBlocking = !bClear;
			*OutHit = Hit;
		}
		return !bClear;
	}
};
