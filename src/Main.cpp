// StructureBlueprint - admin area-select / save / paste for ARK: Survival Evolved
// Built against the ASE ArkServerAPI (ArkApi).

#include "ArkUtils.h"
#include "Capture.h"
#include "Catalog.h"
#include "Coverage.h"
#include "Fill.h"
#include "Paste.h"
#include "Preview.h"
#include "Storage.h"

#include <API/ARK/Ark.h>
#include <Logger/Logger.h>

#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "ArkApi.lib")

namespace sb
{
	namespace
	{
		std::vector<std::string> Split(const std::string& s)
		{
			std::istringstream iss(s);
			std::vector<std::string> out;
			std::string tok;
			while (iss >> tok) out.push_back(tok);
			return out;
		}

		std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		bool HasFlag(const std::vector<std::string>& args, const std::string& flag)
		{
			for (const auto& a : args) if (a == flag) return true;
			return false;
		}

		// Returns the value following `flag`, or empty if absent.
		std::string FlagValue(const std::vector<std::string>& args, const std::string& flag)
		{
			for (size_t i = 0; i + 1 < args.size(); ++i)
				if (args[i] == flag) return args[i + 1];
			return "";
		}

		void Usage(AShooterPlayerController* pc)
		{
			Reply(pc, L"StructureCopy - /sc (long forms in brackets)");
			Reply(pc, L"  p 1 | p 2        set selection corners at your feet (pos1/pos2)");
			Reply(pc, L"  s <name>         capture the selection (save) [--margin N] [--no-items]");
			Reply(pc, L"  v <name> [yaw]   build it here (paste) [--invuln] [--exact]");
			Reply(pc, L"                   [--ground high|low] [--sink N]");
			Reply(pc, L"                   [--team N] [--rate N]");
			Reply(pc, L"  pv <name> [yaw]  outline + collision check, no build (preview)");
			Reply(pc, L"  c                build the previewed placement (confirm)");
			Reply(pc, L"  f [radius]       fill nearby turrets to capacity [--stack N]");
			Reply(pc, L"  u                undo the last paste");
			Reply(pc, L"  l | info <name> | delete <name> | clear | cancel");
			Reply(pc, L"  scan | links     diagnostics for what is in the selection");
			Reply(pc, L"  catalog          dump every structure class, mods included");
			Reply(pc, L"  t [name|size]    elevation map of the ground in front of you");
			Reply(pc, L"  cov [span] [dz]  turret coverage map around you");
		}

		void HandleSave(AShooterPlayerController* pc, uint64 steam_id, const std::vector<std::string>& args)
		{
			if (args.size() < 3) { Reply(pc, L"usage: /sb save <name>"); return; }
			const std::string name = args[2];
			if (!storage::IsValidName(name))
			{
				Reply(pc, L"name must be 1-64 chars of letters, digits, _ or -");
				return;
			}

			// Structure origins sit at tile centres, so the box is grown slightly
			// before testing. --margin overrides; --margin 0 is exact.
			float margin = kDefaultMargin;
			const std::string margin_arg = FlagValue(args, "--margin");
			if (!margin_arg.empty()) margin = static_cast<float>(std::atof(margin_arg.c_str()));

			Blueprint bp;
			CaptureStats stats;
			std::string error;
			// Inventories are captured by default. --no-items keeps a save small
			// when only the shell matters.
			const bool with_items = !HasFlag(args, "--no-items");

			if (!CaptureSelection(SelectionFor(steam_id), name, steam_id, margin,
			                      with_items, bp, stats, error))
			{
				Reply(pc, L"capture failed: " + Widen(error));
				return;
			}
			if (!storage::Save(bp, error))
			{
				Reply(pc, L"save failed: " + Widen(error));
				return;
			}
			Reply(pc, L"Saved '" + Widen(name) + L"' - "
				+ std::to_wstring(bp.pieces.size()) + L" pieces, "
				+ std::to_wstring(bp.classes.size()) + L" distinct types.");

			// Always show where actors were lost. A silent under-capture is the
			// worst failure mode here: the save looks fine until you paste it.
			Reply(pc, L"  found " + std::to_wstring(stats.octree_returned)
				+ L" nearby, " + std::to_wstring(stats.in_box) + L" inside the box");
			if (stats.containers > 0 || stats.turrets > 0)
				Reply(pc, L"  contents: " + std::to_wstring(stats.item_stacks)
					+ L" item stacks in " + std::to_wstring(stats.containers)
					+ L" containers, " + std::to_wstring(stats.turrets) + L" turret settings");
			Reply(pc, L"  floor mounts: " + std::to_wstring(stats.floors_captured)
				+ L" (turrets, generators and anything sitting on a surface)");
			Reply(pc, L"  support links: " + std::to_wstring(stats.links_captured)
				+ L" captured, " + std::to_wstring(stats.links_outside) + L" pointed outside the box");
			if (stats.links_captured == 0)
				Reply(pc, L"  WARNING: no support links captured - the pasted copy will"
					+ std::wstring(L" not collapse when its supports are destroyed."));
			if (stats.near_miss > 0)
				Reply(pc, L"  WARNING: " + std::to_wstring(stats.near_miss)
					+ L" structures sit just OUTSIDE the box - widen the selection,"
					+ L" or raise --margin, and save again.");
			if (stats.no_path > 0 || stats.no_transform > 0)
				Reply(pc, L"  SKIPPED " + std::to_wstring(stats.no_path)
					+ L" with unresolvable blueprint path, "
					+ std::to_wstring(stats.no_transform) + L" with no transform");
		}

		// Shared by preview and paste so the two can never disagree about where
		// the build would land.
		PasteOptions BuildOptions(AShooterPlayerController* pc, const std::vector<std::string>& args)
		{
			PasteOptions opts;
			opts.origin = ArkApi::IApiUtils::GetPosition(pc);
			opts.invulnerable = HasFlag(args, "--invuln");
			opts.snap_to_ground = !HasFlag(args, "--exact");

			// A uniform drop, so the base buries rather than sitting on the
			// surface. Uniform on purpose: per-column heights would give uneven
			// foundations and a snap surface per column.
			const std::string sink_arg = FlagValue(args, "--sink");
			if (!sink_arg.empty()) opts.sink = static_cast<float>(std::atof(sink_arg.c_str()));

			// Which sampled ground height the whole build sits on. Average by
			// default - what builders do by hand.
			const std::string ground = FlagValue(args, "--ground");
			if (ground == "low")       opts.ground_mode = PasteOptions::Ground::Lowest;
			else if (ground == "high") opts.ground_mode = PasteOptions::Ground::Highest;

			if (args.size() > 3 && args[3][0] != '-')
			{
				try { opts.yaw = std::stof(args[3]); } catch (...) { opts.yaw = 0.f; }
			}

			const std::string team_arg = FlagValue(args, "--team");
			opts.team = team_arg.empty()
				? ArkApi::GetApiUtils().GetTribeID(pc)
				: std::atoi(team_arg.c_str());

			const std::string rate_arg = FlagValue(args, "--rate");
			if (!rate_arg.empty()) opts.per_tick = std::max(1, std::atoi(rate_arg.c_str()));

			opts.owner_id = static_cast<int>(ArkApi::IApiUtils::GetPlayerID(pc));
			opts.owner_name = ArkApi::IApiUtils::GetCharacterName(pc);
			return opts;
		}

		void StartPaste(AShooterPlayerController* pc, uint64 steam_id,
		                const Blueprint& bp, const PasteOptions& opts)
		{
			std::string error;
			if (!BeginPaste(bp, opts, steam_id, error))
			{
				Reply(pc, L"paste failed: " + Widen(error));
				return;
			}

			const int spread = static_cast<int>(LastGroundSpread());
			if (opts.sink > 0.f)
				Reply(pc, L"Sunk " + std::to_wstring(static_cast<int>(opts.sink))
					+ L" units into the ground");
			if (opts.snap_to_ground && spread > 200)
				Reply(pc, L"NOTE: ground varies " + std::to_wstring(spread)
					+ L" units under this footprint. Sitting on the AVERAGE, so expect"
					+ L" some sinking on the high side and overhang on the low."
					+ L" --ground high|low to change.");

			Reply(pc, L"Pasting " + std::to_wstring(bp.pieces.size()) + L" pieces at "
				+ std::to_wstring(static_cast<int>(opts.per_tick)) + L"/tick"
				+ (opts.invulnerable ? L" (INVULNERABLE - no owner, cannot be damaged or demolished)" : L""));
		}

		void HandlePaste(AShooterPlayerController* pc, uint64 steam_id, const std::vector<std::string>& args)
		{
			if (args.size() < 3) { Reply(pc, L"usage: /sb paste <name> [yaw]"); return; }
			if (PasteInProgress()) { Reply(pc, L"a paste is already running"); return; }

			std::string error;
			auto bp = storage::Load(args[2], error);
			if (!bp) { Reply(pc, L"load failed: " + Widen(error)); return; }

			PasteOptions opts = BuildOptions(pc, args);
			AdjustOriginToGround(*bp, opts);
			StartPaste(pc, steam_id, *bp, opts);
		}

		// Diagnostic: which octree group actually holds the pieces here.
		void HandleScan(AShooterPlayerController* pc, uint64 steam_id)
		{
			const Selection& sel = SelectionFor(steam_id);
			if (!sel.Complete()) { Reply(pc, L"set both corners first"); return; }

			const FVector lo = sel.Min(), hi = sel.Max();
			Reply(pc, L"Selection " + std::to_wstring(static_cast<int>(hi.X - lo.X)) + L" x "
				+ std::to_wstring(static_cast<int>(hi.Y - lo.Y)) + L" x "
				+ std::to_wstring(static_cast<int>(hi.Z - lo.Z)));

			for (const ScanRow& row : ScanGroups(sel))
			{
				Reply(pc, L"  " + Widen(row.name) + L": " + std::to_wstring(row.in_box)
					+ L" in box (" + std::to_wstring(row.total) + L" in radius)");
			}

			// Cross-check against the level's own actor list. Anything listed
			// here is physically inside the box but invisible to the octree
			// query capture uses, which is the only way pieces can vanish from a
			// selection that was set generously wide.
			int level_total = 0, level_in_box = 0;
			const std::vector<MissedRow> missed = ScanMissed(sel, level_total, level_in_box);

			Reply(pc, L"  LEVEL actor list: " + std::to_wstring(level_in_box)
				+ L" in box (of " + std::to_wstring(level_total) + L" on the map)");

			if (missed.empty())
			{
				Reply(pc, L"  nothing in the box is hidden from the octree");
			}
			else
			{
				int total_missed = 0;
				for (const MissedRow& m : missed) total_missed += m.count;
				Reply(pc, L"  MISSED BY OCTREE: " + std::to_wstring(total_missed) + L" actors");
				int shown = 0;
				for (const MissedRow& m : missed)
				{
					if (shown++ >= 6) break;
					Reply(pc, L"    " + std::to_wstring(m.count) + L" x " + Widen(m.cls));
				}
			}
		}

		// Diagnostic: which link fields ARK actually maintains on these pieces.
		void HandleLinks(AShooterPlayerController* pc, uint64 steam_id)
		{
			const Selection& sel = SelectionFor(steam_id);
			if (!sel.Complete()) { Reply(pc, L"set both corners first"); return; }

			const LinkStats st = InspectLinks(sel);
			Reply(pc, L"Link graph over " + std::to_wstring(st.structures) + L" structures:");
			Reply(pc, L"  PlacedOnFloorStructure set : " + std::to_wstring(st.with_placed_on_floor));
			Reply(pc, L"  LinkedStructures non-empty : " + std::to_wstring(st.with_linked)
				+ L" (" + std::to_wstring(st.linked_refs) + L" refs)");
			Reply(pc, L"  StructuresPlacedOnFloor    : " + std::to_wstring(st.with_on_floor_list)
				+ L" (" + std::to_wstring(st.on_floor_refs) + L" refs)");
			Reply(pc, L"  PrimarySnappedStructChild  : " + std::to_wstring(st.with_snapped_child));
		}

		void HandleFill(AShooterPlayerController* pc, uint64 /*steam_id*/,
		                const std::vector<std::string>& args)
		{
			// Radius around the admin, not a box selection: filling turrets is
			// something you do standing in the base. Fills to capacity - free
			// slots times the ammo's own stack size - rather than a fixed amount.
			float radius = kDefaultFillRadius;
			if (args.size() > 2)
			{
				const float parsed = static_cast<float>(std::atof(args[2].c_str()));
				if (parsed > 0.f) radius = parsed;
			}

			int per_stack = 0;   // 0 = use the ammo's own max stack
			const std::string stack_arg = FlagValue(args, "--stack");
			if (!stack_arg.empty()) per_stack = std::atoi(stack_arg.c_str());

			FillStats stats;
			std::string error;
			if (!FillTurrets(ArkApi::IApiUtils::GetPosition(pc), radius, per_stack, stats, error))
			{
				Reply(pc, L"fill failed: " + Widen(error));
				return;
			}

			Reply(pc, L"Filled " + std::to_wstring(stats.filled) + L" of "
				+ std::to_wstring(stats.turrets) + L" turrets within "
				+ std::to_wstring(static_cast<int>(radius)) + L" units");
			Reply(pc, L"  " + std::to_wstring(stats.stacks_added) + L" stacks, "
				+ std::to_wstring(stats.rounds_added) + L" rounds total");
			if (stats.already_full > 0)
				Reply(pc, L"  " + std::to_wstring(stats.already_full) + L" already full");
			if (stats.no_template > 0 || stats.no_inventory > 0)
				Reply(pc, L"  skipped " + std::to_wstring(stats.no_template)
					+ L" with no declared ammo type, " + std::to_wstring(stats.no_inventory)
					+ L" with no inventory");
		}

		// Dumps every loaded structure class, mod ones included, to catalog.json.
		void HandleCatalog(AShooterPlayerController* pc)
		{
			CatalogStats stats;
			std::string error;
			if (!WriteCatalog(stats, error))
			{
				Reply(pc, L"catalog failed: " + Widen(error));
				return;
			}

			Reply(pc, L"Catalogued " + std::to_wstring(stats.structures)
				+ L" structure classes (" + std::to_wstring(stats.from_mods)
				+ L" from " + std::to_wstring(stats.mods_present) + L" mods)");
			Reply(pc, L"  scanned " + std::to_wstring(stats.objects_scanned)
				+ L" objects, " + std::to_wstring(stats.classes_seen) + L" classes");
			Reply(pc, L"  written to ArkApi/Plugins/StructureBlueprint/catalog.json");
		}

		// How many turrets can engage each point around you. Distance is 3D:
		// a turret 8000 units up cannot reach the ground with a 6000 range, and
		// flat circles would claim cover that is not there.
		void HandleCoverage(AShooterPlayerController* pc, const std::vector<std::string>& args)
		{
			float span = 12000.f;   // 40 tiles - twice a Heavy turret's reach
			float fixed_z = 0.f;

			if (args.size() > 2)
			{
				const float n = static_cast<float>(std::atof(args[2].c_str()));
				if (n > 0.f) span = n;
			}
			if (args.size() > 3)
			{
				const float n = static_cast<float>(std::atof(args[3].c_str()));
				if (n != 0.f) fixed_z = ArkApi::IApiUtils::GetPosition(pc).Z + n;
			}

			CoverageMap map;
			if (!ScanCoverage(ArkApi::IApiUtils::GetPosition(pc), span, fixed_z, map))
			{
				Reply(pc, map.turrets_considered == 0
					? L"no turrets in range to map"
					: L"could not sample here");
				return;
			}

			Reply(pc, L"Coverage from " + std::to_wstring(map.turrets_considered)
				+ L" turrets over " + std::to_wstring(static_cast<int>(span / 299.4053f))
				+ L" tiles" + (fixed_z != 0.f
					? L", at " + std::to_wstring(static_cast<int>(args.size() > 3 ? std::atof(args[3].c_str()) : 0))
						+ L" units above you"
					: L", following the ground"));
			Reply(pc, L"peak " + std::to_wstring(map.max_count)
				+ L" turrets on one point; " + std::to_wstring(map.covered_cells)
				+ L" of " + std::to_wstring(map.total_cells) + L" sampled points covered at all");

			for (int gy = CoverageMap::kGrid - 1; gy >= 0; --gy)
			{
				std::wstring row;
				for (int gx = 0; gx < CoverageMap::kGrid; ++gx)
				{
					if (!map.hit[gx][gy]) { row += L" "; continue; }
					const int n = map.count[gx][gy];
					if (n == 0) row += L".";
					else if (n < 10) row += static_cast<wchar_t>(L'0' + n);
					else if (n < 36) row += static_cast<wchar_t>(L'a' + (n - 10));
					else row += L"#";
				}
				Reply(pc, row);
			}
			Reply(pc, L". none  1-9 count  a-z 10-35  # 36+     (N at top, you at centre)");
		}

		// Draws the ground the build would sit on. Terrain is invisible from the
		// server and hard to judge by eye in game, and site selection is what
		// actually determines whether a rigid build sits well - the structure
		// itself cannot be bent to the ground without breaking its courses.
		void HandleTerrain(AShooterPlayerController* pc, const std::vector<std::string>& args)
		{
			float span_x = 4000.f, span_y = 4000.f;
			std::string label = "4000 x 4000";

			if (args.size() > 2)
			{
				const float n = static_cast<float>(std::atof(args[2].c_str()));
				if (n > 0.f)
				{
					span_x = span_y = n;
					label = args[2] + " x " + args[2];
				}
				else
				{
					std::string error;
					auto bp = storage::Load(args[2], error);
					if (!bp) { Reply(pc, L"load failed: " + Widen(error)); return; }
					span_x = bp->size_x; span_y = bp->size_y;
					label = args[2] + " footprint";
				}
			}

			TerrainMap map;
			if (!ScanTerrain(ArkApi::IApiUtils::GetPosition(pc), span_x, span_y, map))
			{
				Reply(pc, L"could not sample the ground here");
				return;
			}

			Reply(pc, L"Terrain over " + Widen(label) + L" from your position ("
				+ std::to_wstring(map.samples) + L" samples)");
			Reply(pc, L"spread " + std::to_wstring(static_cast<int>(map.hi - map.lo))
				+ L" units:  low " + std::to_wstring(static_cast<int>(map.lo))
				+ L"  high +" + std::to_wstring(static_cast<int>(map.hi))
				+ L"   (relative to average)");

			// North at the top, so the map reads the way the compass does.
			for (int gy = TerrainMap::kGrid - 1; gy >= 0; --gy)
			{
				std::wstring row;
				for (int gx = 0; gx < TerrainMap::kGrid; ++gx)
				{
					if (!map.hit[gx][gy]) { row += L" ? "; continue; }
					const float d = map.rel[gx][gy];
					if (d > 200.f)       row += L" ^ ";
					else if (d > 50.f)   row += L" + ";
					else if (d < -200.f) row += L" v ";
					else if (d < -50.f)  row += L" - ";
					else                 row += L" . ";
				}
				Reply(pc, row);
			}
			Reply(pc, L"^ >+200   + >+50   . level   - <-50   v <-200      (N at top)");
		}

		void HandlePreview(AShooterPlayerController* pc, uint64 steam_id, const std::vector<std::string>& args)
		{
			if (args.size() < 3) { Reply(pc, L"usage: /sb preview <name> [yaw]"); return; }

			std::string error;
			auto bp = storage::Load(args[2], error);
			if (!bp) { Reply(pc, L"load failed: " + Widen(error)); return; }

			PasteOptions opts = BuildOptions(pc, args);
			AdjustOriginToGround(*bp, opts);
			PreviewInfo info;
			if (!BeginPreview(steam_id, *bp, opts, info, error))
			{
				Reply(pc, L"preview failed: " + Widen(error));
				return;
			}

			Reply(pc, L"Preview: " + std::to_wstring(info.pieces) + L" pieces, footprint "
				+ std::to_wstring(static_cast<int>(info.span_x)) + L" x "
				+ std::to_wstring(static_cast<int>(info.span_y)) + L", height "
				+ std::to_wstring(static_cast<int>(info.span_z))
				+ L" (yaw " + std::to_wstring(static_cast<int>(info.yaw)) + L")");

			if (info.structures_in_way > 0)
				Reply(pc, L"WARNING: " + std::to_wstring(info.structures_in_way)
					+ L" existing structures are inside this volume");
			else
				Reply(pc, L"Clear: no existing structures in the way");

			// Per-piece result, not a footprint average: this says which pieces
			// actually end up inside the landscape.
			if (info.pieces_buried > 0)
				Reply(pc, L"WARNING: " + std::to_wstring(info.pieces_buried) + L" of "
					+ std::to_wstring(info.pieces) + L" pieces would be BURIED, worst by "
					+ std::to_wstring(static_cast<int>(info.worst_burial)) + L" units");
			else
				Reply(pc, L"Clear: no pieces end up below ground ("
					+ std::to_wstring(info.ground_columns) + L" columns checked)");

			if (info.enemy_structures > 0)
				Reply(pc, L"WARNING: " + std::to_wstring(info.enemy_structures)
					+ L" structures from " + std::to_wstring(info.enemy_teams)
					+ L" other tribe(s) nearby, closest "
					+ std::to_wstring(static_cast<int>(info.nearest_enemy)) + L" units");

			if (info.ground_sampled)
			{
				const int variation = static_cast<int>(info.ground_max_z - info.ground_min_z);
				const int base_offset = static_cast<int>(info.base_z - info.ground_min_z);

				Reply(pc, L"Ground varies " + std::to_wstring(variation)
					+ L" units across the footprint (" + std::to_wstring(info.ground_samples) + L" samples)");

				if (variation > 300)
					Reply(pc, L"WARNING: uneven ground - parts of the build may float or bury");
				if (base_offset < -100)
					Reply(pc, L"WARNING: base sits " + std::to_wstring(-base_offset) + L" units BELOW ground");
			}
			else
			{
				Reply(pc, L"Ground could not be sampled here");
			}

			Reply(pc, L"Markers placed at the corners. /sb confirm to build, /sb cancel to drop it.");
		}

		void HandleConfirm(AShooterPlayerController* pc, uint64 steam_id)
		{
			if (PasteInProgress()) { Reply(pc, L"a paste is already running"); return; }

			Blueprint bp;
			PasteOptions opts;
			std::string error;
			if (!ConfirmPreview(steam_id, bp, opts, error))
			{
				Reply(pc, L"confirm failed: " + Widen(error));
				return;
			}

			StartPaste(pc, steam_id, bp, opts);
		}

		void HandleInfo(AShooterPlayerController* pc, const std::vector<std::string>& args)
		{
			if (args.size() < 3) { Reply(pc, L"usage: /sb info <name>"); return; }
			std::string error;
			auto bp = storage::Load(args[2], error);
			if (!bp) { Reply(pc, L"load failed: " + Widen(error)); return; }

			Reply(pc, Widen(bp->name) + L": " + std::to_wstring(bp->pieces.size()) + L" pieces, "
				+ std::to_wstring(bp->classes.size()) + L" types");
			Reply(pc, L"captured " + Widen(bp->created_utc) + L" by " + Widen(bp->created_by));
			Reply(pc, L"extent " + std::to_wstring(static_cast<int>(bp->size_x)) + L" x "
				+ std::to_wstring(static_cast<int>(bp->size_y)) + L" x "
				+ std::to_wstring(static_cast<int>(bp->size_z)));
		}

		void Dispatch(AShooterPlayerController* pc, FString* message, EChatSendMode::Type)
		{
			if (!IsAdmin(pc)) { Reply(pc, L"admin only"); return; }

			const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
			const std::vector<std::string> args = Split(message->ToString());
			if (args.size() < 2) { Usage(pc); return; }

			const std::string& cmd = args[1];

			// Position: "/sc p 1" and "/sc p 2". The long pos1/pos2 spellings stay
			// as aliases so older notes and muscle memory keep working.
			bool set_a = (cmd == "pos1");
			bool set_b = (cmd == "pos2");
			if (cmd == "p")
			{
				if (args.size() < 3) { Reply(pc, L"usage: /sc p 1  or  /sc p 2"); return; }
				if (args[2] == "1") set_a = true;
				else if (args[2] == "2") set_b = true;
				else { Reply(pc, L"usage: /sc p 1  or  /sc p 2"); return; }
			}

			if (set_a || set_b)
			{
				Selection& sel = SelectionFor(steam_id);
				const FVector pos = ArkApi::IApiUtils::GetPosition(pc);
				if (set_a) { sel.a = pos; sel.has_a = true; }
				else { sel.b = pos; sel.has_b = true; }

				Reply(pc, std::wstring(set_a ? L"pos1" : L"pos2") + L" set");
				if (sel.Complete())
				{
					const FVector lo = sel.Min(), hi = sel.Max();
					Reply(pc, L"selection " + std::to_wstring(static_cast<int>(hi.X - lo.X)) + L" x "
						+ std::to_wstring(static_cast<int>(hi.Y - lo.Y)) + L" x "
						+ std::to_wstring(static_cast<int>(hi.Z - lo.Z)));
				}
			}
			else if (cmd == "clear") { ClearSelection(steam_id); Reply(pc, L"selection cleared"); }
			else if (cmd == "save" || cmd == "s") { HandleSave(pc, steam_id, args); }
			else if (cmd == "scan") { HandleScan(pc, steam_id); }
			else if (cmd == "links") { HandleLinks(pc, steam_id); }
			else if (cmd == "catalog") { HandleCatalog(pc); }
			else if (cmd == "terrain" || cmd == "t") { HandleTerrain(pc, args); }
			else if (cmd == "coverage" || cmd == "cov") { HandleCoverage(pc, args); }
			else if (cmd == "fill" || cmd == "f") { HandleFill(pc, steam_id, args); }
			else if (cmd == "preview" || cmd == "pv") { HandlePreview(pc, steam_id, args); }
			else if (cmd == "confirm" || cmd == "c") { HandleConfirm(pc, steam_id); }
			else if (cmd == "paste" || cmd == "v") { HandlePaste(pc, steam_id, args); }
			else if (cmd == "info") { HandleInfo(pc, args); }
			else if (cmd == "cancel")
			{
				// One escape hatch for both: an admin who types cancel wants
				// whatever is pending gone, not to guess which subsystem owns it.
				CancelPaste();
				ClearPreview(steam_id);
				Reply(pc, L"cancelled - paste stopped and preview markers removed");
			}
			else if (cmd == "undo" || cmd == "u")
			{
				int destroyed = 0; std::string error;
				if (!UndoLastPaste(destroyed, error)) Reply(pc, L"undo failed: " + Widen(error));
				else Reply(pc, L"removed " + std::to_wstring(destroyed) + L" structures");
			}
			else if (cmd == "list" || cmd == "l")
			{
				const auto names = storage::List();
				if (names.empty()) { Reply(pc, L"no saved builds"); return; }
				for (const auto& n : names) Reply(pc, L"  " + Widen(n));
			}
			else if (cmd == "delete")
			{
				if (args.size() < 3) { Reply(pc, L"usage: /sb delete <name>"); return; }
				std::string error;
				if (!storage::Delete(args[2], error)) Reply(pc, L"delete failed: " + Widen(error));
				else Reply(pc, L"deleted " + Widen(args[2]));
			}
			else { Usage(pc); }
		}
	} // namespace

	void Load()
	{
		Log::Get().Init("StructureBlueprint");
		ArkApi::GetCommands().AddChatCommand(L"/sc", &Dispatch);
		ArkApi::GetCommands().AddChatCommand(L"/sb", &Dispatch); // old prefix, still accepted
		ArkApi::GetCommands().AddOnTickCallback(L"sb.paste", &TickPaste);
		Log::GetLog()->info("StructureBlueprint loaded. Saves: {}", storage::SavesDir());
	}

	void Unload()
	{
		CancelPaste();
		ArkApi::GetCommands().RemoveChatCommand(L"/sc");
		ArkApi::GetCommands().RemoveChatCommand(L"/sb");
		ArkApi::GetCommands().RemoveOnTickCallback(L"sb.paste");
	}
} // namespace sb

BOOL APIENTRY DllMain(HMODULE, DWORD call_reason, LPVOID)
{
	switch (call_reason)
	{
	case DLL_PROCESS_ATTACH: sb::Load(); break;
	case DLL_PROCESS_DETACH: sb::Unload(); break;
	default: break;
	}
	return TRUE;
}
