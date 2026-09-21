#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct SgdbGame
{
	int64_t id;
	std::string name;
};

struct SgdbGrid
{
	int64_t id;
	std::string url;
	int width;
	int height;
	std::string author;
};

// Minimal client for the parts of the SteamGridDB API (steamgriddb.com)
// this needs: searching for a game by name, and listing its portrait
// ("grid") capsule art. Uses WinHTTP directly rather than a third-party
// HTTP library, since it's a small surface and WinHTTP ships with Windows.
class SteamGridDb
{
public:
	explicit SteamGridDb(std::string apiKey) : m_apiKey(std::move(apiKey)) {}

	// Empty on any failure (network error, bad key, no matches).
	std::vector<SgdbGame> SearchGames(const std::string& term);

	// Portrait (600x900) grids only - what Steam's Library view actually
	// uses. Empty on any failure.
	std::vector<SgdbGrid> GetPortraitGrids(int64_t gameId);

	// Widescreen (920x430) grids only - the horizontal capsule art Steam
	// uses for the "last played"/continue-playing tile, including
	// SteamVR Home's own dashboard view of it. Empty on any failure.
	std::vector<SgdbGrid> GetWidescreenGrids(int64_t gameId);

	// Downloads url to destPath. false on any failure; never throws.
	static bool DownloadFile(const std::string& url, const std::string& destPath);

	// Downloads url into memory (for decoding a thumbnail without writing
	// it to disk first). nullopt on any failure.
	static std::optional<std::string> DownloadBytes(const std::string& url);

private:
	std::string m_apiKey;
};
