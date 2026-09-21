#include "GridArtSync.h"
#include "ArtMatcherGui.h"
#include "SteamShortcuts.h"

#include <windows.h>

#include <cstdio>
#include <optional>
#include <string>

namespace
{
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
}

std::vector<MetaGame> GridArtSync::Run(const std::vector<MetaGame>& games, const std::string& injectorPath,
	const std::string& shortcutsPath, const std::string& gridDir)
{
	auto apiKey = RegReadSZ(HKEY_CURRENT_USER, L"Software\\ReviveNext", L"SteamGridDbApiKey");
	if (!apiKey)
	{
		printf("No SteamGridDB API key configured - asking for one.\n");
		apiKey = ArtMatcherGui::PromptForApiKey();
		if (!apiKey)
		{
			printf("No key entered - skipping Library grid art.\n");
			return {};
		}
	}

	auto appIds = SteamShortcuts::ReadAssignedAppIds(shortcutsPath);
	return ArtMatcherGui::Run(games, appIds, injectorPath, shortcutsPath, gridDir, *apiKey);
}
