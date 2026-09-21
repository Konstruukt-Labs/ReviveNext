#pragma once

#include <string>

// Converts a source image (as shipped by Meta Horizon's own store asset
// cache - JPEGs) to PNG using Windows' built-in WIC codecs, since Steam's
// shortcuts.vdf "icon" field doesn't reliably pick up .jpg icons the way it
// does .png/.ico. No extra image library dependency needed - WIC ships with
// Windows itself.
class IconConverter
{
public:
	// Skips the conversion (returning true immediately) if destPngPath
	// already exists and is newer than sourcePath, so re-syncs don't
	// reconvert unchanged art every time. Requires COM to already be
	// initialized on the calling thread (CoInitializeEx).
	static bool ConvertToPng(const std::string& sourcePath, const std::string& destPngPath);
};
