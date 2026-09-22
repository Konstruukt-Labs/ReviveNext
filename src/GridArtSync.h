#pragma once

#include <string>
#include <vector>

#include "GameDiscovery.h"

// Entry point for the art review window (ArtMatcherGui): reads the
// SteamGridDB API key setting and hands off to it. The window itself
// covers art matching for every discovered game, so it opens regardless
// of whether a key ends up configured - art search is just unavailable
// without one.
class GridArtSync
{
public:
	// artDir is ReviveNext's own art storage (see ArtMatcherGui::Run).
	// Before opening the window, any art still sitting in Steam's grid
	// dir under the old shortcut-appid naming (from when ReviveNext
	// still synced Steam Library shortcuts) is migrated into artDir
	// under its per-game names, using the appid mapping recorded in the
	// SteamVR registration manifest written back then.
	//
	// Blocks until the user closes the review window; everything picked
	// is written immediately. Returns the games that were matched.
	static std::vector<MetaGame> Run(const std::vector<MetaGame>& games,
		const std::string& injectorPath, const std::string& artDir);
};
