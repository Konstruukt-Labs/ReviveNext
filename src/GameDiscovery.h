#pragma once

#include <optional>
#include <string>
#include <vector>

// Finds Meta Horizon's local install and discovers installed Rift-platform
// games from it, independent of whether Meta Horizon Link (the app that
// normally owns this data) is currently installed - only the on-disk
// install/library data and a couple of registry keys it leaves behind are
// needed. Ported from Revive's equivalent (revivemanifestcontroller.cpp /
// ReviveInjector's main.cpp), which arrived at this approach - including
// the manual override keys - after a lot of real-world testing (see
// Revive's own commit history for that story); this starts from the
// working end state rather than repeating that path.
struct MetaGame
{
	std::string canonicalName;
	std::string appId;
	std::string title;
	// Relative to <libraryPath>/Software/<canonicalName>/, e.g.
	// "bin\\win64\\game.exe" or the resolved true executable for Unreal
	// Engine titles (see ResolveLaunchFile in the .cpp).
	std::string launchFile;
	std::string launchParameters;
	// The library root this game was found under - contains this game's
	// entry at Manifests/<canonicalName>.json and its files at
	// Software/<canonicalName>/.
	std::string libraryPath;
	// Path to the store's square cover art (cover_square_image.jpg) under
	// <base>/CoreData/Software/StoreAssets/<canonicalName>_assets/, if
	// present - Meta Horizon Link's own cached copy, not anything
	// downloaded by ReviveNext. Empty if not found.
	std::string coverArtPath;
};

class GameDiscovery
{
public:
	// The folder containing CoreData/ (cover art etc.) and Support/ (the
	// Oculus background runtime). Checks
	// HKCU\Software\ReviveNext\MetaHorizonPathOverride first (a manual
	// override for when the registry lookup below fails, e.g. after
	// uninstalling Meta Horizon Link while keeping games installed - same
	// mechanism and reasoning as Revive's MetaHorizonPathOverride), then
	// falls back to HKLM\Software\Oculus VR, LLC\Oculus's Base value.
	// Returns nullopt if neither resolves to a real directory.
	static std::optional<std::string> GetBasePath();

	// Every registered game library root (each containing a Manifests/ and
	// a Software/ subdirectory as siblings). Checks
	// HKCU\Software\ReviveNext\GamesLibraryPathOverride first, same
	// override reasoning as GetBasePath(); otherwise resolves every GUID
	// under HKCU\Software\Oculus VR, LLC\Oculus\Libraries via its Path
	// value (a \\?\Volume{GUID}\... path resolved to a drive-letter mount
	// point).
	static std::vector<std::string> GetLibraryPaths();

	// Scans every library from GetLibraryPaths() and returns the
	// installed, launchable (non-core, non-thirdParty "APP") games found
	// across all of them.
	static std::vector<MetaGame> ScanGames();
};
