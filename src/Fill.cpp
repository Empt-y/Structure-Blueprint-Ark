#include "Fill.h"
#include "ArkUtils.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace sb
{
	namespace
	{
		// Field offsets come from the PDB by name, so reading a turret field off a
		// non-turret would return whatever happens to live at that offset. Gate on
		// the blueprint path before casting.
		//
		// This is a name heuristic and will miss a modded turret that does not say
		// "turret" in its path - which fails safe: it is skipped, not corrupted.
		bool LooksLikeTurret(AActor* actor)
		{
			const FString path = GetBlueprintPath(actor);
			if (path.Len() == 0) return false;

			std::string s = path.ToString();
			std::transform(s.begin(), s.end(), s.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s.find("turret") != std::string::npos;
		}
	} // namespace

	bool FillTurrets(const FVector& center, float radius, int per_stack,
	                 FillStats& stats, std::string& error)
	{
		stats = FillStats{};

		if (radius <= 0.f) { error = "radius must be positive"; return false; }

		// Turrets are ordinary structures, so the same two octree groups capture
		// uses cover them; dedupe because a piece can appear in both.
		std::unordered_set<AActor*> seen;
		for (EServerOctreeGroup::Type group : { EServerOctreeGroup::STRUCTURES,
		                                        EServerOctreeGroup::STRUCTURES_CORE })
		{
			TArray<AActor*> found = ArkApi::GetApiUtils().GetAllActorsInRange(center, radius, group);
			for (int i = 0; i < found.Num(); ++i)
				if (found[i] != nullptr) seen.insert(found[i]);
		}

		for (AActor* actor : seen)
		{
			if (!LooksLikeTurret(actor)) continue;
			++stats.turrets;

			auto* turret = static_cast<APrimalStructureTurret*>(actor);

			UPrimalInventoryComponent* inventory = turret->MyInventoryComponentField();
			if (inventory == nullptr) { ++stats.no_inventory; continue; }

			TSubclassOf<UPrimalItem> ammo = turret->AmmoItemTemplateField();
			if (ammo.uClass == nullptr) { ++stats.no_template; continue; }

			// Fill to capacity rather than adding a fixed amount: free slots
			// times the ammo's own stack size. Both come off the game - the
			// inventory knows its slot count, the item class knows its stack.
			int stack = per_stack;
			if (stack <= 0)
			{
				stack = 1;
				if (UObject* cdo = ammo.uClass->GetDefaultObject(true))
					if (const int max_q = static_cast<UPrimalItem*>(cdo)->MaxItemQuantityField())
						stack = max_q;
			}

			const int capacity = inventory->MaxInventoryItemsField();
			const int used = inventory->InventoryItemsField().Num();
			const int free_slots = (capacity > 0) ? (capacity - used) : 0;

			if (free_slots <= 0) { ++stats.already_full; continue; }

			bool any = false;
			for (int slot = 0; slot < free_slots; ++slot)
			{
				UPrimalItem* item = UPrimalItem::AddNewItem(
					ammo, inventory, false,
					true,          // bDontStack: one full stack per slot
					0.0f, true, stack, false, 0.0f, false,
					TSubclassOf<UPrimalItem>(), 0.0f, false, true);
				if (item == nullptr) break;

				any = true;
				++stats.stacks_added;
				stats.rounds_added += stack;
			}

			if (any) ++stats.filled;
		}

		if (stats.turrets == 0) { error = "no turrets within range"; return false; }
		return true;
	}
} // namespace sb
