#pragma once

#include <string>

namespace sb
{
	struct CatalogStats
	{
		int objects_scanned = 0;  // entries walked in the global object array
		int classes_seen = 0;     // ...that were UClasses
		int structures = 0;       // ...deriving from APrimalStructure
		int from_mods = 0;        // ...whose path is under /Game/Mods/
		int mods_present = 0;     // distinct mod ids represented
	};

	// Walks the engine's global object array and writes every loaded class that
	// derives from APrimalStructure to catalog.json in the plugin folder, with
	// its blueprint path and owning mod id where it has one.
	//
	// Only LOADED classes are visible. ARK loads blueprint classes lazily, so a
	// mod structure nobody has referenced yet will not appear - the count is a
	// floor, not a guarantee of completeness.
	bool WriteCatalog(CatalogStats& stats, std::string& error);
} // namespace sb
