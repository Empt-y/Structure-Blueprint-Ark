#include "Paste.h"
#include "ArkUtils.h"
#include "Gzip.h"
#include "Storage.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <json.hpp>

namespace sb
{
	namespace
	{
		// Placement and linking are separate phases. A piece can only be linked
		// once its neighbours exist, and the graph is bidirectional, so there is
		// no placement order that would let us link as we go.
		enum class Phase { Placing, Linking };

		struct PasteJob
		{
			Blueprint bp;
			PasteOptions opts;
			uint64 requester = 0;

			std::vector<UClass*> resolved;        // parallel to bp.classes
			std::vector<UClass*> item_resolved;   // parallel to bp.item_classes
			std::vector<int32_t> order;      // placement order, bottom-up
			std::vector<AActor*> spawned;    // parallel to bp.pieces
			Phase phase = Phase::Placing;
			size_t cursor = 0;
			size_t link_cursor = 0;   // piece being linked
			size_t link_sub = 0;      // edge within that piece
			int failed = 0;
			int links_made = 0;
			int floors_restored = 0;
			int stacks_restored = 0;
			int devices_restored = 0;
		};

		// Undo identifies pasted pieces by blueprint path and world position
		// rather than by pointer. Pointers die with a plugin reload or a restart,
		// which made undo useless in exactly the situation it mattered most:
		// a large paste that went wrong.
		struct UndoItem { int32_t cls = 0; float x = 0.f, y = 0.f, z = 0.f; };

		struct PasteRecord
		{
			FVector lo{}, hi{};
			std::vector<std::string> classes;
			std::vector<UndoItem> items;
			bool valid = false;
		};

		std::string UndoPath() { return storage::PluginDir() + "/undo.gz"; }

		void PersistUndo(const PasteRecord& rec)
		{
			try
			{
				nlohmann::json items = nlohmann::json::array();
				for (const UndoItem& it : rec.items)
					items.push_back(nlohmann::json{ {"c", it.cls}, {"p", {it.x, it.y, it.z}} });

				const nlohmann::json j{
					{"lo", {rec.lo.X, rec.lo.Y, rec.lo.Z}},
					{"hi", {rec.hi.X, rec.hi.Y, rec.hi.Z}},
					{"classes", rec.classes},
					{"items", items},
				};

				std::string packed;
				if (!gz::Compress(j.dump(), packed)) return;

				std::ofstream out(UndoPath(), std::ios::binary | std::ios::trunc);
				if (out) out.write(packed.data(), static_cast<std::streamsize>(packed.size()));
			}
			catch (const std::exception&) { /* undo is best effort */ }
		}

		bool LoadUndo(PasteRecord& rec)
		{
			try
			{
				std::ifstream in(UndoPath(), std::ios::binary);
				if (!in) return false;

				const std::string packed((std::istreambuf_iterator<char>(in)),
				                          std::istreambuf_iterator<char>());
				std::string raw;
				if (!gz::Decompress(packed, raw)) return false;

				const nlohmann::json j = nlohmann::json::parse(raw);
				rec = PasteRecord{};
				rec.lo = FVector{ j["lo"][0], j["lo"][1], j["lo"][2] };
				rec.hi = FVector{ j["hi"][0], j["hi"][1], j["hi"][2] };
				rec.classes = j["classes"].get<std::vector<std::string>>();
				for (const auto& e : j["items"])
				{
					UndoItem it;
					it.cls = e["c"].get<int32_t>();
					it.x = e["p"][0]; it.y = e["p"][1]; it.z = e["p"][2];
					rec.items.push_back(it);
				}
				rec.valid = !rec.items.empty();
				return rec.valid;
			}
			catch (const std::exception&) { return false; }
		}

		std::optional<PasteJob> g_job;
		PasteRecord g_last;
	} // namespace

	namespace { float g_ground_spread = 0.f; }

	float LastGroundSpread() { return g_ground_spread; }

	void AdjustOriginToGround(const Blueprint& bp, PasteOptions& opts)
	{
		g_ground_spread = 0.f;
		if (!opts.snap_to_ground || bp.pieces.empty()) return;

		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (world == nullptr) return;

		// Pieces are stored relative to the selection's minimum corner, which is
		// wherever the admin happened to put it - not necessarily the base of the
		// build. So find the actual lowest piece rather than assuming z == 0.
		float min_z = bp.pieces[0].z;
		for (const Piece& piece : bp.pieces) min_z = std::min(min_z, piece.z);

		// Sample the whole footprint, not just the origin corner. Probing one
		// point means terrain anywhere else is ignored, and half a large build
		// can end up inside a hillside.
		constexpr int kGrid = 7;
		FVector offset_up{ 0.f, 0.f, 3000.f };
		FVector offset_down{ 0.f, 0.f, -20000.f };

		float lo = 1e9f, hi = -1e9f, sum = 0.f;
		int hits = 0;

		for (int gx = 0; gx < kGrid; ++gx)
		{
			for (int gy = 0; gy < kGrid; ++gy)
			{
				const float fx = bp.size_x * (static_cast<float>(gx) / (kGrid - 1));
				const float fy = bp.size_y * (static_cast<float>(gy) / (kGrid - 1));
				const FVector r = RotateAroundZ(FVector{ fx, fy, 0.f }, opts.yaw);

				FVector start{ opts.origin.X + r.X, opts.origin.Y + r.Y, opts.origin.Z };
				FVector ground{};
				if (!UVictoryCore::GetGroundLocation(world, &ground, &start, &offset_up, &offset_down))
					continue;

				lo = std::min(lo, ground.Z);
				hi = std::max(hi, ground.Z);
				sum += ground.Z;
				++hits;
			}
		}

		if (hits == 0) return;
		g_ground_spread = hi - lo;

		float target = sum / hits;
		if (opts.ground_mode == PasteOptions::Ground::Highest) target = hi;
		else if (opts.ground_mode == PasteOptions::Ground::Lowest) target = lo;

		opts.origin.Z = target - min_z;
	}

	bool PasteInProgress() { return g_job.has_value(); }
	void CancelPaste() { g_job.reset(); }

	bool BeginPaste(const Blueprint& bp, const PasteOptions& opts,
	                uint64 requester_steam_id, std::string& error)
	{
		if (g_job.has_value()) { error = "a paste is already running"; return false; }
		if (bp.pieces.empty()) { error = "blueprint is empty"; return false; }

		PasteJob job;
		job.bp = bp;
		job.opts = opts;
		job.requester = requester_steam_id;

		// Resolve every class up front. Failing here is much friendlier than
		// discovering a missing mod halfway through placing 100k pieces.
		job.resolved.reserve(bp.classes.size());
		int missing = 0;
		for (const std::string& path : bp.classes)
		{
			FString fpath(path.c_str());
			UClass* cls = UVictoryCore::BPLoadClass(&fpath);
			if (cls == nullptr) ++missing;
			job.resolved.push_back(cls);
		}
		// Item classes too. A missing one costs that stack, not the paste.
		job.item_resolved.reserve(bp.item_classes.size());
		for (const std::string& path : bp.item_classes)
		{
			FString fpath(path.c_str());
			job.item_resolved.push_back(UVictoryCore::BPLoadClass(&fpath));
		}

		if (missing == static_cast<int>(bp.classes.size()))
		{
			error = "none of this build's classes exist on this map (missing mods?)";
			return false;
		}

		// Bottom-up. Structural correctness now comes from the link pass, but
		// building upward still looks right while it streams in and keeps pieces
		// resting on ground that already exists.
		job.order.resize(bp.pieces.size());
		for (int32_t i = 0; i < static_cast<int32_t>(bp.pieces.size()); ++i) job.order[i] = i;
		std::sort(job.order.begin(), job.order.end(), [&](int32_t a, int32_t b) {
			return bp.pieces[a].z < bp.pieces[b].z;
		});

		job.spawned.assign(bp.pieces.size(), nullptr);
		g_job = std::move(job);

		g_last = PasteRecord{};
		return true;
	}

	namespace
	{
		void FinishJob(PasteJob& job)
		{
			g_last = PasteRecord{};
			g_last.valid = true;
			g_last.classes = job.bp.classes;

			const float sx = job.bp.size_x, sy = job.bp.size_y, sz = job.bp.size_z;
			const float pad = 200.f; // rotation can push pieces slightly past the raw extent
			const float reach = std::sqrt(sx * sx + sy * sy) + pad;
			g_last.lo = FVector{ job.opts.origin.X - reach, job.opts.origin.Y - reach, job.opts.origin.Z - pad };
			g_last.hi = FVector{ job.opts.origin.X + reach, job.opts.origin.Y + reach, job.opts.origin.Z + sz + pad };

			// Record where each piece actually landed, recomputed with the same
			// transform placement used, so undo can find them again after a
			// reload when every pointer is long gone.
			for (size_t i = 0; i < job.spawned.size(); ++i)
			{
				if (job.spawned[i] == nullptr) continue;
				const Piece& piece = job.bp.pieces[i];
				const FVector rotated = RotateAroundZ(FVector{ piece.x, piece.y, piece.z }, job.opts.yaw);

				UndoItem it;
				it.cls = piece.class_index;
				it.x = job.opts.origin.X + rotated.X;
				it.y = job.opts.origin.Y + rotated.Y;
				it.z = job.opts.origin.Z + rotated.Z;
				g_last.items.push_back(it);
			}
			PersistUndo(g_last);

			AShooterPlayerController* pc = ArkApi::GetApiUtils().FindPlayerFromSteamId(job.requester);
			if (pc != nullptr)
			{
				Reply(pc, L"Paste complete: " + std::to_wstring(g_last.items.size())
					+ L" placed, " + std::to_wstring(job.failed) + L" failed, "
					+ std::to_wstring(job.links_made) + L" support links, "
					+ std::to_wstring(job.floors_restored) + L" floor mounts, "
					+ std::to_wstring(job.stacks_restored) + L" item stacks, "
					+ std::to_wstring(job.devices_restored) + L" device states restored.");
			}

			g_job.reset();
		}
	} // namespace

	void TickPaste(float /*delta_seconds*/)
	{
		if (!g_job.has_value()) return;
		PasteJob& job = *g_job;

		UWorld* world = ArkApi::GetApiUtils().GetWorld();
		if (world == nullptr) { g_job.reset(); return; }

		const int budget = job.opts.per_tick > 0 ? job.opts.per_tick : 25;

		if (job.phase == Phase::Placing)
		{
			for (int placed = 0; placed < budget && job.cursor < job.order.size(); ++placed)
			{
				const int32_t idx = job.order[job.cursor++];
				const Piece& piece = job.bp.pieces[idx];

				UClass* cls = job.resolved[piece.class_index];
				if (cls == nullptr) { ++job.failed; continue; }

				const FVector local{ piece.x, piece.y, piece.z };
				const FVector rotated = RotateAroundZ(local, job.opts.yaw);
				FVector world_loc{
					job.opts.origin.X + rotated.X,
					job.opts.origin.Y + rotated.Y,
					job.opts.origin.Z + rotated.Z
				};

				world_loc.Z -= job.opts.sink;
				FRotator world_rot{ piece.pitch, piece.yaw + job.opts.yaw, piece.roll };

				AActor* actor = UVictoryCore::SpawnActorInWorld(
					world, TSubclassOf<AActor>(cls), world_loc, world_rot,
					nullptr, 0, FName(), nullptr, nullptr);

				if (actor == nullptr) { ++job.failed; continue; }

				auto* structure = static_cast<APrimalStructure*>(actor);
				job.spawned[idx] = actor;

				if (!job.opts.invulnerable)
				{
					// The step that turns a bare spawned actor into a real
					// structure. Without it the piece keeps team 0 and no owner,
					// which makes it undamageable, undemolishable and exempt from
					// decay.
					//
					// ForcePrimaryParent is the floor a mounted item sits on. It is
					// null for ordinary structural pieces, whose relationships are
					// rebuilt in the link phase instead. Placement is sorted
					// bottom-up so the floor already exists by the time its
					// mounted item is placed.
					APrimalStructure* floor = nullptr;
					if (piece.floor >= 0 && piece.floor < static_cast<int32_t>(job.spawned.size()))
						floor = static_cast<APrimalStructure*>(job.spawned[piece.floor]);

					FString owner_name = job.opts.owner_name;
					structure->NonPlayerFinalStructurePlacement(
						job.opts.team, job.opts.owner_id, &owner_name, floor);

					if (floor != nullptr) ++job.floors_restored;

					// --- container contents and device state ---------------
					if (!piece.items.empty() && IsA(actor, ContainerClass()))
					{
						auto* container = static_cast<APrimalStructureItemContainer*>(actor);
						if (UPrimalInventoryComponent* inv = container->MyInventoryComponentField())
						{
							for (const ItemEntry& entry : piece.items)
							{
								if (entry.cls < 0 || entry.cls >= static_cast<int32_t>(job.item_resolved.size()))
									continue;
								UClass* icls = job.item_resolved[entry.cls];
								if (icls == nullptr) continue;

								UPrimalItem* made = UPrimalItem::AddNewItem(
									icls, inv, false, false, 0.0f,
									!entry.blueprint, entry.quantity, entry.blueprint,
									0.0f, false, TSubclassOf<UPrimalItem>(), 0.0f, false, true);
								if (made == nullptr) continue;

								// AddNewItem creates a default-quality item; the
								// captured state is written back over it. Quality
								// lives in the stat array, not a single field.
								made->ItemQuantityField() = entry.quantity;
								made->ItemDurabilityField() = entry.durability;
								made->CraftedSkillBonusField() = entry.skill_bonus;

								if (entry.stats.size() == 8)
									if (unsigned short* sv = made->ItemStatValuesField()())
										for (int k = 0; k < 8; ++k)
											sv[k] = entry.stats[static_cast<size_t>(k)];

								if (!entry.name.empty())
									made->CustomItemNameField() = FString(entry.name.c_str());

								++job.stacks_restored;
							}
						}
					}

					if (piece.activated >= 0 && IsA(actor, ContainerClass()))
					{
						static_cast<APrimalStructureItemContainer*>(actor)
							->bContainerActivated() = (piece.activated == 1);
						++job.devices_restored;
					}

					if (piece.ranges.size() == 3 && IsA(actor, TurretClass()))
					{
						auto* turret = static_cast<APrimalStructureTurret*>(actor);
						if (float* r = turret->TargetingRangesField()())
							for (int k = 0; k < 3; ++k) r[k] = piece.ranges[static_cast<size_t>(k)];
					}
				}
			}

			if (job.cursor >= job.order.size())
			{
				// Invulnerable builds are scaffolding, not real bases - they have
				// no owner and take no damage, so a support graph is meaningless.
				if (job.opts.invulnerable) { FinishJob(job); return; }
				job.phase = Phase::Linking;
			}
			return;
		}

		// --- link phase --------------------------------------------------
		// Rebuild APrimalStructure::LinkedStructures so ARK's own ReprocessTree
		// can collapse the build when a support is destroyed. Throttled like
		// placement: a large base has hundreds of thousands of edges.
		// One unit of budget per edge, not per piece. A heavily connected junction
		// can carry far more links than an average wall panel, and doing all of
		// them in a single tick is what makes a dense build hitch mid-paste.
		for (int done = 0; done < budget && job.link_cursor < job.bp.pieces.size(); ++done)
		{
			const size_t idx = job.link_cursor;
			AActor* actor = job.spawned[idx];
			const std::vector<int32_t>& links = job.bp.pieces[idx].links;

			if (actor == nullptr || job.link_sub >= links.size())
			{
				++job.link_cursor;
				job.link_sub = 0;
				continue;   // still costs budget, so a huge run of gaps cannot stall a tick
			}

			const int32_t target = links[job.link_sub++];
			if (target < 0 || target >= static_cast<int32_t>(job.spawned.size())) continue;

			AActor* other = job.spawned[target];
			if (other == nullptr) continue;

			static_cast<APrimalStructure*>(actor)->LinkStructure(static_cast<APrimalStructure*>(other));
			++job.links_made;
		}

		if (job.link_cursor >= job.bp.pieces.size()) FinishJob(job);
	}

	bool UndoLastPaste(int& destroyed, std::string& error)
	{
		destroyed = 0;
		if (g_job.has_value()) { error = "a paste is still running; cancel it first"; return false; }

		// Fall back to the on-disk record, so undo survives a plugin reload or a
		// server restart rather than quietly forgetting the paste it was for.
		if (!g_last.valid || g_last.items.empty())
		{
			if (!LoadUndo(g_last)) { error = "nothing to undo"; return false; }
		}

		// Bucket positions rather than comparing floats exactly. Pieces are
		// hundreds of units apart, so 10-unit buckets cannot collide, but they
		// absorb any drift between what we wrote and where the actor reports.
		auto Key = [](float x, float y, float z) {
			return std::to_string(std::lround(x / 10.0f)) + "_"
			     + std::to_string(std::lround(y / 10.0f)) + "_"
			     + std::to_string(std::lround(z / 10.0f));
		};

		std::unordered_map<std::string, std::string> wanted; // position -> class path
		for (const UndoItem& it : g_last.items)
		{
			if (it.cls < 0 || it.cls >= static_cast<int32_t>(g_last.classes.size())) continue;
			wanted[Key(it.x, it.y, it.z)] = g_last.classes[it.cls];
		}
		if (wanted.empty()) { error = "nothing to undo"; return false; }

		const FVector center{
			(g_last.lo.X + g_last.hi.X) * 0.5f,
			(g_last.lo.Y + g_last.hi.Y) * 0.5f,
			(g_last.lo.Z + g_last.hi.Z) * 0.5f
		};
		const float dx = g_last.hi.X - g_last.lo.X;
		const float dy = g_last.hi.Y - g_last.lo.Y;
		const float dz = g_last.hi.Z - g_last.lo.Z;
		const float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);

		std::vector<AActor*> live;
		for (EServerOctreeGroup::Type group : { EServerOctreeGroup::STRUCTURES,
		                                        EServerOctreeGroup::STRUCTURES_CORE })
		{
			TArray<AActor*> found = ArkApi::GetApiUtils().GetAllActorsInRange(center, radius, group);
			for (int i = 0; i < found.Num(); ++i)
				if (found[i] != nullptr) live.push_back(found[i]);
		}

		// Match on position AND class, so a pre-existing structure that happens
		// to sit where a pasted piece went is never destroyed by mistake.
		for (AActor* actor : live)
		{
			FVector loc; FRotator rot;
			if (!ReadTransform(actor, loc, rot)) continue;

			const auto it = wanted.find(Key(loc.X, loc.Y, loc.Z));
			if (it == wanted.end()) continue;

			const FString path = GetBlueprintPath(actor);
			if (path.Len() == 0 || path.ToString() != it->second) continue;

			actor->Destroy(true, true);
			++destroyed;
		}

		g_last = PasteRecord{};
		std::error_code ec;
		std::filesystem::remove(UndoPath(), ec);
		return true;
	}
} // namespace sb
