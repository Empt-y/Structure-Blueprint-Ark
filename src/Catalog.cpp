#include "Catalog.h"
#include "ArkUtils.h"
#include "Storage.h"

#include <API/ARK/Ark.h>
#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace sb
{
	namespace
	{
		// A class's own full name looks like
		//   BlueprintGeneratedClass /Game/.../Foo.Foo_C
		// and the loadable form is
		//   Blueprint'/Game/.../Foo.Foo'
		// Same shape as GetBlueprintPath, but taken off the class directly rather
		// than via its default object.
		std::string ClassToBlueprintPath(UClass* cls)
		{
			if (cls == nullptr) return "";

			FString full;
			cls->GetFullName(&full, nullptr);

			int space = 0;
			if (!full.FindChar(' ', space)) return "";

			FString path = full.Mid(space + 1, full.Len() - (space + 1));
			if (path.Len() < 3) return "";

			// Native classes have no _C suffix and are not Blueprint-loadable;
			// keep them raw so the catalogue still records that they exist.
			std::string s = path.ToString();
			if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0)
			{
				s = "Blueprint'" + s.substr(0, s.size() - 2) + "'";
			}
			return s;
		}

		std::string ShortName(const std::string& path)
		{
			const size_t dot = path.rfind('.');
			std::string name = (dot == std::string::npos) ? path : path.substr(dot + 1);
			if (!name.empty() && name.back() == 0x27) name.pop_back();
			return name;
		}

		// IsA for a class rather than an actor - the catalogue walks classes.
		bool IsA_Class(UClass* cls, UClass* base)
		{
			return cls != nullptr && base != nullptr && cls->IsChildOf(base);
		}

		// Mod content lives under /Game/Mods/<workshop id>/...
		std::string ModIdOf(const std::string& path)
		{
			const std::string marker = "/Game/Mods/";
			const size_t at = path.find(marker);
			if (at == std::string::npos) return "";

			const size_t start = at + marker.size();
			const size_t end = path.find('/', start);
			if (end == std::string::npos) return "";
			return path.substr(start, end - start);
		}
	} // namespace

	bool WriteCatalog(CatalogStats& stats, std::string& error)
	{
		stats = CatalogStats{};

		UClass* base = Globals::FindClass("Class /Script/ShooterGame.PrimalStructure");
		if (base == nullptr)
		{
			error = "could not resolve the APrimalStructure base class";
			return false;
		}

		std::vector<nlohmann::json> entries;
		std::set<std::string> mods;
		std::set<std::string> seen_paths;

		auto& array = Globals::GUObjectArray()().ObjObjects;
		stats.objects_scanned = array.NumElements;

		for (int i = 0; i < array.NumElements; ++i)
		{
			UObject* obj = array.GetObjectPtr(i)->Object;
			if (obj == nullptr) continue;
			if (!Globals::HasClassCastFlags(obj, ClassCastFlags::CASTCLASS_UClass)) continue;

			++stats.classes_seen;
			auto* cls = static_cast<UClass*>(obj);

			if (cls == base) continue;
			if (!cls->IsChildOf(base)) continue;

			const std::string path = ClassToBlueprintPath(cls);
			if (path.empty()) continue;
			if (!seen_paths.insert(path).second) continue;

			++stats.structures;

			const std::string mod_id = ModIdOf(path);
			if (!mod_id.empty()) { ++stats.from_mods; mods.insert(mod_id); }

			nlohmann::json e{
				{"name", ShortName(path)},
				{"path", path},
			};
			if (!mod_id.empty()) e["mod"] = mod_id;

			// Turrets carry their engagement ranges on the class default object,
			// so real numbers can be read without placing anything in the world.
			if (IsA_Class(cls, TurretClass()))
			{
				if (UObject* cdo = cls->GetDefaultObject(true))
				{
					auto* turret = static_cast<APrimalStructureTurret*>(cdo);
					if (float* r = turret->TargetingRangesField()())
						e["ranges"] = { r[0], r[1], r[2] };

					// The firing cone, straight off the class. Yaw is probably
					// free rotation; pitch is the one that decides whether a
					// turret can engage a flier overhead or a soaker at its feet.
					e["yaw_delta"] = turret->MaxFireYawDeltaField();
					e["pitch_delta"] = turret->MaxFirePitchDeltaField();
					e["range_setting"] = turret->RangeSettingField();
					e["fire_interval"] = turret->FireIntervalField();
					e["bullets_per_shot"] = turret->NumBulletsPerShotField();
					e["aim_spread"] = turret->AimSpreadField();
				}
			}
			entries.push_back(std::move(e));
		}

		stats.mods_present = static_cast<int>(mods.size());

		std::sort(entries.begin(), entries.end(),
			[](const nlohmann::json& a, const nlohmann::json& b) {
				return a["name"].get<std::string>() < b["name"].get<std::string>();
			});

		try
		{
			const nlohmann::json doc{
				{"structures", static_cast<int>(entries.size())},
				{"mods", std::vector<std::string>(mods.begin(), mods.end())},
				{"entries", entries},
			};

			const std::string path = storage::PluginDir() + "/catalog.json";
			std::ofstream out(path, std::ios::trunc);
			if (!out) { error = "cannot write catalog.json"; return false; }
			out << doc.dump(1, '\t');
		}
		catch (const std::exception& e) { error = e.what(); return false; }

		return true;
	}
} // namespace sb
