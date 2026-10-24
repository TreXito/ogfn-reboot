#pragma once
// STUB of the fork's FortPickup.h. The adapter only needs: a pickup is an actor,
// it owns a primary item entry, and the entry knows its definition and count.
#include "reboot.h"

struct AFortPickup : AActor
{
	FFortItemEntry* Entry = nullptr;
	FFortItemEntry* GetPrimaryPickupItemEntry() { return Entry; }

	// Real AFortPickup API (FortPickup.h:211, FortPlayerPawn.h:52). The adapter calls
	// BOTH to take an item, exactly like the fork's capsule-overlap path does for a
	// human player, so the stub must carry both or the pickup path cannot be tested.
	bool bPickedUp = false;
	AFortPlayerPawn* PickupTarget = nullptr;   // who is collecting it
	static inline int s_completed = 0;
	// Real body (FortPickup.cpp:309) has NO bPickedUp bail: it resolves the target pawn,
	// adds the entry to the inventory and destroys the pickup. The stub invented a
	// `bPickedUp` guard, which made every pickup look like it failed to complete right
	// after ServerHandlePickupHook had set that flag. Stubs must mirror the client, not
	// the caller's assumptions.
	static char CompletePickupAnimationHook(AFortPickup* Pickup)
	{
		if (!Pickup) return 0;
		Pickup->bPickedUp = true;
		Pickup->bDestroyed = true;   // the item left the world
		// The real hook puts the entry in the OWNER's inventory (FortPickup.cpp:321-349)
		// before the pickup disappears. Without this, "picked up" meant nothing: the bot
		// walked to a rifle, the pickup vanished, and the bot was still empty-handed, so
		// anything that then equipped-the-best-it-carried found nothing.
		auto* AC = Pickup->PickupTarget
		             ? dynamic_cast<AFortPlayerControllerAthena*>(Pickup->PickupTarget->Controller)
		             : nullptr;
		if (AC && Pickup->Entry)
		{
			auto* Inv = AC->GetWorldInventory();
			if (Inv)
			{
				auto* Item = new UFortItem();
				Item->name = "LootedItem";
				Item->Entry = *Pickup->Entry;
				Inv->GetItemList().GetItemInstances().push_back(Item);
			}
		}
		s_completed++;
		return 1;
	}
};



// Defined here (not reboot.h) because the body needs AFortPickup complete, and it is
// inline so every TU that calls it sees the same definition.
inline void AFortPlayerPawn::ServerHandlePickupHook(AFortPlayerPawn* Pawn, AFortPickup* Pickup,
                                                   float InFlyTime, FVector InStartDirection,
                                                   bool bPlayPickupSound)
{
	(void)InFlyTime; (void)InStartDirection; (void)bPlayPickupSound;
	if (!Pickup || Pickup->bPickedUp) return;
	Pickup->bPickedUp = true;
	Pickup->PickupTarget = Pawn;   // who will collect it
	s_handleCalls++;
}
