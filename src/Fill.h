#pragma once

#include <API/ARK/Ark.h>
#include <string>

namespace sb
{
	// Default reach for /sc fill, in world units. A 14x14 tower is about 4200
	// across, so this comfortably covers one from the middle without spilling
	// into a neighbouring base.
	constexpr float kDefaultFillRadius = 10000.f;

	struct FillStats
	{
		int turrets = 0;       // turrets found in range
		int filled = 0;        // ...that took ammo
		int already_full = 0;  // ...that had no free slots
		int no_inventory = 0;  // skipped: no inventory component
		int no_template = 0;   // skipped: turret declares no ammo type
		int stacks_added = 0;
		long long rounds_added = 0;
	};

	// Loads every turret within `radius` of `center` with its own declared ammo.
	//
	// Fills to capacity by default: free slots x the ammo's own max stack size,
	// both read off the turret rather than assumed. `per_stack` overrides the
	// stack size when non-zero.
	//
	// The ammo class is read from APrimalStructureTurret::AmmoItemTemplate rather
	// than mapped from a hardcoded table, so Heavy takes bullets, Tek takes
	// element, and modded turrets take whatever they were built to use - without
	// this plugin needing to know about any of them.
	bool FillTurrets(const FVector& center, float radius, int per_stack,
	                 FillStats& stats, std::string& error);
} // namespace sb
