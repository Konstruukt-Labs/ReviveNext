#pragma once

#include <optional>
#include <string>
#include <vector>

#include "GameDiscovery.h"

// Native Win32 window for reviewing every discovered game and matching it
// against SteamGridDB for Library art - both the portrait cover (600x900)
// and the widescreen art (920x430). A single search covers both: picking a
// game shows candidate thumbnails for each kind side by side, and either
// (or both) can be picked in the same step - both current picks stay
// visible at once, no toggle needed to see or change one versus the other.
// Shows the game's local Meta Horizon cover art (so an ambiguous
// canonicalName like "insomniac-games-i-28" is actually identifiable)
// alongside those candidates - text alone (as a console prompt would show)
// isn't enough to confirm a match.
class ArtMatcherGui
{
public:
	// artDir is ReviveNext's own art storage: matched art is saved as
	// <artDir>/<canonicalName><ext> (widescreen) and
	// <artDir>/<canonicalName>p<ext> (portrait) - keyed by the game, not
	// by any Steam-assigned id, so it works without Steam Library
	// shortcuts and the same files feed the SteamVR application
	// registration's image_path.
	//
	// apiKey is optional - without one, search is disabled (with a note
	// pointing at the Settings button), but the window still opens so
	// Settings/navigation stay reachable.
	//
	// Blocks (runs its own message loop) until the window is closed.
	// Everything picked is written immediately (art files + persisted
	// title overrides). Returns the games matched during this run.
	static std::vector<MetaGame> Run(const std::vector<MetaGame>& games,
		const std::string& injectorPath, const std::string& artDir,
		const std::optional<std::string>& apiKey);

	// Shows a small window asking for a SteamGridDB API key (entered
	// masked, like a password field), with instructions and a clickable
	// link to generate one. Saves it to
	// HKCU\Software\ReviveNext\SteamGridDbApiKey and returns it if the
	// user saves; nullopt if they close the window without saving.
	static std::optional<std::string> PromptForApiKey();
};
