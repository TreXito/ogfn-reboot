#pragma once
// STUB: adapter only needs StartAircraftPhase (force-bus trigger) from the game mode.
#include "reboot.h"

struct AFortGameModeAthena : UObject
{
	static inline int s_aircraftCalls = 0;
	static inline int s_restartCalls = 0;
	static inline int s_waveSpawns = 0;
	void StartAircraftPhase() { s_aircraftCalls++; }

	// Engine restart-at-transform: spawn a fresh pawn at Xf, hand it to the
	// controller (possess lives inside the engine call in the real build).
	void RestartPlayerAtTransform(AFortPlayerControllerAthena* C, FTransform Xf)
	{
		s_restartCalls++;
		auto P = new AFortPlayerPawnAthena();
		P->name = "WavePawn" + std::to_string(s_restartCalls);
		P->Location = Xf.Translation;
		C->MyPawn = P;
		s_waveSpawns++;
	}
};
