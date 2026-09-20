#include "Capture.h"
#include "ArkUtils.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace sb
{
	FVector Selection::Min() const
	{
		return FVector{ std::min(a.X, b.X), std::min(a.Y, b.Y), std::min(a.Z, b.Z) };
	}
	FVector Selection::Max() const
	{
		return FVector{ std::max(a.X, b.X), std::max(a.Y, b.Y), std::max(a.Z, b.Z) };
	}

	static std::unordered_map<uint64, Selection> g_selections;

	Selection& SelectionFor(uint64 steam_id) { return g_selections[steam_id]; }
	void ClearSelection(uint64 steam_id) { g_selections.erase(steam_id); }

	namespace
	{
		std::string UtcNow()
		{
			const std::time_t t = std::time(nullptr);
			std::tm tm{};
			gmtime_s(&tm, &t);
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
			return buf;
		}

		bool InBox(const FVector& p, const FVector& lo, const FVector& hi)
		{
			return p.X >= lo.X && p.X <= hi.X
			    && p.Y >= lo.Y && p.Y <= hi.Y
			    && p.Z >= lo.Z && p.Z <= hi.Z;
		}

		struct Sphere { FVector center; float radius; };

		Sphere EnclosingSphere(const FVector& lo, const FVector& hi)
		{
			const float dx = hi.X - lo.X, dy = hi.Y - lo.Y, dz = hi.Z - lo.Z;
			return Sphere{
				FVector{ (lo.X + hi.X) * 0.5f, (lo.Y + hi.Y) * 0.5f, (lo.Z + hi.Z) * 0.5f },
				0.5f * std::sqrt(dx * dx + dy * dy + dz * dz)
			};
		}

		// ARK does not keep every structure in one octree group - foundations and
		// other load-bearing pieces sit in STRUCTURES_CORE rather than STRUCTURES.
		// Querying a single group silently loses whole categories of build, so
		// union several and dedupe by pointer. Cheaper than guessing at the
		// bitmask overload's encoding, and unambiguous.
		const EServerOctreeGroup::Type kCaptureGroups[] = {
			EServerOctreeGroup::STRUCTURES,
			EServerOctreeGroup::STRUCTURES_CORE,
		};

		std::vector<AActor*> CollectInSphere(const Sphere& s,
		                                     const EServerOctreeGroup::Type* groups, int group_count)
		{
			std::unordered_set<AActor*> seen;
			std::vector<AActor*> out;

			for (int g = 0; g < group_count; ++g)
			{
				TArray<AActor*> found = ArkApi::GetApiUtils()
					.GetAllActorsInRange(s.center, s.radius, groups[g]);

				for (int i = 0; i < found.Num(); ++i)
				{
					AActor* a = found[i];
					if (a == nullptr) continue;
					if (seen.insert(a).second) out.push_back(a);
				}
			}
			return out;
		}
	} // namespace

	std::vector<AActor*> CollectSelection(const Selection& sel)
	{
		std::vector<AActor*> out;
		if (!sel.Complete()) return out;

		const FVector lo = sel.Min(), hi = sel.Max();
		for (AActor* a : CollectInSphere(EnclosingSphere(lo, hi), kCaptureGroups,
		                                 static_cast<int>(std::size(kCaptureGroups))))
		{
			FVector p; FRotator r;
			if (!ReadTransform(a, p, r)) continue;
			if (InBox(p, lo, hi)) out.push_back(a);
		}
		return out;
	}

	std::vector<ScanRow> ScanGroups(const Selection& sel)
	{
		std::vector<ScanRow> rows;
		if (!sel.Complete()) return rows;

		const FVector lo = sel.Min();
		const FVector hi = sel.Max();
		const Sphere s = EnclosingSphere(lo, hi);

		struct Named { const char* name; EServerOctreeGroup::Type type; };
		static const Named kGroups[] = {
			{ "STRUCTURES",        EServerOctreeGroup::STRUCTURES },
			{ "STRUCTURES_CORE",   EServerOctreeGroup::STRUCTURES_CORE },
			{ "THERMALSTRUCTURES", EServerOctreeGroup::THERMALSTRUCTURES },
			{ "TARGETABLEACTORS",  EServerOctreeGroup::TARGETABLEACTORS },
			{ "ALL_SPATIAL",       EServerOctreeGroup::ALL_SPATIAL },
		};

		for (const Named& g : kGroups)
		{
			TArray<AActor*> found = ArkApi::GetApiUtils().GetAllActorsInRange(s.center, s.radius, g.type);

			ScanRow row;
			row.name = g.name;
			row.total = found.Num();

			for (int i = 0; i < found.Num(); ++i)
			{
				AActor* a = found[i];
				if (a == nullptr) continue;
				FVector p; FRotator r;
				if (!ReadTransform(a, p, r)) continue;
				if (InBox(p, lo, hi)) ++row.in_box;
			}
			rows.push_back(row);
		}
		return rows;
	}

	std::vector<MissedRow> ScanMissed(const Selection& sel, int& level_total, int& level_in_box)
	{
		level_total = 0;
		level_in_box = 0;
		std::vector<MissedRow> rows;
		if (!sel.Complete()) return rows;

		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (world == nullptr || world->PersistentLevelField() == nullptr) return rows;

		const FVector lo = sel.Min();
		const FVector hi = sel.Max();

		// What capture would currently see.
		std::unordered_set<AActor*> seen_by_octree;
		for (AActor* a : CollectInSphere(EnclosingSphere(lo, hi), kCaptureGroups,
		                                 static_cast<int>(std::size(kCaptureGroups))))
			seen_by_octree.insert(a);

		std::unordered_map<std::string, int> tally;

		const TArray<AActor*> all = world->PersistentLevelField()->GetActorsField();
		level_total = all.Num();

		for (int i = 0; i < all.Num(); ++i)
		{
			AActor* a = all[i];
			if (a == nullptr) continue;

			FVector p; FRotator r;
			if (!ReadTransform(a, p, r)) continue;
			if (!InBox(p, lo, hi)) continue;

			++level_in_box;
			if (seen_by_octree.find(a) != seen_by_octree.end()) continue;

			const FString path = GetBlueprintPath(a);
			std::string name = path.Len() > 0 ? path.ToString() : std::string("<no blueprint path>");
			const size_t dot = name.rfind('.');
			if (dot != std::string::npos) name = name.substr(dot + 1);
			// Blueprint paths end in a single quote: Blueprint'/Game/....Foo'
			if (!name.empty() && name.back() == 0x27) name.pop_back();
			++tally[name];
		}

		for (const auto& kv : tally) rows.push_back(MissedRow{ kv.first, kv.second });
		std::sort(rows.begin(), rows.end(),
			[](const MissedRow& a, const MissedRow& b) { return a.count > b.count; });
		return rows;
	}

	LinkStats InspectLinks(const Selection& sel)
	{
		LinkStats st;
		if (!sel.Complete()) return st;

		const FVector lo = sel.Min();
		const FVector hi = sel.Max();
		const std::vector<AActor*> actors = CollectInSphere(
			EnclosingSphere(lo, hi), kCaptureGroups, static_cast<int>(std::size(kCaptureGroups)));

		for (AActor* actor : actors)
		{
			FVector p; FRotator r;
			if (!ReadTransform(actor, p, r)) continue;
			if (!InBox(p, lo, hi)) continue;

			auto* structure = static_cast<APrimalStructure*>(actor);
			++st.structures;

			if (structure->PlacedOnFloorStructureField() != nullptr) ++st.with_placed_on_floor;
			if (structure->PrimarySnappedStructureChildField() != nullptr) ++st.with_snapped_child;

			const TArray<APrimalStructure*> linked = structure->LinkedStructuresField();
			if (linked.Num() > 0) { ++st.with_linked; st.linked_refs += linked.Num(); }

			const TArray<APrimalStructure*>& on_floor = structure->StructuresPlacedOnFloorField();
			if (on_floor.Num() > 0) { ++st.with_on_floor_list; st.on_floor_refs += on_floor.Num(); }
		}
		return st;
	}

	bool CaptureSelection(const Selection& sel, const std::string& name,
	                      uint64 by_steam_id, float margin, bool with_items,
	                      Blueprint& out, CaptureStats& stats, std::string& error)
	{
		stats = CaptureStats{};

		if (!sel.Complete()) { error = "set both corners first"; return false; }

		// Grow the box before testing. Structure origins sit at the centre of a
		// tile, so the foundation an admin stands on to set a corner is usually
		// just outside the raw selection.
		const FVector raw_lo = sel.Min();
		const FVector raw_hi = sel.Max();
		const FVector lo{ raw_lo.X - margin, raw_lo.Y - margin, raw_lo.Z - margin };
		const FVector hi{ raw_hi.X + margin, raw_hi.Y + margin, raw_hi.Z + margin };
		const Sphere sphere = EnclosingSphere(lo, hi);

		const std::vector<AActor*> actors = CollectInSphere(
			sphere, kCaptureGroups, static_cast<int>(std::size(kCaptureGroups)));
		stats.octree_returned = static_cast<int>(actors.size());

		// First pass: keep the in-box structures and remember where each one
		// landed, so the second pass can turn parent pointers into indices.
		std::vector<APrimalStructure*> kept;
		std::unordered_map<APrimalStructure*, int32_t> index_of;
		std::unordered_map<std::string, int32_t> class_ids;
		std::unordered_map<std::string, int32_t> item_ids;

		kept.reserve(actors.size());

		for (AActor* actor : actors)
		{
			auto* structure = static_cast<APrimalStructure*>(actor);

			FVector loc; FRotator rot;
			if (!ReadTransform(structure, loc, rot)) { ++stats.no_transform; continue; }
			if (!InBox(loc, lo, hi))
			{
				// A structure sitting just beyond the edge almost always means the
				// selection clipped the build rather than that a neighbour happened
				// to be nearby. Silently dropping these is what let a 3x3 come back
				// as a 3x2 with no indication anything was lost.
				const FVector pad{ 500.f, 500.f, 500.f };
				const FVector wide_lo{ lo.X - pad.X, lo.Y - pad.Y, lo.Z - pad.Z };
				const FVector wide_hi{ hi.X + pad.X, hi.Y + pad.Y, hi.Z + pad.Z };
				if (InBox(loc, wide_lo, wide_hi)) ++stats.near_miss;
				continue;
			}

			++stats.in_box;
			index_of[structure] = static_cast<int32_t>(kept.size());
			kept.push_back(structure);
		}

		if (kept.empty()) { error = "no structures inside the selection"; return false; }

		const float dx = hi.X - lo.X, dy = hi.Y - lo.Y, dz = hi.Z - lo.Z;

		out = Blueprint{};
		out.name = name;
		out.created_utc = UtcNow();
		out.created_by = std::to_string(by_steam_id);
		out.origin_x = lo.X; out.origin_y = lo.Y; out.origin_z = lo.Z;
		out.size_x = dx; out.size_y = dy; out.size_z = dz;
		out.pieces.reserve(kept.size());

		// kept index -> index in out.pieces, or -1 when that structure was
		// skipped. Without this remap the link indices would drift whenever a
		// piece failed to serialise, silently wiring pieces to wrong neighbours.
		std::vector<int32_t> kept_to_piece(kept.size(), -1);

		for (size_t k = 0; k < kept.size(); ++k)
		{
			APrimalStructure* structure = kept[k];

			FVector loc; FRotator rot;
			if (!ReadTransform(structure, loc, rot)) { ++stats.no_transform; continue; }

			const FString bp_path = GetBlueprintPath(structure);
			if (bp_path.Len() == 0) { ++stats.no_path; continue; }

			const std::string key = bp_path.ToString();
			auto it = class_ids.find(key);
			if (it == class_ids.end())
			{
				it = class_ids.emplace(key, static_cast<int32_t>(out.classes.size())).first;
				out.classes.push_back(key);
			}

			Piece piece;
			piece.class_index = it->second;
			piece.x = loc.X - lo.X;
			piece.y = loc.Y - lo.Y;
			piece.z = loc.Z - lo.Z;
			piece.pitch = rot.Pitch;
			piece.yaw = rot.Yaw;
			piece.roll = rot.Roll;

			// --- container contents and device state -----------------------
			if (with_items && IsA(structure, ContainerClass()))
			{
				auto* container = static_cast<APrimalStructureItemContainer*>(structure);
				piece.activated = container->bContainerActivated()() ? 1 : 0;

				if (UPrimalInventoryComponent* inv = container->MyInventoryComponentField())
				{
					++stats.containers;
					const TArray<UPrimalItem*> contents = inv->InventoryItemsField();

					for (int n = 0; n < contents.Num(); ++n)
					{
						UPrimalItem* item = contents[n];
						if (item == nullptr) continue;

						const FString ipath = GetBlueprintPath(item);
						if (ipath.Len() == 0) continue;

						const std::string ikey = ipath.ToString();
						auto iit = item_ids.find(ikey);
						if (iit == item_ids.end())
						{
							iit = item_ids.emplace(ikey,
								static_cast<int32_t>(out.item_classes.size())).first;
							out.item_classes.push_back(ikey);
						}

						ItemEntry entry;
						entry.cls = iit->second;
						entry.quantity = item->ItemQuantityField();
						entry.durability = item->ItemDurabilityField();
						entry.skill_bonus = item->CraftedSkillBonusField();
						entry.blueprint = item->bIsBlueprint()();
						entry.name = item->CustomItemNameField().ToString();

						// Quality is not a single field - it lives in the stat
						// array, so carry the array verbatim.
						if (unsigned short* sv = item->ItemStatValuesField()())
							entry.stats.assign(sv, sv + 8);

						piece.items.push_back(std::move(entry));
						++stats.item_stacks;
					}
				}
			}

			if (with_items && IsA(structure, TurretClass()))
			{
				++stats.turrets;
				auto* turret = static_cast<APrimalStructureTurret*>(structure);
				if (float* r = turret->TargetingRangesField()())
					piece.ranges.assign(r, r + 3);

				// Which of the three ranges is actually selected, and how much
				// ammo is in it. The class default said index 1; what a placed
				// turret is set to is the number that matters.
				piece.range_setting = turret->RangeSettingField();
				piece.ammo = turret->NumBulletsField();
			}

			kept_to_piece[k] = static_cast<int32_t>(out.pieces.size());
			out.pieces.push_back(piece);
		}

		// Second pass for the support graph. Measured on a hand-built base, ARK
		// keeps this in LinkedStructures - a bidirectional graph - and leaves
		// PlacedOnFloorStructure empty. Links must be resolved only once every
		// piece has an index, and links leaving the selection are dropped rather
		// than left dangling.
		for (size_t k = 0; k < kept.size(); ++k)
		{
			const int32_t self = kept_to_piece[k];
			if (self < 0) continue;

			// Floor-mounted items (turrets, generators) carry no links at all -
			// their relationship is this pointer instead.
			if (APrimalStructure* floor = kept[k]->PlacedOnFloorStructureField())
			{
				const auto fit = index_of.find(floor);
				if (fit != index_of.end())
				{
					const int32_t target = kept_to_piece[fit->second];
					if (target >= 0 && target != self)
					{
						out.pieces[self].floor = target;
						++stats.floors_captured;
					}
				}
			}

			const TArray<APrimalStructure*> linked = kept[k]->LinkedStructuresField();
			for (int i = 0; i < linked.Num(); ++i)
			{
				APrimalStructure* other = linked[i];
				if (other == nullptr) continue;

				const auto found = index_of.find(other);
				if (found == index_of.end()) { ++stats.links_outside; continue; }

				const int32_t target = kept_to_piece[found->second];
				if (target < 0 || target == self) continue;

				out.pieces[self].links.push_back(target);
				++stats.links_captured;
			}
		}

		stats.captured = static_cast<int>(out.pieces.size());

		if (out.pieces.empty()) { error = "found structures but none had a resolvable blueprint path"; return false; }
		return true;
	}
} // namespace sb
