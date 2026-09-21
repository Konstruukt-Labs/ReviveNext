#include "TitleOverrides.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <optional>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
	std::optional<std::string> StorePath()
	{
		wchar_t path[MAX_PATH];
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
			return std::nullopt;
		return (fs::path(path) / "ReviveNext" / "titles.json").string();
	}
}

std::unordered_map<std::string, std::string> TitleOverrides::Load()
{
	std::unordered_map<std::string, std::string> result;

	auto path = StorePath();
	if (!path)
		return result;

	std::ifstream file(*path);
	if (!file)
		return result;

	try
	{
		json data;
		file >> data;
		if (data.is_object())
			for (auto& [key, value] : data.items())
				if (value.is_string())
					result[key] = value.get<std::string>();
	}
	catch (const json::exception&)
	{
	}
	return result;
}

void TitleOverrides::Set(const std::string& canonicalName, const std::string& title)
{
	auto path = StorePath();
	if (!path)
		return;

	auto current = Load();
	current[canonicalName] = title;

	std::error_code ec;
	fs::create_directories(fs::path(*path).parent_path(), ec);

	json data = json::object();
	for (auto& [key, value] : current)
		data[key] = value;

	std::ofstream file(*path, std::ios::trunc);
	if (file)
		file << data.dump(2);
}
