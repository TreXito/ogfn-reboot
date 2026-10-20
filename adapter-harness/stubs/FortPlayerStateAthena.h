#pragma once
// STUB of the fork's FortPlayerStateAthena.h. Only the team id matters to the
// adapter: it is what separates a squadmate from an enemy.
#include "reboot.h"

struct AFortPlayerStateAthena : AFortPlayerState
{
	int Team = 1;
	uint8_t& GetTeamIndex() { return TeamByte; }
	uint8_t TeamByte = 1;
};
