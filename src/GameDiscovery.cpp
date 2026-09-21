#include "GameDiscovery.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <regex>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
	std::wstring Utf8ToWide(const std::string& s)
	{
		if (s.empty())
			return {};
		int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring result(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), len);
		return result;
	}

	std::string WideToUtf8(const std::wstring& s)
	{
		if (s.empty())
			return {};
		int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
		std::string result(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), len, nullptr, nullptr);
		return result;
	}

	// Reads a REG_SZ value, returning nullopt if the key/value is missing,
	// of the wrong type, or empty.
	std::optional<std::string> RegReadSZ(HKEY root, const std::wstring& subkey, const std::wstring& value, REGSAM flags = 0)
	{
		HKEY key;
		if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | flags, &key) != ERROR_SUCCESS)
			return std::nullopt;

		DWORD type = 0, size = 0;
		if (RegQueryValueExW(key, value.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0)
		{
			RegCloseKey(key);
			return std::nullopt;
		}

		std::wstring result(size / sizeof(wchar_t), L'\0');
		RegQueryValueExW(key, value.c_str(), nullptr, nullptr, (LPBYTE)result.data(), &size);
		RegCloseKey(key);

		while (!result.empty() && result.back() == L'\0')
			result.pop_back();

		return result.empty() ? std::nullopt : std::make_optional(WideToUtf8(result));
	}

	// Resolves a library's \\?\Volume{GUID}\rest\of\path Path value to a
	// real drive-letter path, e.g. "D:\OculusSoftware".
	std::optional<std::string> ResolveLibraryVolumePath(const std::wstring& volumePath)
	{
		// The volume name portion is exactly "\\?\Volume{GUID}\" - 49 characters.
		if (volumePath.size() < 49)
			return std::nullopt;

		std::wstring volumeName = volumePath.substr(0, 49);
		wchar_t mountPoint[MAX_PATH] = {};
		DWORD returnLength = 0;
		if (!GetVolumePathNamesForVolumeNameW(volumeName.c_str(), mountPoint, MAX_PATH, &returnLength) || mountPoint[0] == L'\0')
			return std::nullopt;

		std::wstring result = mountPoint;
		result += volumePath.substr(49);
		return WideToUtf8(result);
	}

	std::vector<std::string> GetRegisteredLibraryPaths()
	{
		std::vector<std::string> paths;

		HKEY librariesKey;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Oculus VR, LLC\\Oculus\\Libraries", 0, KEY_READ, &librariesKey) != ERROR_SUCCESS)
			return paths;

		DWORD subkeyCount = 0;
		RegQueryInfoKeyW(librariesKey, nullptr, nullptr, nullptr, &subkeyCount, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

		for (DWORD i = 0; i < subkeyCount; ++i)
		{
			wchar_t guid[256] = {};
			DWORD guidLen = 256;
			if (RegEnumKeyExW(librariesKey, i, guid, &guidLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
				continue;

			auto rawPath = RegReadSZ(HKEY_CURRENT_USER, L"Software\\Oculus VR, LLC\\Oculus\\Libraries\\" + std::wstring(guid), L"Path");
			if (!rawPath)
				continue;

			if (auto resolved = ResolveLibraryVolumePath(Utf8ToWide(*rawPath)))
				paths.push_back(*resolved);
		}

		RegCloseKey(librariesKey);
		return paths;
	}

	std::string TrimTrailingSlash(std::string path)
	{
		while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
			path.pop_back();
		return path;
	}

	// nlohmann::json's own .value(key, fallback) throws json::type_error
	// (not something a missing-key fallback would suggest) whenever the key
	// IS present but holds JSON null or another type than requested - and
	// real Oculus manifests do this routinely (e.g. "launchParameters":
	// null instead of the key being omitted or ""; every manifest on this
	// machine has it, and it was silently dropping most real games until
	// this was added). Treat null/wrong-type the same as "absent" instead.
	std::string ManifestString(const json& manifest, const char* key, std::string fallback = "")
	{
		if (!manifest.contains(key) || !manifest[key].is_string())
			return fallback;
		return manifest[key].get<std::string>();
	}

	bool ManifestBool(const json& manifest, const char* key, bool fallback = false)
	{
		if (!manifest.contains(key) || !manifest[key].is_boolean())
			return fallback;
		return manifest[key].get<bool>();
	}

	// Unreal Engine games' manifest "launchFile" often points at a
	// non-functional shim; the real executable lives under
	// Binaries\Win64\, preferring one ending in -Shipping.exe if present.
	// Ported from Oculus.js's generateManifest().
	std::string ResolveLaunchFile(const json& manifest)
	{
		std::string launch = ManifestString(manifest, "launchFile");
		if (!manifest.contains("files") || !manifest["files"].is_object())
			return launch;

		static const std::regex shipping(R"(-Shipping\.exe$)", std::regex::icase);
		static const std::regex binaries(R"(Binaries[\\/]Win64[\\/](.*)\.exe)", std::regex::icase);

		for (auto& [file, _] : manifest["files"].items())
		{
			if (std::regex_search(file, binaries))
			{
				launch = file;
				if (std::regex_search(file, shipping))
					break;
			}
		}

		// Manifests use forward slashes; launch args need Windows-style paths.
		std::replace(launch.begin(), launch.end(), '/', '\\');
		return launch;
	}
}

std::optional<std::string> GameDiscovery::GetBasePath()
{
	if (auto override = RegReadSZ(HKEY_CURRENT_USER, L"Software\\ReviveNext", L"MetaHorizonPathOverride"))
	{
		if (fs::is_directory(*override))
			return TrimTrailingSlash(*override);
	}

	// The Base value is written by the 32-bit installer, so it lives under
	// the WOW6432Node view on 64-bit Windows.
	if (auto base = RegReadSZ(HKEY_LOCAL_MACHINE, L"Software\\Oculus VR, LLC\\Oculus", L"Base", KEY_WOW64_32KEY))
	{
		if (fs::is_directory(*base))
			return TrimTrailingSlash(*base);
	}

	return std::nullopt;
}

std::vector<std::string> GameDiscovery::GetLibraryPaths()
{
	if (auto override = RegReadSZ(HKEY_CURRENT_USER, L"Software\\ReviveNext", L"GamesLibraryPathOverride"))
	{
		if (fs::is_directory(*override))
			return { TrimTrailingSlash(*override) };
	}

	auto paths = GetRegisteredLibraryPaths();
	for (auto& path : paths)
		path = TrimTrailingSlash(path);
	return paths;
}

std::vector<MetaGame> GameDiscovery::ScanGames()
{
	std::vector<MetaGame> games;
	auto basePath = GetBasePath();

	for (const std::string& libraryPath : GetLibraryPaths())
	{
		fs::path manifestsDir = fs::path(libraryPath) / "Manifests";
		if (!fs::is_directory(manifestsDir))
			continue;

		for (const auto& entry : fs::directory_iterator(manifestsDir))
		{
			if (entry.path().extension() != ".json")
				continue;

			std::ifstream file(entry.path(), std::ios::binary);
			if (!file)
				continue;

			// Malformed JSON (truncated file, etc.) is the only thing this
			// needs to guard against now that field reads go through
			// ManifestString/ManifestBool - those never throw on a
			// null/wrong-typed field, just fall back.
			try
			{
				json manifest;
				file >> manifest;

				// Only real, installable games - not Oculus's own
				// core/system components (Home, Dash, the Touch tutorial,
				// etc.) and not third-party (non-Rift-platform) entries.
				if (ManifestString(manifest, "packageType") != "APP")
					continue;
				if (ManifestBool(manifest, "isCore") || ManifestBool(manifest, "thirdParty"))
					continue;

				MetaGame game;
				game.canonicalName = ManifestString(manifest, "canonicalName");
				game.appId = ManifestString(manifest, "appId");
				game.title = game.canonicalName;
				game.launchFile = ResolveLaunchFile(manifest);
				game.launchParameters = ManifestString(manifest, "launchParameters");
				if (game.launchParameters == "None")
					game.launchParameters.clear();
				game.libraryPath = libraryPath;

				if (basePath)
				{
					fs::path coverArt = fs::path(*basePath) / "CoreData" / "Software" / "StoreAssets"
						/ (game.canonicalName + "_assets") / "cover_square_image.jpg";
					if (fs::exists(coverArt))
						game.coverArtPath = coverArt.string();
				}

				if (!game.canonicalName.empty() && !game.launchFile.empty())
					games.push_back(std::move(game));
			}
			catch (const json::exception&)
			{
				continue;
			}
		}
	}

	return games;
}
