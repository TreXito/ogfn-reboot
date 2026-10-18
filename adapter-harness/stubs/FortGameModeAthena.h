#pragma once
// STUB: adapter only needs StartAircraftPhase (force-bus trigger) from the game mode.
#include "reboot.h"

struct AFortGameModeAthena : UObject
{
	static inline int s_aircraftCalls = 0;
	void StartAircraftPhase() { s_aircraftCalls++; }
};
