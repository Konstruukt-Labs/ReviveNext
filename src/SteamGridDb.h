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
//
// Holds one WinHTTP session handle for its whole lifetime rather than
// opening a fresh one per call - WinHttpOpen does proxy detection and
// other one-time setup that's wasted work when repeated for every one of
// the dozens of requests (search, grid listings, thumbnail downloads) a
// single review session makes. The session handle is documented as safe
// to use concurrently from multiple threads (each creating its own
// connect/request handles from it), which DownloadBytes/DownloadFile rely
// on to let callers download several thumbnails in parallel.
class SteamGridDb
{
public:
	explicit SteamGridDb(std::string apiKey);
	~SteamGridDb();
	SteamGridDb(const SteamGridDb&) = delete;
	SteamGridDb& operator=(const SteamGridDb&) = delete;

	// Empty on any failure (network error, bad key, no matches).
	std::vector<SgdbGame> SearchGames(const std::string& term);

	// Portrait (600x900) grids only - what Steam's Library view actually
	// uses. Empty on any failure.
	std::vector<SgdbGrid> GetPortraitGrids(int64_t gameId);

	// Widescreen (920x430) grids only - the horizontal capsule art Steam
	// uses for the "last played"/continue-playing tile, including
	// SteamVR Home's own dashboard view of it. Empty on any failure.
	std::vector<SgdbGrid> GetWidescreenGrids(int64_t gameId);

	// Downloads url to destPath. false on any failure; never throws. Safe
	// to call concurrently (from multiple threads) on the same instance.
	bool DownloadFile(const std::string& url, const std::string& destPath) const;

	// Downloads url into memory (for decoding a thumbnail without writing
	// it to disk first). nullopt on any failure. Safe to call
	// concurrently (from multiple threads) on the same instance.
	std::optional<std::string> DownloadBytes(const std::string& url) const;

private:
	std::string m_apiKey;
	void* m_session = nullptr; // HINTERNET - void* to keep windows.h out of this header
};
