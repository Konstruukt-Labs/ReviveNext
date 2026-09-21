#include "GameDiscovery.h"
#include "GridArtSync.h"
#include "SteamShortcuts.h"
#include "TitleOverrides.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>

namespace
{
	// Double-clicking a console app from Explorer spawns a brand-new
	// console just for it, which Windows closes the instant the process
	// exits - any printed output (including error messages) flashes by
	// unreadably fast. Running from an already-open terminal doesn't have
	// this problem, since that console stays open regardless. Detect the
	// former case (we're the only process attached to this console) and
	// pause so double-clicking is actually usable.
	void PauseIfLaunchedByDoubleClick()
	{
		DWORD processList[2];
		if (GetConsoleProcessList(processList, 2) <= 1)
		{
			printf("\nPress Enter to close this window...");
			fflush(stdout);
			getchar();
		}
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

		printf("Found %zu game(s).\n", games.size());
		for (auto& game : games)
			printf("  %s\n", game.title.c_str());

		if (games.empty())
		{
			printf("Nothing to sync.\n");
			return 0;
		}

		auto shortcutsPath = SteamShortcuts::FindShortcutsPath();
		if (!shortcutsPath)
		{
			fprintf(stderr, "Could not locate Steam's shortcuts.vdf (is Steam installed and has it been run at least once?).\n");
			return 1;
		}
		printf("Syncing to: %s\n", shortcutsPath->c_str());

		// The actual launch target is Revive's ReviveInjector.exe, bundled
		// alongside this exe (see runtime/ and CMakeLists.txt) - it does the
		// real Oculus SDK -> OpenVR/OpenXR translation. ReviveNext itself is
		// never what Steam launches.
		wchar_t selfPathW[MAX_PATH];
		GetModuleFileNameW(nullptr, selfPathW, MAX_PATH);
		int len = WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, nullptr, 0, nullptr, nullptr);
		std::string selfPath(len - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, selfPathW, -1, selfPath.data(), len, nullptr, nullptr);

		std::string injectorPath = selfPath;
		if (auto pos = injectorPath.find_last_of("\\/"); pos != std::string::npos)
			injectorPath = injectorPath.substr(0, pos + 1) + "ReviveInjector.exe";

		if (!std::filesystem::exists(injectorPath))
		{
			fprintf(stderr, "ReviveInjector.exe not found next to ReviveNext.exe (expected: %s).\n", injectorPath.c_str());
			return 1;
		}

		if (!SteamShortcuts::Sync(games, injectorPath, *shortcutsPath))
		{
			fprintf(stderr, "Failed to write shortcuts.vdf.\n");
			return 1;
		}

		printf("Done. Restart Steam for changes to take effect.\n");

		std::string gridDir = (std::filesystem::path(*shortcutsPath).parent_path() / "grid").string();
		GridArtSync::Run(games, injectorPath, *shortcutsPath, gridDir);

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
int main()
{
	int result = RunMain();
	PauseIfLaunchedByDoubleClick();
	return result;
}
