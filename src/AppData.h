#pragma once

#include <optional>
#include <string>

// Per-user, runtime-writable storage under %LOCALAPPDATA%\ReviveNext\ -
// picked art, the SteamVR registration manifest, title overrides and the
// autoregister log all live here, so an installed copy (Program Files,
// not user-writable) works without elevation and survives reinstalls.
namespace AppData
{
	// %LOCALAPPDATA%\ReviveNext, created if needed; nullopt when the
	// folder can't be determined or created.
	std::optional<std::string> Dir();

	// <Dir()>\<name>, created if needed.
	std::optional<std::string> SubDir(const char* name);
}
