#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "GameDiscovery.h"

// Entry point for the post-sync review window (ArtMatcherGui): reads the
// SteamGridDB API key setting (prompting for one if it's never been set)
// and hands off to it. The window itself covers both Library art matching
// and removing/re-adding games to the Library, so it opens regardless of
// whether a key ends up configured - art search is just unavailable
// without one.
class GridArtSync
{
public:
	// appIds is whatever the caller's just-completed SteamShortcuts::Sync
	// already knows (see its outAppIds parameter) - reused as-is rather
	// than this re-reading and re-parsing shortcuts.vdf right after Sync
	// wrote it, just to recover the same data.
	//
	// Blocks until the user closes the review window. If anything was
	// matched, the window itself walks the user through closing Steam and
	// applies the picked titles to shortcuts.vdf automatically before
	// returning - no separate ReviveNext re-run needed. Returns the games
	// that were matched (not those only included/excluded).
	static std::vector<MetaGame> Run(const std::vector<MetaGame>& games,
		const std::unordered_map<std::string, uint32_t>& appIds,
		const std::string& injectorPath, const std::string& shortcutsPath, const std::string& gridDir);
};
