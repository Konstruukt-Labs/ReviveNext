#include "AppData.h"
#include "GameDiscovery.h"
#include "GridArtSync.h"
#include "SteamVrAppRegistration.h"
#include "TitleOverrides.h"

#include <windows.h>
#include <shlobj.h>
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
	void EnsureActionManifestEnvVar(const std::string& runtimeDir, bool quiet = false)
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
			if (!quiet)
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

		// The actual launch target is Revive's own ReviveInjector.exe,
		// bundled alongside this exe (see CMakeLists.txt) - it does the real
		// Oculus SDK -> OpenVR/OpenXR translation, and it's what the
		// SteamVR-registered application entries launch.
		wchar_t selfPathW[MAX_PATH];
		GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
		int len = WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, nullptr, 0, nullptr, nullptr);
		std::string selfPath(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, selfPath.data(), len, nullptr, nullptr);

		std::string selfDir = selfPath;
		if (auto pos = selfDir.find_last_of("\\/"); pos != std::string::npos)
			selfDir = selfDir.substr(0, pos + 1);

		// Everything except the exe itself lives under Resources/ (see
		// CMakeLists.txt - the installer ships the same layout).
		std::string resourcesDir = selfDir + "Resources";
		std::string injectorPath = resourcesDir + "\\ReviveInjector.exe";

		if (!std::filesystem::exists(injectorPath))
		{
			ShowError(L"Resources\\ReviveInjector.exe not found next to ReviveNext.exe.");
			return 1;
		}

		EnsureActionManifestEnvVar(resourcesDir);

		// ReviveNext's own art storage (see ArtMatcherGui::Run) - per-user
		// under %LOCALAPPDATA%, so an installed copy never needs elevation,
		// and it carries over anything previously matched under Steam's
		// shortcut naming (see GridArtSync::Run).
		auto artDirOpt = AppData::SubDir("Art");
		if (!artDirOpt)
		{
			ShowError(L"Could not create the art storage folder under %LOCALAPPDATA%.");
			return 1;
		}
		std::string artDir = *artDirOpt;
		GridArtSync::Run(games, injectorPath, artDir);

		// Register each game as a visible SteamVR application last, so it
		// uses the art GridArtSync just wrote/matched and any titles picked
		// in that session - launching from SteamVR's own Library then gets
		// SteamVR's app-launch flow end to end. See SteamVrAppRegistration.h.
		// Needs SteamVR running; silently skipped otherwise (entries persist
		// in SteamVR's storage once registered).
		titleOverrides = TitleOverrides::Load();
		for (auto& game : games)
			if (auto it = titleOverrides.find(game.canonicalName); it != titleOverrides.end())
				game.title = it->second;
		SteamVrAppRegistration::Sync(games, injectorPath, artDir);

		return 0;
	}
}

// ReviveNext: finds installed Meta Horizon (Oculus Rift-platform) games,
// lets their SteamVR Library art be matched against SteamGridDB, and
// registers each as a SteamVR application so they appear - and launch -
// natively in SteamVR's own Library. Run this whenever the installed game
// list might have changed.
//
// /autoregister is the headless mode SteamVR itself triggers: ReviveNext
// registers its own application entry with those arguments and marks it
// auto-launch, so SteamVR starts a copy of this exe (hidden, is_internal)
// every time SteamVR itself starts. That copy rescans the games and
// refreshes the registration without any UI, keeping SteamVR's Library
// entries current with no resident process and nothing for the user to
// remember. Interactive runs (no arguments) behave exactly as before.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int)
{
	// openvr_api.dll is delay-loaded and lives under Resources/ - pull it
	// in explicitly before the first OpenVR call, or the delay-load helper
	// would go looking beside the exe, where it does not exist.
	{
		wchar_t selfPathW[MAX_PATH];
		GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
		if (wchar_t* slash = wcsrchr(selfPathW, L'\\'))
			*slash = L'\0';
		LoadLibraryW((std::wstring(selfPathW) + L"\\Resources\\openvr_api.dll").c_str());
	}

	if (cmdLine && wcsstr(cmdLine, L"/unregister"))
	{
		wchar_t selfPathW[MAX_PATH];
		GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
		int len = WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, nullptr, 0, nullptr, nullptr);
		std::string selfPath(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, selfPath.data(), len, nullptr, nullptr);
		std::string selfDir = selfPath;
		if (auto pos = selfDir.find_last_of("\\/"); pos != std::string::npos)
			selfDir = selfDir.substr(0, pos + 1);

		SteamVrAppRegistration::Unregister(selfDir + "Resources\\ReviveInjector.exe");
		return 0;
	}

	if (cmdLine && wcsstr(cmdLine, L"/autoregister"))
	{
		// Log to %LOCALAPPDATA%\ReviveNext\autoregister.log so the
		// SteamVR-triggered runs (invisible by nature) leave a trace.
		auto games = GameDiscovery::ScanGames();

		auto titleOverrides = TitleOverrides::Load();
		for (auto& game : games)
			if (auto it = titleOverrides.find(game.canonicalName); it != titleOverrides.end())
				game.title = it->second;

		wchar_t selfPathW[MAX_PATH];
		GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
		int len = WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, nullptr, 0, nullptr, nullptr);
		std::string selfPath(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, selfPath.data(), len, nullptr, nullptr);

		std::string selfDir = selfPath;
		if (auto pos = selfDir.find_last_of("\\/"); pos != std::string::npos)
			selfDir = selfDir.substr(0, pos + 1);

		std::string logLine;
		std::string resourcesDir = selfDir + "Resources";
		if (games.empty() || !std::filesystem::exists(resourcesDir + "\\ReviveInjector.exe"))
		{
			logLine = games.empty() ? "no games found" : "ReviveInjector.exe missing";
		}
		else
		{
			if (auto artDir = AppData::SubDir("Art"))
			{
				EnsureActionManifestEnvVar(resourcesDir, /*quiet=*/true);
				SteamVrAppRegistration::Sync(games, resourcesDir + "\\ReviveInjector.exe", *artDir, /*quiet=*/true);
			}
			else
			{
				logLine = "could not create art storage";
			}
			logLine = "registered " + std::to_string(games.size()) + " games";
		}

		if (wchar_t* localAppData = nullptr; SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData) == S_OK)
		{
			std::filesystem::path logDir = std::filesystem::path(localAppData) / "ReviveNext";
			std::error_code ec;
			std::filesystem::create_directories(logDir, ec);
			if (FILE* log = _wfopen((logDir / L"autoregister.log").c_str(), L"a"))
			{
				SYSTEMTIME st = {};
			GetLocalTime(&st);
				fprintf(log, "%04u-%02u-%02u %02u:%02u:%02u  %s\n",
					st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, logLine.c_str());
				fclose(log);
			}
			CoTaskMemFree(localAppData);
		}
		return 0;
	}

	return RunMain();
}
