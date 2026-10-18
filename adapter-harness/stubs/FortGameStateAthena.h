#pragma once
// STUB of the fork's FortGameStateAthena.h. GetOffset/Get<float> come from UObject:
// the sim registers a "NextRadius" offset with a real value to exercise the
// resolved-zone branch of BuildSnapshot.
#include "reboot.h"

// Mirrors the real enum name/values the adapter compares against.
enum class EAthenaGamePhase : uint8_t { None = 0, Setup = 1, Warmup = 2, Aircraft = 3, SafeZones = 4, EndGame = 5 };

struct AFortGameStateAthena : UObject
{
	EAthenaGamePhase Phase = EAthenaGamePhase::Setup;
	EAthenaGamePhase GetGamePhase() { return Phase; } // real returns a reference; value is fine for == compare
};
