#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "GameDiscovery.h"

// Native Win32 window for matching each game against SteamGridDB and
// picking its Library grid art by hand. Shows the game's local Meta
// Horizon cover art (so an ambiguous canonicalName like
// "insomniac-games-i-28" is actually identifiable) side by side with a
// searchable, clickable grid of SteamGridDB candidate thumbnails - text
// alone (as a console prompt would show) isn't enough to confirm a match.
class ArtMatcherGui
{
public:
	// appIds maps canonicalName -> the appid Steam itself assigned that
	// shortcut (see SteamShortcuts::ReadAssignedAppIds) - that's what
	// names the file this writes into gridDir (<appid>p.<ext>). A game
	// missing from appIds (Steam hasn't loaded the synced shortcuts.vdf
	// yet) is skipped with an explanatory message rather than guessed at.
	//
	// injectorPath/shortcutsPath are only used for the final title
	// re-sync (see below) - matching itself doesn't touch shortcuts.vdf.
	//
	// Blocks (runs its own message loop) until every game is matched or
	// skipped, or the window is closed. If anything was matched, the
	// window then walks the user through fully closing Steam and, once
	// it detects Steam has closed, re-syncs shortcuts.vdf itself with the
	// picked titles (reading ALL current title overrides, not just this
	// session's, so it never regresses an earlier session's picks) before
	// letting the window close - no separate ReviveNext re-run needed.
	//
	// Returns the games matched during this run.
	static std::vector<MetaGame> Run(const std::vector<MetaGame>& games,
		const std::unordered_map<std::string, uint32_t>& appIds,
		const std::string& injectorPath, const std::string& shortcutsPath,
		const std::string& gridDir, const std::string& apiKey);

	// Shows a small window asking for a SteamGridDB API key (entered
	// masked, like a password field), with instructions and a clickable
	// link to generate one. Saves it to
	// HKCU\Software\ReviveNext\SteamGridDbApiKey and returns it if the
	// user saves; nullopt if they close the window without saving.
	static std::optional<std::string> PromptForApiKey();
};
