#pragma once

#include "Model.h"
#include <API/ARK/Ark.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace sb
{
	// Two-corner selection, tracked per admin so several can work at once.
	struct Selection
	{
		bool has_a = false, has_b = false;
		FVector a{}, b{};

		bool Complete() const { return has_a && has_b; }
		FVector Min() const;
		FVector Max() const;
	};

	Selection& SelectionFor(uint64 steam_id);
	void ClearSelection(uint64 steam_id);

	// Where pieces go missing. Capture used to drop unusable actors silently,
	// which made an under-capture impossible to diagnose from in game.
	struct CaptureStats
	{
		int octree_returned = 0;   // actors the octree handed back (deduped)
		int in_box = 0;            // ...of those, inside the selection
		int no_transform = 0;      // skipped: no readable root component
		int no_path = 0;           // skipped: blueprint path would not resolve
		int near_miss = 0;         // just OUTSIDE the box - selection likely clipping
		int links_captured = 0;    // support-graph edges recorded
		int floors_captured = 0;   // floor-mount relationships recorded
		int links_outside = 0;     // edges pointing out of the selection, dropped
		int containers = 0;        // structures with an inventory
		int item_stacks = 0;       // stacks recorded across them
		int turrets = 0;           // turrets whose ranges were recorded
		int captured = 0;          // actually written to the save
	};

	// One row per octree group, so we can see empirically which group a given
	// structure type actually lives in rather than assuming.
	struct ScanRow
	{
		const char* name = "";
		int total = 0;
		int in_box = 0;
	};

	// Structures inside the selection, deduped across the octree groups capture
	// uses. Shared so other commands do not reimplement the query.
	std::vector<AActor*> CollectSelection(const Selection& sel);

	std::vector<ScanRow> ScanGroups(const Selection& sel);

	// Ground truth. The octree can omit actors - stasised ones especially - so
	// this walks the persistent level's own actor list instead and reports what
	// is inside the box but absent from the octree groups capture relies on.
	struct MissedRow { std::string cls; int count = 0; };
	std::vector<MissedRow> ScanMissed(const Selection& sel, int& level_total, int& level_in_box);

	// ARK propagates structural collapse over its link graph. We captured none of
	// it, which is why a pasted base survives having its supports destroyed.
	// Measure which fields are actually populated before trusting any of them.
	struct LinkStats
	{
		int structures = 0;
		int with_placed_on_floor = 0;   // PlacedOnFloorStructure != null
		int with_linked = 0;            // LinkedStructures non-empty
		int linked_refs = 0;            // total entries across LinkedStructures
		int with_on_floor_list = 0;     // StructuresPlacedOnFloor non-empty
		int on_floor_refs = 0;
		int with_snapped_child = 0;     // PrimarySnappedStructureChild != null
	};

	LinkStats InspectLinks(const Selection& sel);

	// Walks the structures inside the selection and builds a Blueprint.
	// Returns false and fills `error` if the selection is empty or unusable.
	// `margin` grows the selection box on every axis before testing. InBox tests
	// a structure's origin, not its footprint, so a foundation you are standing
	// on to set a corner usually has its origin just outside the box. Half a
	// foundation is the sane default.
	constexpr float kDefaultMargin = 150.f;

	bool CaptureSelection(const Selection& sel, const std::string& name,
	                      uint64 by_steam_id, float margin, bool with_items,
	                      Blueprint& out, CaptureStats& stats, std::string& error);
} // namespace sb
