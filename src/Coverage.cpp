#include "Coverage.h"
#include "ArkUtils.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace sb
{
	namespace
	{
		struct Emplacement
		{
			FVector at{};
			float range = 0.f;   // engagement radius, from the turret itself
		};
	} // namespace

	bool ScanCoverage(const FVector& center, float span, float fixed_z, CoverageMap& out)
	{
		out = CoverageMap{};
		out.span = span;
		out.sample_z = fixed_z;
		out.total_cells = CoverageMap::kGrid * CoverageMap::kGrid;

		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (world == nullptr || span <= 0.f) return false;

		// Reach far enough that a turret outside the sampled square can still
		// cover part of it. 6000 is the Heavy/Tek maximum; rocket turrets reach
		// 12000, so allow for the longest.
		constexpr float kLongestTurretRange = 12000.f;
		const float search = span * 0.7071f + kLongestTurretRange;

		std::unordered_set<AActor*> seen;
		for (EServerOctreeGroup::Type group : { EServerOctreeGroup::STRUCTURES,
		                                        EServerOctreeGroup::STRUCTURES_CORE })
		{
			TArray<AActor*> found = ArkApi::GetApiUtils().GetAllActorsInRange(center, search, group);
			for (int i = 0; i < found.Num(); ++i)
				if (found[i] != nullptr) seen.insert(found[i]);
		}

		std::vector<Emplacement> turrets;
		for (AActor* actor : seen)
		{
			if (!IsA(actor, TurretClass())) continue;

			FVector p; FRotator r;
			if (!ReadTransform(actor, p, r)) continue;

			auto* turret = static_cast<APrimalStructureTurret*>(actor);
			float range = 0.f;
			if (float* ranges = turret->TargetingRangesField()())
				range = std::max({ ranges[0], ranges[1], ranges[2] });
			if (range <= 0.f) continue;

			turrets.push_back(Emplacement{ p, range });
		}

		out.turrets_considered = static_cast<int>(turrets.size());
		if (turrets.empty()) return false;

		FVector offset_up{ 0.f, 0.f, 3000.f };
		FVector offset_down{ 0.f, 0.f, -20000.f };
		const float half = span * 0.5f;

		for (int gx = 0; gx < CoverageMap::kGrid; ++gx)
		{
			for (int gy = 0; gy < CoverageMap::kGrid; ++gy)
			{
				const float fx = -half + span * (static_cast<float>(gx) / (CoverageMap::kGrid - 1));
				const float fy = -half + span * (static_cast<float>(gy) / (CoverageMap::kGrid - 1));

				FVector at{ center.X + fx, center.Y + fy, center.Z };

				if (fixed_z != 0.f)
				{
					at.Z = fixed_z;
				}
				else
				{
					FVector start = at;
					FVector ground{};
					if (!UVictoryCore::GetGroundLocation(world, &ground, &start, &offset_up, &offset_down))
						continue;
					at.Z = ground.Z;
				}

				out.hit[gx][gy] = true;

				int n = 0;
				for (const Emplacement& t : turrets)
				{
					const float dx = t.at.X - at.X;
					const float dy = t.at.Y - at.Y;
					const float dz = t.at.Z - at.Z;
					if (dx * dx + dy * dy + dz * dz <= t.range * t.range) ++n;
				}

				out.count[gx][gy] = n;
				out.max_count = std::max(out.max_count, n);
				if (n > 0) ++out.covered_cells;
			}
		}

		return true;
	}
} // namespace sb
