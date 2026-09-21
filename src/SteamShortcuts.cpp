#include "SteamShortcuts.h"
#include "BinaryVdf.h"
#include "IconConverter.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kOwnerTag = "ReviveNext";
	constexpr const char* kIdTagPrefix = "ReviveNext:";
	// One-time migration: entries tagged under this tool's previous
	// working name (MetaBridge, before this rename) are still recognized
	// as ours so renaming doesn't duplicate or orphan shortcuts already
	// synced under the old tag - FillEntry always writes the current tag,
	// so the sync right after this one upgrades them for good.
	constexpr const char* kLegacyIdTagPrefix = "MetaBridge:";

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

	std::optional<uint32_t> RegReadDWORD(HKEY root, const wchar_t* subkey, const wchar_t* value)
	{
		HKEY key;
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
			return std::nullopt;

		DWORD type = 0, data = 0, size = sizeof(data);
		LONG err = RegQueryValueExW(key, value, nullptr, &type, (LPBYTE)&data, &size);
		RegCloseKey(key);

		if (err != ERROR_SUCCESS || type != REG_DWORD)
			return std::nullopt;
		return data;
	}

	std::optional<std::string> FindActiveUserId(const std::string& steamPath)
	{
		// Steam being currently running (a safe assumption - launching a
		// SteamVR title needs it running anyway) means this reflects the
		// actual logged-in user directly.
		if (auto activeUser = RegReadDWORD(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser"))
		{
			if (*activeUser != 0)
				return std::to_string(*activeUser);
		}

		// Fall back to whichever userdata/<id>/config directory was
		// touched most recently.
		fs::path userdata = fs::path(steamPath) / "userdata";
		std::error_code ec;
		if (!fs::is_directory(userdata, ec))
			return std::nullopt;

		std::optional<std::string> newest;
		fs::file_time_type newestTime;
		for (const auto& entry : fs::directory_iterator(userdata, ec))
		{
			if (!entry.is_directory())
				continue;
			fs::path config = entry.path() / "config";
			auto t = fs::last_write_time(config, ec);
			if (ec)
				continue;
			if (!newest || t > newestTime)
			{
				newest = entry.path().filename().string();
				newestTime = t;
			}
		}
		return newest;
	}

	std::string Quote(const std::string& s)
	{
		return "\"" + s + "\"";
	}

	// Where converted icons are cached, independent of any Steam library -
	// %LOCALAPPDATA%\ReviveNext\icons\<canonicalName>.png. Persists across
	// syncs so unchanged art isn't reconverted every run.
	std::optional<std::string> GetIconCacheDir()
	{
		wchar_t path[MAX_PATH];
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
			return std::nullopt;
		return (fs::path(path) / "ReviveNext" / "icons").string();
	}

	// Steam's shortcuts.vdf "icon" field doesn't reliably pick up .jpg
	// files (the format Meta Horizon's own store asset cache uses) the way
	// it does .png, so convert once and point at the cached copy. Returns
	// an empty string (leaving Steam's default placeholder) on any
	// failure - never fatal to the sync itself.
	std::string ResolveIcon(const MetaGame& game)
	{
		if (game.coverArtPath.empty())
			return {};

		auto cacheDir = GetIconCacheDir();
		if (!cacheDir)
			return {};

		std::string destPath = (fs::path(*cacheDir) / (game.canonicalName + ".png")).string();
		if (!IconConverter::ConvertToPng(game.coverArtPath, destPath))
			return {};

		return destPath;
	}

	std::vector<uint8_t> ReadFile(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
			return {};
		size_t size = (size_t)file.tellg();
		file.seekg(0);
		std::vector<uint8_t> data(size);
		file.read((char*)data.data(), size);
		return data;
	}

	bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;
		file.write((const char*)data.data(), data.size());
		return (bool)file;
	}

	std::vector<std::string> GetTags(VdfNode& entry)
	{
		std::vector<std::string> tags;
		if (auto* tagsNode = entry.Find("tags"))
		{
			if (tagsNode->IsObject())
			{
				for (auto& [_, tag] : tagsNode->AsObject())
					tags.push_back(tag.AsString());
			}
		}
		return tags;
	}

	// The canonicalName this entry is tagged as owning (current or legacy
	// tag prefix), or nullopt if it's not one of ours.
	std::optional<std::string> CanonicalNameFor(VdfNode& entry)
	{
		for (auto& tag : GetTags(entry))
		{
			if (tag.rfind(kIdTagPrefix, 0) == 0)
				return tag.substr(strlen(kIdTagPrefix));
			if (tag.rfind(kLegacyIdTagPrefix, 0) == 0)
				return tag.substr(strlen(kLegacyIdTagPrefix));
		}
		return std::nullopt;
	}

	void SetTags(VdfNode& entry, const std::vector<std::string>& tags)
	{
		VdfNode::Object tagsObj;
		for (size_t i = 0; i < tags.size(); ++i)
			tagsObj[std::to_string(i)] = VdfNode(tags[i]);
		entry["tags"] = VdfNode(std::move(tagsObj));
	}

	void FillEntry(VdfNode& entry, const MetaGame& game, const std::string& injectorPath)
	{
		std::string exePath = (fs::path(game.libraryPath) / "Software" / game.canonicalName / game.launchFile).string();
		std::string startDir = (fs::path(game.libraryPath) / "Software" / game.canonicalName).string();
		// ReviveInjector.exe's /library flag expects the Oculus Libraries
		// registry GUID (it resolves the path itself); ReviveNext has
		// already resolved the real path, so skip /library and /base
		// entirely and hand it the final absolute exe path directly - any
		// argument it doesn't recognize as a flag is treated as (part of)
		// that path.
		std::string launchOptions = "/app " + game.canonicalName + " " + Quote(exePath);
		if (!game.launchParameters.empty())
			launchOptions += " " + game.launchParameters;

		// Preserve whatever appid Steam already assigned (it stays 0 until
		// Steam itself loads a fresh entry and computes one - see
		// ReadAssignedAppIds) rather than resetting it to 0 on every sync,
		// which would silently orphan any Library grid art already saved
		// under the previously-assigned id.
		if (!entry.Find("appid"))
			entry["appid"] = VdfNode((int32_t)0);
		entry["AppName"] = VdfNode(game.title);
		entry["Exe"] = VdfNode(Quote(injectorPath));
		entry["StartDir"] = VdfNode(Quote(startDir));
		entry["icon"] = VdfNode(ResolveIcon(game));
		entry["ShortcutPath"] = VdfNode(std::string());
		entry["LaunchOptions"] = VdfNode(launchOptions);
		entry["AllowDesktopConfig"] = VdfNode(true);
		// Off, not the usual true: Steam's own overlay injects into the
		// launched process the same way ReviveInjector.exe's Detours-based
		// DLL injection does (briefly suspend, inject, resume) - two
		// independent injectors racing to do that to the same freshly
		// created process is a real suspect for the Steam-side stall seen
		// after quitting a game (Steam's own process-tracking thread going
		// completely silent, not just missing this one app's exit - see
		// AGENTS.md-adjacent notes/commit history around this). If turning
		// it off stops the stall, this stays off; if not, revisit.
		entry["AllowOverlay"] = VdfNode(false);
		entry["OpenVR"] = VdfNode(true);
		if (!entry.Find("LastPlayTime"))
			entry["LastPlayTime"] = VdfNode((int32_t)0);
		SetTags(entry, { kOwnerTag, kIdTagPrefix + game.canonicalName });
	}
}

std::optional<std::string> SteamShortcuts::FindShortcutsPath()
{
	auto steamPath = RegReadSZ(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
	if (!steamPath)
		steamPath = RegReadSZ(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
	if (!steamPath)
		return std::nullopt;

	auto userId = FindActiveUserId(*steamPath);
	if (!userId)
		return std::nullopt;

	return (fs::path(*steamPath) / "userdata" / *userId / "config" / "shortcuts.vdf").string();
}

bool SteamShortcuts::Sync(const std::vector<MetaGame>& games, const std::string& injectorPath,
	const std::string& shortcutsPath)
{
	VdfNode root;
	if (auto bytes = ReadFile(shortcutsPath); !bytes.empty())
	{
		auto parsed = VdfNode::Parse(bytes);
		if (!parsed)
			return false; // malformed existing file - don't risk overwriting it blind
		root = std::move(*parsed);
	}

	// Index existing entries this tool previously added, by canonicalName.
	std::unordered_map<std::string, VdfNode*> existingByName;
	std::vector<std::string> allKeys;
	for (auto& [key, entry] : root.AsObject())
	{
		allKeys.push_back(key);
		if (auto name = CanonicalNameFor(entry))
			existingByName[*name] = &entry;
	}

	std::unordered_set<std::string> wantedNames;
	for (auto& game : games)
		wantedNames.insert(game.canonicalName);

	// Update in place, or collect entries to add.
	std::vector<VdfNode> toAdd;
	for (auto& game : games)
	{
		if (auto it = existingByName.find(game.canonicalName); it != existingByName.end())
			FillEntry(*it->second, game, injectorPath);
		else
		{
			VdfNode entry;
			FillEntry(entry, game, injectorPath);
			toAdd.push_back(std::move(entry));
		}
	}

	// Drop entries this tool owns that are no longer in `games`; leave
	// everything else (the user's own shortcuts) untouched.
	VdfNode::Object kept;
	for (auto& [key, entry] : root.AsObject())
	{
		auto name = CanonicalNameFor(entry);
		bool ownedByUs = name.has_value();
		bool stillWanted = ownedByUs && wantedNames.count(*name) != 0;
		if (!ownedByUs || stillWanted)
			kept[key] = std::move(entry);
	}

	// Re-key everything 0..N-1, as Steam itself does on every save.
	VdfNode::Object final_;
	int index = 0;
	for (auto& [_, entry] : kept)
		final_[std::to_string(index++)] = std::move(entry);
	for (auto& entry : toAdd)
		final_[std::to_string(index++)] = std::move(entry);

	root = VdfNode(std::move(final_));

	fs::create_directories(fs::path(shortcutsPath).parent_path());
	return WriteFile(shortcutsPath, root.Serialize());
}

std::unordered_map<std::string, uint32_t> SteamShortcuts::ReadAssignedAppIds(const std::string& shortcutsPath)
{
	std::unordered_map<std::string, uint32_t> result;

	auto bytes = ReadFile(shortcutsPath);
	if (bytes.empty())
		return result;

	auto parsed = VdfNode::Parse(bytes);
	if (!parsed)
		return result;

	for (auto& [key, entry] : parsed->AsObject())
	{
		auto canonicalName = CanonicalNameFor(entry);
		if (!canonicalName)
			continue;

		auto* appidNode = entry.Find("appid");
		uint32_t appid = appidNode ? (uint32_t)appidNode->AsInt() : 0;
		if (appid != 0)
			result[*canonicalName] = appid;
	}
	return result;
}
