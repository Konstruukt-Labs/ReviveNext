#pragma once

#include <string>
#include <vector>

#include "GameDiscovery.h"

// Entry point for the Library grid-art matching step: reads the
// SteamGridDB API key setting, and if present, hands off to the
// interactive matcher (ArtMatcherGui).
class GridArtSync
{
public:
	// Blocks until the user finishes matching. If anything was matched,
	// the window itself walks the user through closing Steam and applies
	// the picked titles to shortcuts.vdf automatically before returning -
	// no separate ReviveNext re-run needed. Returns the games that were
	// matched. Empty if no API key is configured, or nothing was matched.
	static std::vector<MetaGame> Run(const std::vector<MetaGame>& games, const std::string& injectorPath,
		const std::string& shortcutsPath, const std::string& gridDir);
};
