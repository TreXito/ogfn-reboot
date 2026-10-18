#pragma once
// STUB of the fork's FortSafeZoneIndicator.h: the circle lives on this actor --
// its Location is the center, its shrink times feed circle_shrink_in_secs,
// and it may carry its own NextRadius (the fallback path).
#include "reboot.h"

struct AFortSafeZoneIndicator : AActor
{
	float StartShrinkTime = 0.f;
	float FinishShrinkTime = 0.f;

	float GetSafeZoneStartShrinkTime() { return StartShrinkTime; }
	float GetSafeZoneFinishShrinkTime() { return FinishShrinkTime; }

	static UClass* StaticClass()
	{
		static UClass C{ "AFortSafeZoneIndicator" };
		return &C;
	}
};
