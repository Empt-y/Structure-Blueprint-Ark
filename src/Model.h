#pragma once

// On-disk model for a saved build.
//
// Size matters: a 16x16 footprint at 32 walls high with interior rooms runs to
// well over 100k pieces. Two things keep that manageable:
//   1. Class paths are interned into a dictionary; each piece stores an index.
//   2. Keys are single characters and the whole document is gzipped on write.
// A piece therefore costs ~60 bytes of JSON, and gzip takes the repetitive
// coordinate lattice down much further.

#include <string>
#include <vector>
#include <cstdint>
#include <json.hpp>

namespace sb
{
	constexpr int kFormatVersion = 3;

	// One stack in a container. ARK has no single "quality" field: gear quality
	// lives in ItemStatValues, so the raw stat array is what gets carried.
	struct ItemEntry
	{
		int32_t cls = 0;            // index into Blueprint::item_classes
		int32_t quantity = 1;
		float durability = 0.f;
		float skill_bonus = 0.f;
		bool blueprint = false;
		std::vector<uint16_t> stats;   // ItemStatValues[8]
		std::string name;              // CustomItemName
	};

	struct Piece
	{
		int32_t class_index = 0;   // index into Blueprint::classes
		float x = 0.f, y = 0.f, z = 0.f;         // offset from origin, unrotated
		float pitch = 0.f, yaw = 0.f, roll = 0.f; // world rotation at capture time

		// Indices of the pieces this one is linked to. ARK stores structural
		// support as a graph in APrimalStructure::LinkedStructures, not as a
		// child->parent pointer, and it is that graph ReprocessTree walks when a
		// piece dies. Format 1 recorded a single parent, which was measured to be
		// empty on every real build.
		std::vector<int32_t> links;

		// Container contents and device state. Only populated for structures that
		// actually are containers or turrets - checked by class, not by name.
		std::vector<ItemEntry> items;
		std::vector<float> ranges;   // turret TargetingRanges[3]
		int32_t range_setting = -1;  // which of those three is selected
		int32_t ammo = -1;           // rounds loaded, for real ammo-depth figures
		int8_t activated = -1;       // -1 unknown, 0 off, 1 on

		// Things sitting ON a surface (turrets, generators) use a different
		// relationship: PlacedOnFloorStructure, not LinkedStructures. Measured on
		// a 415-piece tower, 37 pieces had exactly this and no links at all.
		// Replayed as NonPlayerFinalStructurePlacement's ForcePrimaryParent.
		int32_t floor = -1;
	};

	struct Blueprint
	{
		int format = kFormatVersion;
		std::string name;
		std::string created_utc;
		std::string created_by;    // steam id of the capturing admin
		std::string source_map;

		// Origin is the capture selection's minimum corner. Everything is stored
		// relative to it so a build is map-independent.
		float origin_x = 0.f, origin_y = 0.f, origin_z = 0.f;
		float size_x = 0.f, size_y = 0.f, size_z = 0.f;

		std::vector<std::string> classes;
		std::vector<std::string> item_classes;
		std::vector<Piece> pieces;
	};

	inline void to_json(nlohmann::json& j, const ItemEntry& it)
	{
		j = nlohmann::json{ {"c", it.cls}, {"q", it.quantity} };
		if (it.durability != 0.f)   j["d"] = it.durability;
		if (it.skill_bonus != 0.f)  j["k"] = it.skill_bonus;
		if (it.blueprint)           j["bp"] = true;
		if (!it.stats.empty())      j["s"] = it.stats;
		if (!it.name.empty())       j["n"] = it.name;
	}

	inline void from_json(const nlohmann::json& j, ItemEntry& it)
	{
		it.cls = j.at("c").get<int32_t>();
		it.quantity = j.value("q", 1);
		it.durability = j.value("d", 0.f);
		it.skill_bonus = j.value("k", 0.f);
		it.blueprint = j.value("bp", false);
		if (j.contains("s")) it.stats = j.at("s").get<std::vector<uint16_t>>();
		it.name = j.value("n", std::string());
	}

	inline void to_json(nlohmann::json& j, const Piece& p)
	{
		j = nlohmann::json{
			{"c", p.class_index},
			{"p", {p.x, p.y, p.z}},
			{"r", {p.pitch, p.yaw, p.roll}},
		};
		if (!p.links.empty()) j["l"] = p.links;
		if (p.floor >= 0) j["fl"] = p.floor;
		if (!p.items.empty()) j["i"] = p.items;
		if (!p.ranges.empty()) j["tr"] = p.ranges;
		if (p.range_setting >= 0) j["rs"] = p.range_setting;
		if (p.ammo >= 0) j["am"] = p.ammo;
		if (p.activated >= 0) j["a"] = static_cast<int>(p.activated);
	}

	inline void from_json(const nlohmann::json& j, Piece& p)
	{
		p.class_index = j.at("c").get<int32_t>();
		const auto& pos = j.at("p");
		p.x = pos[0].get<float>(); p.y = pos[1].get<float>(); p.z = pos[2].get<float>();
		const auto& rot = j.at("r");
		p.pitch = rot[0].get<float>(); p.yaw = rot[1].get<float>(); p.roll = rot[2].get<float>();
		p.floor = j.contains("fl") ? j.at("fl").get<int32_t>() : -1;
		if (j.contains("i")) p.items = j.at("i").get<std::vector<ItemEntry>>();
		if (j.contains("tr")) p.ranges = j.at("tr").get<std::vector<float>>();
		p.range_setting = j.contains("rs") ? j.at("rs").get<int32_t>() : -1;
		p.ammo = j.contains("am") ? j.at("am").get<int32_t>() : -1;
		p.activated = j.contains("a") ? static_cast<int8_t>(j.at("a").get<int>()) : -1;
		if (j.contains("l"))
		{
			p.links = j.at("l").get<std::vector<int32_t>>();
		}
		else if (j.contains("f"))
		{
			// Format 1 save: promote the single parent to a one-entry graph.
			const int32_t parent = j.at("f").get<int32_t>();
			if (parent >= 0) p.links.push_back(parent);
		}
	}

	inline void to_json(nlohmann::json& j, const Blueprint& b)
	{
		j = nlohmann::json{
			{"format", b.format},
			{"name", b.name},
			{"created_utc", b.created_utc},
			{"created_by", b.created_by},
			{"source_map", b.source_map},
			{"origin", {b.origin_x, b.origin_y, b.origin_z}},
			{"size", {b.size_x, b.size_y, b.size_z}},
			{"classes", b.classes},
			{"item_classes", b.item_classes},
			{"pieces", b.pieces},
		};
	}

	inline void from_json(const nlohmann::json& j, Blueprint& b)
	{
		b.format = j.at("format").get<int>();
		b.name = j.at("name").get<std::string>();
		b.created_utc = j.value("created_utc", "");
		b.created_by = j.value("created_by", "");
		b.source_map = j.value("source_map", "");
		const auto& o = j.at("origin");
		b.origin_x = o[0].get<float>(); b.origin_y = o[1].get<float>(); b.origin_z = o[2].get<float>();
		const auto& s = j.at("size");
		b.size_x = s[0].get<float>(); b.size_y = s[1].get<float>(); b.size_z = s[2].get<float>();
		b.classes = j.at("classes").get<std::vector<std::string>>();
		if (j.contains("item_classes"))
			b.item_classes = j.at("item_classes").get<std::vector<std::string>>();
		b.pieces = j.at("pieces").get<std::vector<Piece>>();
	}
} // namespace sb
