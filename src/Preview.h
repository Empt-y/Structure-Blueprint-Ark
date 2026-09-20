#pragma once

#include "Model.h"
#include "Paste.h"
#include <API/ARK/Ark.h>
#include <string>

namespace sb
{
	// What a dry run found. Reported to the admin before anything is committed.
	struct PreviewInfo
	{
		size_t pieces = 0;
		FVector origin{};
		float yaw = 0.f;

		// Footprint extent after rotation, in world axes.
		float span_x = 0.f, span_y = 0.f, span_z = 0.f;

		int structures_in_way = 0;

		bool ground_sampled = false;
		int ground_samples = 0;
		float ground_min_z = 0.f;
		float ground_max_z = 0.f;
		float base_z = 0.f;      // where the build's floor will sit

		// Per-piece validation. The footprint average says a site is "340 units
		// uneven"; this says which pieces actually end up inside the landscape.
		int pieces_buried = 0;
		float worst_burial = 0.f;
		int ground_columns = 0;   // distinct X/Y traced (pieces stack, so far fewer)

		// Enemy build proximity. Anything owned by another team near the site.
		int enemy_structures = 0;
		int enemy_teams = 0;
		float nearest_enemy = 0.f;
	};

	// Loads nothing itself: the caller supplies the blueprint and the intended
	// options. Spawns corner markers and stashes the transform, so a later
	// Confirm pastes at exactly the previewed position rather than wherever the
	// admin happens to be standing by then.
	bool BeginPreview(uint64 steam_id, const Blueprint& bp, const PasteOptions& opts,
	                  PreviewInfo& out, std::string& error);

	// Hands back what was previewed so the caller can start the paste.
	bool ConfirmPreview(uint64 steam_id, Blueprint& bp_out, PasteOptions& opts_out, std::string& error);

	// A readable elevation map of the ground a build would sit on. Sampling is
	// the only way to see terrain from the server, and seeing it is what
	// actually decides whether a rigid build will sit well - deforming the build
	// to the ground is not an option, because varying foundation heights breaks
	// every course above them.
	struct TerrainMap
	{
		static constexpr int kGrid = 11;
		float rel[kGrid][kGrid]{};   // height relative to the average
		bool hit[kGrid][kGrid]{};
		float average = 0.f;
		float lo = 0.f, hi = 0.f;
		int samples = 0;
		float span_x = 0.f, span_y = 0.f;
	};

	bool ScanTerrain(const FVector& origin, float span_x, float span_y, TerrainMap& out);

	// Destroys the marker actors. Safe to call when nothing is pending.
	void ClearPreview(uint64 steam_id);
	bool HasPreview(uint64 steam_id);
} // namespace sb
