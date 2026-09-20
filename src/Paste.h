#pragma once

#include "Model.h"
#include <API/ARK/Ark.h>
#include <string>
#include <vector>

namespace sb
{
	struct PasteOptions
	{
		FVector origin{};        // world location the build's origin corner maps to
		float yaw = 0.f;         // rotation about the origin, degrees
		int team = 0;            // targeting team the pasted base belongs to
		int owner_id = 0;        // owning player id
		FString owner_name{ "" };
		bool invulnerable = false; // skip finalization: indestructible display build
		bool snap_to_ground = true; // sit the lowest piece on the terrain
		// Which sampled ground height to sit on. Average by default, because it
		// is what builders actually do by hand: split the difference so the whole
		// build hugs the terrain, accepting a little sinking on the high side and
		// a little overhang on the low side. Sitting on the highest point avoids
		// burying anything but lifts the entire structure off the ground.
		enum class Ground { Average, Highest, Lowest };
		Ground ground_mode = Ground::Average;

		// A uniform downward offset, so the base buries rather than resting on
		// the surface. Deliberately uniform: dropping each column to its own
		// ground height would give uneven foundations and a new snap surface per
		// column, and bases are designed against one primary snap surface.
		float sink = 0.f;
		int per_tick = 25;         // placement budget per frame
	};

	// Drops the build so its lowest piece rests on the ground beneath the
	// origin. Without this the origin is simply wherever the admin was standing,
	// which for a flying admin means the whole base is pasted in mid air.
	// Call once, before BeginPreview or BeginPaste - never both, or it stacks.
	void AdjustOriginToGround(const Blueprint& bp, PasteOptions& opts);

	// Ground spread found by the last AdjustOriginToGround call, for reporting.
	float LastGroundSpread();

	// Queues a paste. Only one runs at a time; returns false if busy or if the
	// blueprint's classes cannot be resolved on this map.
	bool BeginPaste(const Blueprint& bp, const PasteOptions& opts,
	                uint64 requester_steam_id, std::string& error);

	bool PasteInProgress();
	void CancelPaste();

	// Destroys the most recent paste. Re-queries the world rather than trusting
	// stored pointers, so pieces destroyed in the meantime cannot dangle.
	bool UndoLastPaste(int& destroyed, std::string& error);

	// Driven from the ArkApi tick callback.
	void TickPaste(float delta_seconds);
} // namespace sb
