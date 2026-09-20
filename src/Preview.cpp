#include "Preview.h"
#include "ArkUtils.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sb
{
	namespace
	{
		struct Pending
		{
			bool active = false;
			Blueprint bp;
			PasteOptions opts;
			std::vector<AActor*> markers;
		};

		std::unordered_map<uint64, Pending> g_pending;

		// Marker actors are spawned WITHOUT finalization on purpose: they are
		// scaffolding, so they should be unowned, indestructible and immune to
		// decay for the short time they exist. ClearPreview removes them.
		UClass* ResolveMarkerClass(const Blueprint& bp)
		{
			static const char* candidates[] = {
				"Blueprint'/Game/PrimalEarth/Structures/StandingTorch.StandingTorch'",
				"Blueprint'/Game/PrimalEarth/Structures/Wooden/Wood_Pillar.Wood_Pillar'",
			};

			for (const char* path : candidates)
			{
				FString p(path);
				if (UClass* c = UVictoryCore::BPLoadClass(&p)) return c;
			}

			// Fall back to something the save itself uses, which is guaranteed to
			// exist on this map because the paste would otherwise fail anyway.
			for (const std::string& path : bp.classes)
			{
				FString p(path.c_str());
				if (UClass* c = UVictoryCore::BPLoadClass(&p)) return c;
			}
			return nullptr;
		}

		bool InBox(const FVector& p, const FVector& lo, const FVector& hi)
		{
			return p.X >= lo.X && p.X <= hi.X
			    && p.Y >= lo.Y && p.Y <= hi.Y
			    && p.Z >= lo.Z && p.Z <= hi.Z;
		}
	} // namespace

	bool ScanTerrain(const FVector& origin, float span_x, float span_y, TerrainMap& out)
	{
		out = TerrainMap{};
		out.span_x = span_x;
		out.span_y = span_y;

		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (world == nullptr) return false;

		FVector offset_up{ 0.f, 0.f, 3000.f };
		FVector offset_down{ 0.f, 0.f, -20000.f };

		float raw[TerrainMap::kGrid][TerrainMap::kGrid]{};
		float sum = 0.f;
		float lo = 1e9f, hi = -1e9f;

		for (int gx = 0; gx < TerrainMap::kGrid; ++gx)
		{
			for (int gy = 0; gy < TerrainMap::kGrid; ++gy)
			{
				const float fx = span_x * (static_cast<float>(gx) / (TerrainMap::kGrid - 1));
				const float fy = span_y * (static_cast<float>(gy) / (TerrainMap::kGrid - 1));

				FVector start{ origin.X + fx, origin.Y + fy, origin.Z };
				FVector ground{};
				if (!UVictoryCore::GetGroundLocation(world, &ground, &start, &offset_up, &offset_down))
					continue;

				raw[gx][gy] = ground.Z;
				out.hit[gx][gy] = true;
				sum += ground.Z;
				lo = std::min(lo, ground.Z);
				hi = std::max(hi, ground.Z);
				++out.samples;
			}
		}

		if (out.samples == 0) return false;

		out.average = sum / out.samples;
		out.lo = lo - out.average;
		out.hi = hi - out.average;

		for (int gx = 0; gx < TerrainMap::kGrid; ++gx)
			for (int gy = 0; gy < TerrainMap::kGrid; ++gy)
				if (out.hit[gx][gy]) out.rel[gx][gy] = raw[gx][gy] - out.average;

		return true;
	}

	bool HasPreview(uint64 steam_id)
	{
		auto it = g_pending.find(steam_id);
		return it != g_pending.end() && it->second.active;
	}

	void ClearPreview(uint64 steam_id)
	{
		auto it = g_pending.find(steam_id);
		if (it == g_pending.end()) return;

		// Re-query before destroying: a marker could already be gone, and a stale
		// pointer would be a crash rather than a tidy-up.
		if (!it->second.markers.empty())
		{
			UWorld* world = ArkApi::GetApiUtils().GetWorld();
			if (world != nullptr)
			{
				std::unordered_set<AActor*> live;
				const FVector o = it->second.opts.origin;
				TArray<AActor*> nearby = ArkApi::GetApiUtils()
					.GetAllActorsInRange(o, 100000.f, EServerOctreeGroup::STRUCTURES);
				for (int i = 0; i < nearby.Num(); ++i)
					if (nearby[i] != nullptr) live.insert(nearby[i]);

				for (AActor* m : it->second.markers)
					if (live.find(m) != live.end()) m->Destroy(true, true);
			}
		}
		g_pending.erase(it);
	}

	bool BeginPreview(uint64 steam_id, const Blueprint& bp, const PasteOptions& opts,
	                  PreviewInfo& out, std::string& error)
	{
		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (world == nullptr) { error = "no world"; return false; }
		if (bp.pieces.empty()) { error = "blueprint is empty"; return false; }

		ClearPreview(steam_id); // replace any previous outline

		out = PreviewInfo{};
		out.pieces = bp.pieces.size();
		out.origin = opts.origin;
		out.yaw = opts.yaw;
		out.base_z = opts.origin.Z;

		// Footprint corners in local space, rotated into the world.
		const FVector local[4] = {
			FVector{ 0.f,       0.f,       0.f },
			FVector{ bp.size_x, 0.f,       0.f },
			FVector{ bp.size_x, bp.size_y, 0.f },
			FVector{ 0.f,       bp.size_y, 0.f },
		};

		FVector corners[4];
		FVector lo{ 1e9f, 1e9f, opts.origin.Z };
		FVector hi{ -1e9f, -1e9f, opts.origin.Z + bp.size_z };

		for (int i = 0; i < 4; ++i)
		{
			const FVector r = RotateAroundZ(local[i], opts.yaw);
			corners[i] = FVector{ opts.origin.X + r.X, opts.origin.Y + r.Y, opts.origin.Z };
			lo.X = std::min(lo.X, corners[i].X); hi.X = std::max(hi.X, corners[i].X);
			lo.Y = std::min(lo.Y, corners[i].Y); hi.Y = std::max(hi.Y, corners[i].Y);
		}

		out.span_x = hi.X - lo.X;
		out.span_y = hi.Y - lo.Y;
		out.span_z = bp.size_z;

		// --- what is already standing here ---------------------------------
		const FVector center{ (lo.X + hi.X) * 0.5f, (lo.Y + hi.Y) * 0.5f, (lo.Z + hi.Z) * 0.5f };
		const float dx = hi.X - lo.X, dy = hi.Y - lo.Y, dz = hi.Z - lo.Z;
		const float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);

		TArray<AActor*> existing = ArkApi::GetApiUtils()
			.GetAllActorsInRange(center, radius, EServerOctreeGroup::STRUCTURES);

		for (int i = 0; i < existing.Num(); ++i)
		{
			AActor* a = existing[i];
			if (a == nullptr) continue;
			FVector p; FRotator r;
			if (!ReadTransform(a, p, r)) continue;
			if (InBox(p, lo, hi)) ++out.structures_in_way;
		}

		// --- how flat is the ground ----------------------------------------
		// A 5x5 grid over the footprint is enough to catch a slope or a dip
		// without turning the preview into a long synchronous trace loop.
		constexpr int kGrid = 5;
		float min_z = 1e9f, max_z = -1e9f;
		int hits = 0;

		FVector offset_up{ 0.f, 0.f, 3000.f };
		FVector offset_down{ 0.f, 0.f, -20000.f };

		for (int gx = 0; gx < kGrid; ++gx)
		{
			for (int gy = 0; gy < kGrid; ++gy)
			{
				const float fx = bp.size_x * (static_cast<float>(gx) / (kGrid - 1));
				const float fy = bp.size_y * (static_cast<float>(gy) / (kGrid - 1));
				const FVector r = RotateAroundZ(FVector{ fx, fy, 0.f }, opts.yaw);

				FVector start{ opts.origin.X + r.X, opts.origin.Y + r.Y, opts.origin.Z };
				FVector ground{};

				if (UVictoryCore::GetGroundLocation(world, &ground, &start, &offset_up, &offset_down))
				{
					min_z = std::min(min_z, ground.Z);
					max_z = std::max(max_z, ground.Z);
					++hits;
				}
			}
		}

		if (hits > 0)
		{
			out.ground_sampled = true;
			out.ground_samples = hits;
			out.ground_min_z = min_z;
			out.ground_max_z = max_z;
		}

		// --- per-piece burial check ----------------------------------------
		// A tower's pieces stack in columns, so cache the ground per X/Y: 2500
		// pieces usually resolve to a few hundred traces.
		{
			std::unordered_map<long long, float> column;
			auto key = [](float x, float y) {
				return (static_cast<long long>(std::lround(x)) << 21)
				     ^ static_cast<long long>(std::lround(y));
			};

			for (const Piece& piece : bp.pieces)
			{
				const FVector r = RotateAroundZ(FVector{ piece.x, piece.y, piece.z }, opts.yaw);
				const float wx = opts.origin.X + r.X;
				const float wy = opts.origin.Y + r.Y;
				const float wz = opts.origin.Z + r.Z;

				const long long k = key(wx, wy);
				auto found = column.find(k);
				if (found == column.end())
				{
					FVector start{ wx, wy, opts.origin.Z };
					FVector ground{};
					if (!UVictoryCore::GetGroundLocation(world, &ground, &start, &offset_up, &offset_down))
						continue;
					found = column.emplace(k, ground.Z).first;
					++out.ground_columns;
				}

				const float depth = found->second - wz;
				if (depth > 0.f)
				{
					++out.pieces_buried;
					out.worst_burial = std::max(out.worst_burial, depth);
				}
			}
		}

		// --- enemy build proximity ------------------------------------------
		{
			const float reach = std::max(out.span_x, out.span_y) * 0.5f + 10000.f;
			TArray<AActor*> nearby = ArkApi::GetApiUtils()
				.GetAllActorsInRange(center, reach, EServerOctreeGroup::STRUCTURES);

			std::unordered_set<int> teams;
			float nearest = 1e9f;

			for (int i = 0; i < nearby.Num(); ++i)
			{
				AActor* a = nearby[i];
				if (a == nullptr) continue;

				const int team = a->TargetingTeamField();
				// Team 0 is unowned - our own markers and world structures.
				if (team == 0 || team == opts.team) continue;

				FVector p; FRotator r;
				if (!ReadTransform(a, p, r)) continue;

				++out.enemy_structures;
				teams.insert(team);
				nearest = std::min(nearest, FVector::Distance(p, center));
			}

			out.enemy_teams = static_cast<int>(teams.size());
			if (out.enemy_structures > 0) out.nearest_enemy = nearest;
		}

		// --- drop the corner markers ---------------------------------------
		Pending pending;
		pending.active = true;
		pending.bp = bp;
		pending.opts = opts;

		if (UClass* marker_cls = ResolveMarkerClass(bp))
		{
			for (int i = 0; i < 4; ++i)
			{
				FVector at = corners[i];
				FVector ground{};
				if (UVictoryCore::GetGroundLocation(world, &ground, &at, &offset_up, &offset_down))
					at.Z = ground.Z;

				FRotator rot{ 0.f, opts.yaw, 0.f };
				if (AActor* m = UVictoryCore::SpawnActorInWorld(
					world, TSubclassOf<AActor>(marker_cls), at, rot,
					nullptr, 0, FName(), nullptr, nullptr))
				{
					pending.markers.push_back(m);
				}
			}
		}

		g_pending[steam_id] = std::move(pending);
		return true;
	}

	bool ConfirmPreview(uint64 steam_id, Blueprint& bp_out, PasteOptions& opts_out, std::string& error)
	{
		auto it = g_pending.find(steam_id);
		if (it == g_pending.end() || !it->second.active)
		{
			error = "nothing previewed - run /sb preview <name> first";
			return false;
		}

		bp_out = it->second.bp;
		opts_out = it->second.opts;

		ClearPreview(steam_id); // markers must go before the real pieces arrive
		return true;
	}
} // namespace sb
