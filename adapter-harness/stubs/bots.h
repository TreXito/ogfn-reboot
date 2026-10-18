#pragma once
// STUB of the fork's bots.h: the fleet container and player-bot handle the adapter reads.
#include "reboot.h"

class PlayerBot
{
public:
	AFortPlayerPawnAthena* Pawn = nullptr;
	AFortPlayerControllerAthena* Controller = nullptr;
	bool bIsAthenaController = true;
};

inline std::vector<PlayerBot> AllPlayerBotsToTick;

namespace OgfnBot { void TickAll(); }
