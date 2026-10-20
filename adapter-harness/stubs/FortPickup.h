#pragma once
// STUB of the fork's FortPickup.h. The adapter only needs: a pickup is an actor,
// it owns a primary item entry, and the entry knows its definition and count.
#include "reboot.h"

struct AFortPickup : AActor
{
	FFortItemEntry* Entry = nullptr;
	FFortItemEntry* GetPrimaryPickupItemEntry() { return Entry; }
};
