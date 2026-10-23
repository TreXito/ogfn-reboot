#pragma once
// STUB: mirrors the real BuildingSMActor/BuildingActor surface the adapter uses.
// Real headers: BuildingSMActor.h (ABuildingSMActor : ABuildingActor) and
// BuildingActor.h (InitializeBuildingActor, SilentDie, GetHealth).
#include "reboot.h"

struct ABuildingSMActor : AActor
{
	static inline int s_playerPlaced = 0;
	static inline int s_initialised = 0;
	static inline int s_silentDie = 0;

	void SetPlayerPlaced(bool b) { if (b) s_playerPlaced++; }

	// Real signature: (UObject* Controller, ABuildingActor* BuildingOwner,
	//                   bool bUsePlayerBuildAnimations, UObject* ReplacedBuilding)
	void InitializeBuildingActor(UObject* Controller, AActor* BuildingOwner, bool bUsePlayerBuildAnimations,
	                             UObject* ReplacedBuilding = nullptr)
	{
		(void)Controller; (void)BuildingOwner; (void)bUsePlayerBuildAnimations; (void)ReplacedBuilding;
		s_initialised++;
	}

	void SilentDie() { s_silentDie++; }
	bool IsDestroyed() { return bDestroyed; }
};
