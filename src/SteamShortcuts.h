#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "GameDiscovery.h"

// Registers/unregisters MetaGames as Steam non-Steam-game shortcuts with
// the OpenVR ("VR Library") flag set, so they appear in SteamVR's native
// Library alongside real Steam VR titles, per shortcuts.vdf's documented
// schema (https://github.com/CorporalQuesadilla/Steam-Shortcut-Manager/wiki).
//
// Every entry this tool writes is tagged "ReviveNext" so a later sync can
// tell which entries it owns and update or remove them without touching
// anything the user added themselves.
class SteamShortcuts
{
public:
	// Locates the current Steam user's shortcuts.vdf
	// (<Steam>/userdata/<accountId>/config/shortcuts.vdf). Returns nullopt
	// if Steam's install path or the active user can't be determined.
	static std::optional<std::string> FindShortcutsPath();

	// injectorPath is what "Exe" launches (quoted automatically) - Revive's
	// own ReviveInjector.exe, which does the actual Oculus SDK ->
	// OpenVR/OpenXR translation; ReviveNext only decides which games exist
	// and what to pass it (see Main.cpp's EnsureActionManifestEnvVar for
	// how it points ReviveInjector.exe's action manifest lookup at our own
	// vendored copy without needing Revive installed). Adds a new entry
	// (tagged "ReviveNext" and "ReviveNext:<canonicalName>", the latter
	// used to match entries back up across syncs without depending on
	// LaunchOptions staying unchanged) for each game not already present,
	// updates existing ones in place if the launch target changed, and
	// removes any previously-added ReviveNext entry for a game no longer in
	// `games` (i.e. actually uninstalled).
	//
	// A GUI-driven "remove/hide a still-installed game from the Library"
	// feature was tried and removed: outright deleting the entry makes
	// Steam permanently refuse to ever assign that shortcut identity a
	// fresh appid again (confirmed empirically - not even a Steam restart,
	// directly clicking it, or Manage -> Set Custom Artwork unstuck it),
	// and shortcuts.vdf's IsHidden field turned out not to actually hide an
	// already-shown shortcut from Steam's Library view at all (that's
	// controlled by a per-client, LevelDB-backed collection Steam's own UI
	// owns, not anything in this file) - so IsHidden was a no-op dead end.
	// Uninstalling the game in Meta Horizon (dropping it from `games`) is
	// the only reliable way to remove one from the Library.
	//
	// Returns false if the file couldn't be read/written.
	static bool Sync(const std::vector<MetaGame>& games, const std::string& injectorPath,
		const std::string& shortcutsPath);

	// Reads back the "appid" Steam itself computed and persisted for each
	// ReviveNext-owned entry, keyed by canonicalName. Steam assigns this
	// (a 32-bit value used to name this shortcut's Library grid art files)
	// the first time it loads a shortcut whose appid is 0 - not something
	// ReviveNext computes itself, since Steam's exact algorithm for it is
	// undocumented. Entries Steam hasn't processed yet (still 0, e.g.
	// Steam hasn't been (re)started since the last sync) are omitted.
	static std::unordered_map<std::string, uint32_t> ReadAssignedAppIds(const std::string& shortcutsPath);
};
