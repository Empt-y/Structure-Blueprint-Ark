#include "Storage.h"

#include "Gzip.h"

#include <Tools.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iterator>

namespace fs = std::filesystem;

namespace sb::storage
{
	static constexpr const char* kExt = ".sbp";

	std::string PluginDir()
	{
		return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/StructureBlueprint";
	}

	std::string SavesDir()
	{
		return PluginDir() + "/saves";
	}

	bool IsValidName(const std::string& name)
	{
		if (name.empty() || name.size() > 64) return false;
		return std::all_of(name.begin(), name.end(), [](unsigned char c) {
			return std::isalnum(c) || c == '_' || c == '-';
		});
	}

	static std::string PathFor(const std::string& name)
	{
		return SavesDir() + "/" + name + kExt;
	}

	bool Save(const Blueprint& bp, std::string& error)
	{
		try
		{
			fs::create_directories(SavesDir());

			const nlohmann::json j = bp;
			const std::string raw = j.dump();

			// Gzip on write. The coordinate lattice of a large base is highly
			// repetitive, so this typically lands at a few percent of raw size.
			std::string packed;
			if (!gz::Compress(raw, packed)) { error = "compression failed"; return false; }

			// Write to a temporary file and rename over the target, so an
			// interrupted save cannot leave a truncated library entry behind.
			const std::string final_path = PathFor(bp.name);
			const std::string temp_path = final_path + ".tmp";

			{
				std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
				if (!out) { error = "cannot open file for writing"; return false; }
				out.write(packed.data(), static_cast<std::streamsize>(packed.size()));
				if (!out.good()) { error = "write failed"; return false; }
			}

			std::error_code ec;
			fs::rename(temp_path, final_path, ec);
			if (ec) { fs::remove(temp_path, ec); error = "could not replace existing save"; return false; }

			return true;
		}
		catch (const std::exception& e) { error = e.what(); return false; }
	}

	std::optional<Blueprint> Load(const std::string& name, std::string& error)
	{
		try
		{
			const std::string path = PathFor(name);
			if (!fs::exists(path)) { error = "no save named '" + name + "'"; return std::nullopt; }

			std::ifstream in(path, std::ios::binary);
			if (!in) { error = "cannot open file for reading"; return std::nullopt; }

			const std::string packed((std::istreambuf_iterator<char>(in)),
			                          std::istreambuf_iterator<char>());

			std::string raw;
			if (!gz::Decompress(packed, raw)) { error = "save is corrupt or not a gzip file"; return std::nullopt; }

			const nlohmann::json j = nlohmann::json::parse(raw);
			Blueprint bp = j.get<Blueprint>();

			if (bp.format > kFormatVersion)
			{
				error = "save was written by a newer plugin version";
				return std::nullopt;
			}
			return bp;
		}
		catch (const std::exception& e) { error = e.what(); return std::nullopt; }
	}

	std::vector<std::string> List()
	{
		std::vector<std::string> names;
		std::error_code ec;
		if (!fs::exists(SavesDir(), ec)) return names;

		for (const auto& entry : fs::directory_iterator(SavesDir(), ec))
		{
			if (!entry.is_regular_file()) continue;
			if (entry.path().extension() != kExt) continue;
			names.push_back(entry.path().stem().string());
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	bool Delete(const std::string& name, std::string& error)
	{
		std::error_code ec;
		if (!fs::remove(PathFor(name), ec)) { error = ec ? ec.message() : "no such save"; return false; }
		return true;
	}
} // namespace sb::storage
