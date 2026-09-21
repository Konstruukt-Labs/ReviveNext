#include "GameDiscovery.h"
#include "GridArtSync.h"
#include "SteamShortcuts.h"
#include "TitleOverrides.h"

#include <windows.h>
#include <filesystem>
#include <unordered_map>

namespace
{
	void ShowError(const std::wstring& message)
	{
		MessageBoxW(nullptr, message.c_str(), L"ReviveNext", MB_ICONERROR);
	}

	// Revive's InputManager::LoadActionManifest() (compiled into the
	// vendored LibRevive64.dll, unchanged) normally finds its
	// action_manifest.json via a registry key Revive's own installer
	// writes (HKLM\Software\Revive) - since ReviveNext doesn't require
	// Revive to be installed at all, that key may not exist, which shows
	// up at launch as "Failed to load action manifest, input will not
	// function correctly" (SteamVR loads, but controllers do nothing).
	//
	// LibRevive64.dll checks the REVIVE_ACTION_MANIFEST environment
	// variable before ever touching the registry, so setting it here -
	// persistently, at the user level, so it's inherited by every process
	// tree Steam (and anything else) launches from now on, Steam included
	// once restarted - covers this with no extra process in the launch
	// chain. (A wrapper .exe that set this only for its own child was
	// tried first and worked, but added a process hop that measurably
	// changed SteamVR's dashboard behavior during the game's startup
	// compared to a native Steam VR title, confirmed by the user
	// side-by-side - not worth that cost for what a one-time environment
	// variable solves just as well.)
	//
	// Broadcasting WM_SETTINGCHANGE only refreshes processes like Explorer
	// that watch for it live; an already-running Steam won't see this
	// until it's restarted, same as any other environment variable change
	// - worth a one-time popup since it's easy to miss otherwise and the
	// symptom (controllers doing nothing in VR) isn't obviously related.
	void EnsureActionManifestEnvVar(const std::string& runtimeDir)
	{
		std::wstring manifestPath = std::filesystem::path(runtimeDir + "Input\\action_manifest.json").wstring();

		HKEY key;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
			return;

		wchar_t existing[MAX_PATH] = {};
		DWORD size = sizeof(existing);
		DWORD type = 0;
		LONG readResult = RegQueryValueExW(key, L"REVIVE_ACTION_MANIFEST", nullptr, &type, (LPBYTE)existing, &size);

		if (readResult != ERROR_SUCCESS || type != REG_SZ || manifestPath != existing)
		{
			RegSetValueExW(key, L"REVIVE_ACTION_MANIFEST", 0, REG_SZ, (const BYTE*)manifestPath.c_str(),
				(DWORD)((manifestPath.size() + 1) * sizeof(wchar_t)));
			DWORD_PTR result;
			SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
				SMTO_ABORTIFHUNG, 5000, &result);
			MessageBoxW(nullptr,
				L"ReviveNext just configured a setting VR game controllers need.\n\n"
				L"Restart Steam once for it to take effect.",
				L"ReviveNext", MB_ICONINFORMATION);
		}

		RegCloseKey(key);
	}

	int RunMain()
	{
		// IconConverter uses WIC, which needs COM initialized on this thread.
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		auto games = GameDiscovery::ScanGames();

		// Apply any plain-English titles picked in a previous Library-art
		// matching session - independent of shortcuts.vdf, so this holds even
		// if that session never got a chance to write it back itself.
		auto titleOverrides = TitleOverrides::Load();
		for (auto& game : games)
			if (auto it = titleOverrides.find(game.canonicalName); it != titleOverrides.end())
				game.title = it->second;

		// No console to list them in - the review window's own counter
		// ("N / M") and per-game title already show what was found, and
		// that's the only place they'd otherwise need reading.
		if (games.empty())
		{
			MessageBoxW(nullptr, L"No installed Meta Horizon (Oculus Rift-platform) games found.", L"ReviveNext", MB_ICONINFORMATION);
			return 0;
		}

		auto shortcutsPath = SteamShortcuts::FindShortcutsPath();
		if (!shortcutsPath)
		{
			ShowError(L"Could not locate Steam's shortcuts.vdf (is Steam installed and has it been run at least once?).");
			return 1;
		}

		// The actual launch target is Revive's own ReviveInjector.exe,
		// bundled alongside this exe (see runtime/ and CMakeLists.txt) -
		// it does the real Oculus SDK -> OpenVR/OpenXR translation.
		// ReviveNext itself is never what Steam launches.
		wchar_t selfPathW[MAX_PATH];
		GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
		int len = WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, nullptr, 0, nullptr, nullptr);
		std::string selfPath(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, selfPath.data(), len, nullptr, nullptr);

		std::string selfDir = selfPath;
		if (auto pos = selfDir.find_last_of("\\/"); pos != std::string::npos)
			selfDir = selfDir.substr(0, pos + 1);

		std::string injectorPath = selfDir + "ReviveInjector.exe";

		if (!std::filesystem::exists(injectorPath))
		{
			ShowError(L"ReviveInjector.exe not found next to ReviveNext.exe.");
			return 1;
		}

		EnsureActionManifestEnvVar(selfDir);

		// Sync() already sees every entry's Steam-assigned appid while
		// writing shortcuts.vdf back out - handing it here saves
		// GridArtSync a second full read+parse of the same file to
		// recover the same data immediately after.
		std::unordered_map<std::string, uint32_t> appIds;
		if (!SteamShortcuts::Sync(games, injectorPath, *shortcutsPath, &appIds))
		{
			ShowError(L"Failed to write shortcuts.vdf.");
			return 1;
		}

		std::string gridDir = (std::filesystem::path(*shortcutsPath).parent_path() / "grid").string();
		GridArtSync::Run(games, appIds, injectorPath, *shortcutsPath, gridDir);

		return 0;
	}
}

// ReviveNext: finds installed Meta Horizon (Oculus Rift-platform) games and
// registers each as a Steam non-Steam-game shortcut with the OpenVR flag
// set, so they show up natively in SteamVR's own Library alongside real
// Steam VR titles - no custom dashboard/overlay UI of its own. Run this
// (manually, or however it ends up being triggered - see AGENTS.md/README
// for the current state of that decision) whenever the installed game list
// might have changed.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	return RunMain();
}
