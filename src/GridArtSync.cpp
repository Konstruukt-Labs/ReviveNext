#include "GridArtSync.h"
#include "ArtMatcherGui.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	std::optional<std::string> RegReadSZ(HKEY root, const wchar_t* subkey, const wchar_t* value)
	{
		HKEY key;
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
			return std::nullopt;

		DWORD type = 0, size = 0;
		if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0)
		{
			RegCloseKey(key);
			return std::nullopt;
		}

		std::wstring wresult(size / sizeof(wchar_t), L'\0');
		RegQueryValueExW(key, value, nullptr, nullptr, (LPBYTE)wresult.data(), &size);
		RegCloseKey(key);

		while (!wresult.empty() && wresult.back() == L'\0')
			wresult.pop_back();

		int len = WideCharToMultiByte(CP_UTF8, 0, wresult.c_str(), (int)wresult.size(), nullptr, 0, nullptr, nullptr);
		std::string result(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wresult.c_str(), (int)wresult.size(), result.data(), len, nullptr, nullptr);
		return result.empty() ? std::nullopt : std::make_optional(result);
	}

	// Steam's grid dir, where ReviveNext's art used to live under
	// shortcut-appid names. May not exist (Steam never run, shortcuts
	// gone) - the migration just finds nothing then.
	std::string FindSteamGridDir()
	{
		HKEY key;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS)
			return {};
		DWORD type = 0, size = 0;
		if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0)
		{
			RegCloseKey(key);
			return {};
		}
		std::wstring wpath(size / sizeof(wchar_t), L'\0');
		RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, (LPBYTE)wpath.data(), &size);
		RegCloseKey(key);

		int len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), (int)wcslen(wpath.c_str()), nullptr, 0, nullptr, nullptr);
		std::string path(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), (int)wcslen(wpath.c_str()), path.data(), len, nullptr, nullptr);

		// <SteamPath>/userdata/<userid>/config/grid - the one userid dir
		// present is the logged-in user's.
		fs::path userdata = fs::path(path) / "userdata";
		std::error_code ec;
		if (!fs::exists(userdata, ec))
			return {};
		for (const auto& entry : fs::directory_iterator(userdata, ec))
		{
			fs::path grid = entry.path() / "config" / "grid";
			if (fs::exists(grid, ec))
				return grid.string();
		}
		return {};
	}

	bool AllDigits(const std::string& s)
	{
		return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
	}

	// Copies one already-matched art file (and its portrait sibling,
	// <appid>p<ext>, if present) from Steam's grid dir into artDir under
	// the game's own name. Only ever fills gaps - never overwrites what
	// the new naming already has.
	void MigrateOneGame(const std::string& canonicalName, const std::string& steamGridDir, const std::string& artDir)
	{
		constexpr const char* extensions[] = { ".png", ".jpg", ".jpeg", ".webp" };

		// The registration manifest recorded each game's image_path -
		// pointing at its (appid-named) grid file. That filename is the
		// only remaining record of which appid belonged to which game
		// once the Steam shortcuts themselves are gone.
		const fs::path manifestPath = fs::path(artDir).parent_path() / "revive.vrmanifest";
		std::ifstream in(manifestPath);
		if (!in)
			return;
		nlohmann::json manifest;
		try
		{
			in >> manifest;
		}
		catch (...)
		{
			return;
		}
		if (!manifest.contains("applications"))
			return;

		std::string appid;
		for (const auto& app : manifest["applications"])
		{
			if (app.value("app_key", "") == "revive.app." + canonicalName && app.contains("image_path"))
			{
				std::string file = fs::path(app["image_path"].get<std::string>()).stem().string();
				if (AllDigits(file))
					appid = file;
				break;
			}
		}
		if (appid.empty())
			return;

		std::error_code ec;
		for (const char* suffix : { "", "p" })
		{
			for (const char* ext : extensions)
			{
				const fs::path src = fs::path(steamGridDir) / (appid + suffix + ext);
				const fs::path dst = fs::path(artDir) / (canonicalName + suffix + ext);
				if (fs::exists(src, ec) && !fs::exists(dst, ec))
					fs::copy_file(src, dst, ec);
			}
		}
	}
}

std::vector<MetaGame> GridArtSync::Run(const std::vector<MetaGame>& games,
	const std::string& injectorPath, const std::string& artDir)
{
	if (games.empty())
		return {};

	std::error_code ec;
	fs::create_directories(artDir, ec);

	// One-time carry-over of art matched under the old Steam-shortcut
	// naming, so picks made before the Steam Library integration was
	// dropped survive without re-matching.
	if (std::string steamGridDir = FindSteamGridDir(); !steamGridDir.empty())
		for (const MetaGame& game : games)
			MigrateOneGame(game.canonicalName, steamGridDir, artDir);

	// No forced prompt when a key isn't configured - the window itself has
	// a "Settings..." button to add one whenever the user wants.
	auto apiKey = RegReadSZ(HKEY_CURRENT_USER, L"Software\\ReviveNext", L"SteamGridDbApiKey");

	return ArtMatcherGui::Run(games, injectorPath, artDir, apiKey);
}
