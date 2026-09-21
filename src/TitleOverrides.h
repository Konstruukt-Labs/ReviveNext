#pragma once

#include <string>
#include <unordered_map>

// Persists the plain-English title picked for a game during Library-art
// matching (see ArtMatcherGui), so every future scan uses it automatically
// - independent of shortcuts.vdf, and independent of whether Steam happens
// to be running (and would clobber an in-process shortcuts.vdf rewrite)
// at the moment a match is made. Stored at
// %LOCALAPPDATA%\ReviveNext\titles.json, keyed by canonicalName.
class TitleOverrides
{
public:
	static std::unordered_map<std::string, std::string> Load();

	// Read-modify-write - safe to call once per pick without holding the
	// whole map open across a session.
	static void Set(const std::string& canonicalName, const std::string& title);
};
