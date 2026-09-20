#pragma once

#include "Model.h"
#include <optional>
#include <string>
#include <vector>

namespace sb::storage
{
	// Saves live in <server>/ArkApi/Plugins/StructureBlueprint/saves/<name>.sbp.
	// That path is deliberately outside SavedArks/, so a library survives map
	// changes, world wipes and reboots, and can be copied between servers.
	std::string SavesDir();

	// Plugin root, for state that is not part of the blueprint library.
	std::string PluginDir();

	bool Save(const Blueprint& bp, std::string& error);
	std::optional<Blueprint> Load(const std::string& name, std::string& error);
	std::vector<std::string> List();
	bool Delete(const std::string& name, std::string& error);

	// Rejects path separators and traversal so a chat command can never write
	// outside the saves directory.
	bool IsValidName(const std::string& name);
} // namespace sb::storage
