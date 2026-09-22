#pragma once

#include <string>
#include <vector>

#include "GameDiscovery.h"

namespace SteamVrAppRegistration
{
	// Register each game as a visible SteamVR application (app_key
	// "revive.app.<canonicalName>") via AddApplicationManifest, porting
	// what Revive's old overlay did at scan time. Launching from
	// SteamVR's own Library then goes through SteamVR's
	// LaunchApplication flow: the headset shows SteamVR's app-launch
	// screen while the game starts up (instead of falling to desktop
	// view during the shortcut-to-first-frame gap), the game process is
	// identified as the registered application (ReviveInjector.exe's
	// IdentifyApplication() target), and quitting returns to the
	// SteamVR dashboard.
	//
	// Art: entries use the art ReviveNext matched into artDir (see
	// ArtMatcherGui) when present, falling back to Meta Horizon's own
	// cover art.
	//
	// Silently does nothing while SteamVR isn't running: VR_Init would
	// auto-start the whole SteamVR stack, too heavy for a desktop sync
	// run, and registered entries persist in SteamVR's own application
	// storage - syncing occasionally while SteamVR is up is enough.
	void Sync(const std::vector<MetaGame>& games, const std::string& injectorPath,
		const std::string& artDir, bool quiet = false);

	// Removes every application registered from this ReviveNext's
	// revive.vrmanifest (all the games and the tool's own entry) from the
	// running SteamVR, via RemoveApplicationManifest. Takes effect
	// immediately - no SteamVR restart needed. For testing/resetting the
	// registration (ReviveNext.exe /unregister); pops up explaining when
	// SteamVR isn't running, since removal needs a live connection.
	void Unregister(const std::string& injectorPath);
}
