#include "ArtMatcherGui.h"
#include "GameDiscovery.h"
#include "resource.h"
#include "SteamGridDb.h"
#include "TitleOverrides.h"
#include "Version.h"

#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <gdiplus.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace
{
	// A caption row ("Meta Art" / "Library cover" / "Home art") sits just
	// above each image box.
	constexpr int kCaptionY = 128, kCaptionH = 20;
	constexpr int kCoverX = 20, kCoverY = 152, kCoverW = 180, kCoverH = 180;
	// Library cover preview matches its source aspect (600x900) exactly, so
	// it never needs letterboxing. Home art (920x430) is much wider than
	// tall - its box is sized to roughly the same aspect so it isn't
	// distorted either. Both are shown side by side at all times so
	// picking one never requires switching away from the other.
	constexpr int kPortraitX = 220, kPortraitY = 152, kPortraitW = 140, kPortraitH = 210;
	constexpr int kHomeX = 380, kHomeY = 152, kHomeW = 460, kHomeH = 215;

	constexpr int kThumbPortraitW = 96, kThumbPortraitH = 144;
	constexpr int kThumbHomeW = 160, kThumbHomeH = 75;

	constexpr int kIdEditSearch = 101;
	constexpr int kIdBtnSearch = 102;
	constexpr int kIdListboxGames = 103;
	constexpr int kIdListviewGridsPortrait = 104;
	constexpr int kIdListviewGridsHome = 105;
	constexpr int kIdBtnBack = 106;
	constexpr int kIdBtnSkip = 107;
	constexpr int kIdBtnClose = 108;
	constexpr int kIdBtnSettings = 109;
	constexpr int kIdBtnHelp = 113;
	constexpr int kIdBtnPrev = 110;
	constexpr int kIdLinkBuyCoffee = 111;
	constexpr int kIdLinkSponsor = 112;

	enum class Mode { PickingGame, PickingGrid };

	// The two art shapes, both matched and shown at once - see
	// SteamGridDb::GetPortraitGrids/GetWidescreenGrids and ComputeStem
	// for what each one is keyed and searched by.
	enum class ArtKind { Portrait, Home };

	std::wstring Utf8ToWide(const std::string& s)
	{
		if (s.empty())
			return {};
		int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring result(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), len);
		return result;
	}

	std::string WideToUtf8(const std::wstring& s)
	{
		if (s.empty())
			return {};
		int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
		std::string result(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), len, nullptr, nullptr);
		return result;
	}

	// <artDir>/<canonicalName>p for the portrait cover,
	// <artDir>/<canonicalName> (no suffix) for the widescreen art -
	// ReviveNext's own storage, keyed by the game itself (canonical names
	// are already valid, collision-free file names) rather than by any
	// Steam-assigned shortcut id. The same files feed the SteamVR
	// application registration's image_path.
	// The executable's base name ("Game\\Stormland.exe" -> "Stormland") -
	// a better default search seed than the store manifest's title, which
	// is what an unconfirmed game's title still is. Known default
	// suffixes Unreal and other engines append to shipping builds are
	// stripped - they say nothing about the game's name.
	std::string ExeSearchSeed(const MetaGame& game)
	{
		const std::string& file = game.launchFile;
		size_t nameStart = file.find_last_of("\\/") + 1; // 0 when no separator
		std::string name = file.substr(nameStart);
		if (name.size() >= 4 && _stricmp(name.c_str() + name.size() - 4, ".exe") == 0)
			name.resize(name.size() - 4);

		constexpr const char* kStrippedSuffixes[] = {
			"-Win64-Shipping", "-Win32-Shipping", "-Win64-Test", "-Win64-Debug",
			"-Shipping", "-Test", "-Debug", "-Win64", "-Win32",
			"-Win64-Shipping_R", "-EGS", "-Steam",
		};
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (const char* suffix : kStrippedSuffixes)
			{
				size_t len = strlen(suffix);
				if (name.size() > len
					&& _stricmp(name.c_str() + name.size() - len, suffix) == 0)
				{
					name.resize(name.size() - len);
					changed = true;
					break;
				}
			}
		}
		return name;
	}

	std::string ComputeStem(const std::string& canonicalName, const std::string& artDir, ArtKind kind)
	{
		std::string name = canonicalName + (kind == ArtKind::Portrait ? "p" : "");
		return (fs::path(artDir) / name).string();
	}

	constexpr const char* kArtExtensions[] = { ".png", ".jpg", ".jpeg", ".webp" };

	enum class ExistingState { None, Matched, Skipped };

	ExistingState GetExistingState(const std::string& stem)
	{
		std::error_code ec;
		if (fs::exists(stem + ".skip", ec))
			return ExistingState::Skipped;
		for (const char* ext : kArtExtensions)
			if (fs::exists(stem + ext, ec))
				return ExistingState::Matched;
		return ExistingState::None;
	}

	// The actual currently-saved art file for a stem, if any (whichever
	// extension it was saved under).
	std::optional<std::string> FindExistingArtFile(const std::string& stem)
	{
		std::error_code ec;
		for (const char* ext : kArtExtensions)
			if (fs::exists(stem + ext, ec))
				return stem + ext;
		return std::nullopt;
	}

	// Removes any existing art/skip file for this stem (any extension), so
	// a fresh pick or skip always fully replaces whatever was there
	// before, rather than leaving a stale duplicate under some other
	// extension alongside the new one.
	void ClearExisting(const std::string& stem)
	{
		std::error_code ec;
		fs::remove(stem + ".skip", ec);
		for (const char* ext : kArtExtensions)
			fs::remove(stem + ext, ec);
	}

	// Loads a PNG from the exe's resources (see src/ReviveNext.rc).
	// Null if unavailable - callers degrade to simply not drawing it.
	Gdiplus::Image* LoadResourceImage(int id)
	{
		HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
		if (!res)
			return nullptr;
		HGLOBAL loaded = LoadResource(nullptr, res);
		if (!loaded)
			return nullptr;
		void* data = LockResource(loaded);
		DWORD size = SizeofResource(nullptr, res);
		if (!data || !size)
			return nullptr;

		// Copy into a real HGLOBAL: resource memory is not a handle
		// CreateStreamOnHGlobal can take ownership of.
		HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
		if (!copy)
			return nullptr;
		void* dst = GlobalLock(copy);
		memcpy(dst, data, size);
		GlobalUnlock(copy);

		IStream* stream = nullptr;
		if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK)
		{
			GlobalFree(copy);
			return nullptr;
		}
		Gdiplus::Image* image = new Gdiplus::Image(stream);
		stream->Release();
		if (image->GetLastStatus() != Gdiplus::Ok)
		{
			delete image;
			return nullptr;
		}
		return image;
	}

	// The ReviveNext badges (96 px for the portrait Library cover, 48 px
	// for the widescreen Home art) and the window lockup, each loaded
	// lazily once - first use is well after GdiplusStartup in Run().
	Gdiplus::Image* GetBadge(ArtKind kind)
	{
		static Gdiplus::Image* badges[2] = {};
		const int index = kind == ArtKind::Portrait ? 0 : 1;
		if (!badges[index])
			badges[index] = LoadResourceImage(kind == ArtKind::Portrait ? IDB_REVIVE_BADGE_PORTRAIT : IDB_REVIVE_BADGE_HOME);
		return badges[index];
	}

	Gdiplus::Image* GetLogo()
	{
		static Gdiplus::Image* logo = LoadResourceImage(IDB_REVIVE_LOGO);
		return logo;
	}

	int GetEncoderClsid(const WCHAR* format, CLSID* clsid)
	{
		UINT num = 0, size = 0;
		Gdiplus::GetImageEncodersSize(&num, &size);
		if (!size)
			return -1;
		std::vector<BYTE> buffer(size);
		Gdiplus::GetImageEncoders(num, size, (Gdiplus::ImageCodecInfo*)buffer.data());
		for (UINT i = 0; i < num; i++)
		{
			const Gdiplus::ImageCodecInfo* encoder = (const Gdiplus::ImageCodecInfo*)buffer.data() + i;
			if (wcscmp(encoder->MimeType, format) == 0)
			{
				*clsid = encoder->Clsid;
				return (int)i;
			}
		}
		return -1;
	}

	// Burns the ReviveNext badge into a just-downloaded art file: bottom
	// left corner, 20 px padding from the image edges (scaled down only if
	// the image is too small to fit it at native size). Only newly picked
	// art is badged - existing files are never touched, re-picking is what
	// (re-)applies it. WebP is left untouched: GDI+ has no webp encoder,
	// so re-encoding would destroy the file.
	void ApplyBadge(const std::string& path, ArtKind kind)
	{
		Gdiplus::Image* badge = GetBadge(kind);
		if (!badge)
			return;

		const std::string ext = fs::path(path).extension().string();
		const WCHAR* mime = nullptr;
		bool jpeg = false;
		if (_stricmp(ext.c_str(), ".png") == 0)
			mime = L"image/png";
		else if (_stricmp(ext.c_str(), ".jpg") == 0 || _stricmp(ext.c_str(), ".jpeg") == 0)
		{
				mime = L"image/jpeg";
			jpeg = true;
		}
		else
			return;

		// Load through a memory stream: a Bitmap constructed from a file
		// path keeps that file locked, which would block saving back over
		// it below.
		std::ifstream in(path, std::ios::binary);
		std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (data.empty())
			return;
		IStream* stream = nullptr;
		if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK)
			return;
		ULONG written = 0;
		stream->Write(data.data(), (ULONG)data.size(), &written);
		LARGE_INTEGER zero = {};
		stream->Seek(zero, STREAM_SEEK_SET, nullptr);

		{
			Gdiplus::Bitmap art(stream);
			if (art.GetLastStatus() == Gdiplus::Ok)
			{
				const int artW = (int)art.GetWidth(), artH = (int)art.GetHeight();
				int badgeW = (int)badge->GetWidth(), badgeH = (int)badge->GetHeight();
				if (artW < badgeW + 40 || artH < badgeH + 40)
				{
					double scale = (std::min)((artW - 40.0) / badgeW, (artH - 40.0) / badgeH);
					badgeW = (int)(badgeW * scale);
					badgeH = (int)(badgeH * scale);
				}
				if (badgeW > 0 && badgeH > 0)
				{
					Gdiplus::Graphics graphics(&art);
					graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
					graphics.DrawImage(badge, 20, artH - 20 - badgeH, badgeW, badgeH);
				}

				CLSID clsid = {};
				if (GetEncoderClsid(mime, &clsid) >= 0)
				{
					Gdiplus::EncoderParameters params = {};
					ULONG quality = 90;
					if (jpeg)
					{
						params.Count = 1;
						params.Parameter[0].Guid = Gdiplus::EncoderQuality;
						params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
						params.Parameter[0].NumberOfValues = 1;
						params.Parameter[0].Value = &quality;
					}
					art.Save(Utf8ToWide(path).c_str(), &clsid, jpeg ? &params : nullptr);
				}
			}
		}
		stream->Release();
	}

	HBITMAP DownloadAndScaleThumbnail(SteamGridDb& client, const std::string& url, int thumbW, int thumbH)
	{
		auto bytes = client.DownloadBytes(url);
		if (!bytes)
			return nullptr;

		IStream* stream = SHCreateMemStream((const BYTE*)bytes->data(), (UINT)bytes->size());
		if (!stream)
			return nullptr;

		Gdiplus::Image image(stream);
		stream->Release();
		if (image.GetLastStatus() != Gdiplus::Ok)
			return nullptr;

		Gdiplus::Bitmap thumb(thumbW, thumbH, PixelFormat32bppARGB);
		Gdiplus::Graphics g(&thumb);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.DrawImage(&image, 0, 0, thumbW, thumbH);

		HBITMAP hbmp = nullptr;
		thumb.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hbmp);
		return hbmp;
	}

	// All state for the single matching session - a static/global is
	// pragmatic here: this is a one-shot modal tool with exactly one
	// window and one session, not a reusable component.
	struct State
	{
		HWND hwnd = nullptr;
		HWND editSearch = nullptr, btnSearch = nullptr, listboxGames = nullptr;
		HWND listviewGridsPortrait = nullptr, listviewGridsHome = nullptr;
		HWND labelGridsPortrait = nullptr, labelGridsHome = nullptr;
		HWND btnBack = nullptr, btnSkip = nullptr, titleLabel = nullptr, counterLabel = nullptr, statusLabel = nullptr;
		HWND finalLabel = nullptr, btnClose = nullptr, btnSettings = nullptr;
		HWND btnHelp = nullptr;
		HWND linkBuyCoffee = nullptr, linkSponsor = nullptr;
		HWND btnPrev = nullptr;
		HWND captionMeta = nullptr, captionLibrary = nullptr, captionHome = nullptr;

		Gdiplus::Image* coverArt = nullptr;
		Gdiplus::Image* currentArtPortrait = nullptr; // the Library cover already saved for the game on screen, if any
		Gdiplus::Image* currentArtHome = nullptr; // the Home art already saved for the game on screen, if any
		HIMAGELIST imageListPortrait = nullptr, imageListHome = nullptr;

		std::vector<MetaGame> games;
		size_t currentIndex = 0;
		std::string artDir, injectorPath;
		SteamGridDb* client = nullptr; // null when no API key is configured - search is unavailable, not the whole window

		std::vector<SgdbGame> gameResults;
		std::vector<SgdbGrid> gridResultsPortrait, gridResultsHome;
		std::string pickedGameName; // SteamGridDB's title for the game currently shown in the grid picker
		Mode mode = Mode::PickingGame;
		ExistingState existingPortrait = ExistingState::None, existingHome = ExistingState::None;

		std::vector<MetaGame> matchedGames; // returned to the caller when the window closes

	} g;

	void SetStatus(const std::wstring& text)
	{
		SetWindowTextW(g.statusLabel, text.c_str());
	}

	void SwitchMode(Mode mode)
	{
		g.mode = mode;
		bool pickingGame = (mode == Mode::PickingGame);
		ShowWindow(g.listboxGames, pickingGame ? SW_SHOW : SW_HIDE);
		for (HWND h : { g.listviewGridsPortrait, g.listviewGridsHome, g.labelGridsPortrait, g.labelGridsHome })
			ShowWindow(h, pickingGame ? SW_HIDE : SW_SHOW);
		ShowWindow(g.btnBack, pickingGame ? SW_HIDE : SW_SHOW);
	}

	void DoSearch()
	{
		if (!g.client)
			return; // no API key configured - caller is responsible for explaining this in the UI

		wchar_t buf[512];
		GetWindowTextW(g.editSearch, buf, 512);
		std::string term = WideToUtf8(buf);
		if (term.empty())
			return;

		SetStatus(L"Searching...");
		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		g.gameResults = g.client->SearchGames(term);
		SetCursor(LoadCursor(nullptr, IDC_ARROW));

		SendMessageW(g.listboxGames, LB_RESETCONTENT, 0, 0);
		for (auto& r : g.gameResults)
		{
			std::wstring wname = Utf8ToWide(r.name);
			SendMessageW(g.listboxGames, LB_ADDSTRING, 0, (LPARAM)wname.c_str());
		}

		SwitchMode(Mode::PickingGame);
		SetStatus(g.gameResults.empty() ? L"No matches - try a different search term." : L"Double-click a game to see its grid art.");
	}

	// Fetches and fills one kind's listview - shared by ShowGridsForGame so
	// both kinds populate together in a single "pick a game" step, rather
	// than needing to be searched one at a time.
	void PopulateGridList(ArtKind kind, int64_t gameId)
	{
		bool portrait = kind == ArtKind::Portrait;
		HWND listview = portrait ? g.listviewGridsPortrait : g.listviewGridsHome;
		HIMAGELIST& imageList = portrait ? g.imageListPortrait : g.imageListHome;
		std::vector<SgdbGrid>& results = portrait ? g.gridResultsPortrait : g.gridResultsHome;
		int thumbW = portrait ? kThumbPortraitW : kThumbHomeW;
		int thumbH = portrait ? kThumbPortraitH : kThumbHomeH;

		results = portrait ? g.client->GetPortraitGrids(gameId) : g.client->GetWidescreenGrids(gameId);
		if (results.size() > 24)
			results.resize(24);

		if (imageList)
		{
			ImageList_Destroy(imageList);
			imageList = nullptr;
		}
		imageList = ImageList_Create(thumbW, thumbH, ILC_COLOR32, 0, (int)results.size() + 1);
		ListView_SetImageList(listview, imageList, LVSIL_NORMAL);
		ListView_SetIconSpacing(listview, thumbW + 16, thumbH + 24);
		ListView_DeleteAllItems(listview);

		// Downloaded concurrently - each thumbnail is an independent
		// network round-trip, so fetching up to 24 of them one at a time
		// made populating a single game's grid list take up to 24x as
		// long as necessary. Win32 UI calls (ImageList_Add etc.) still
		// happen back on this thread afterward, in order, once every
		// download has finished.
		std::vector<HBITMAP> thumbnails(results.size(), nullptr);
		std::vector<std::thread> workers;
		for (size_t i = 0; i < results.size(); ++i)
			workers.emplace_back([&, i] { thumbnails[i] = DownloadAndScaleThumbnail(*g.client, results[i].url, thumbW, thumbH); });
		for (auto& worker : workers)
			worker.join();

		for (size_t i = 0; i < results.size(); ++i)
		{
			int imageIndex = -1;
			if (thumbnails[i])
			{
				imageIndex = ImageList_Add(imageList, thumbnails[i], nullptr);
				DeleteObject(thumbnails[i]);
			}

			std::wstring label = std::to_wstring(results[i].width) + L"x" + std::to_wstring(results[i].height);
			LVITEMW item = {};
			item.mask = LVIF_TEXT | LVIF_IMAGE;
			item.iItem = (int)i;
			item.pszText = label.data();
			item.iImage = imageIndex;
			ListView_InsertItem(listview, &item);
		}
	}

	void ShowGridsForGame(int idx)
	{
		if (idx < 0 || (size_t)idx >= g.gameResults.size())
			return;

		g.pickedGameName = g.gameResults[idx].name;
		int64_t gameId = g.gameResults[idx].id;

		SetStatus(L"Loading grid art...");
		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		PopulateGridList(ArtKind::Portrait, gameId);
		PopulateGridList(ArtKind::Home, gameId);
		SetCursor(LoadCursor(nullptr, IDC_ARROW));

		SwitchMode(Mode::PickingGrid);
		SetStatus(g.gridResultsPortrait.empty() && g.gridResultsHome.empty()
			? L"No grid art submitted for that game yet."
			: L"Double-click a Library cover or Home art thumbnail to use it - pick either or both.");
	}

	void LoadGameState();

	void AdvanceToNext()
	{
		g.currentIndex++;
		if (g.currentIndex >= g.games.size())
		{
			DestroyWindow(g.hwnd);
			return;
		}
		LoadGameState();
	}

	// Pure browsing, unlike AdvanceToNext - never triggers the end-of-list
	// finish flow, just moves the cursor, so the user can jump straight
	// back to one specific game without re-walking the list.
	void GoToPrev()
	{
		if (g.currentIndex > 0)
		{
			g.currentIndex--;
			LoadGameState();
		}
	}

	// Recomputes both kinds' existing-art state/preview for the game on
	// screen and refreshes the counter text + Skip button label to match,
	// without touching search results, mode, or navigation - called both
	// from LoadGameState and right after a pick, since a pick no longer
	// advances to the next game (the user may still want to pick the
	// other kind for the same game).
	void RefreshArtStatus()
	{
		const MetaGame& game = g.games[g.currentIndex];

		delete g.currentArtPortrait;
		g.currentArtPortrait = nullptr;
		delete g.currentArtHome;
		g.currentArtHome = nullptr;
		g.existingPortrait = ExistingState::None;
		g.existingHome = ExistingState::None;

		std::string stemP = ComputeStem(game.canonicalName, g.artDir, ArtKind::Portrait);
		g.existingPortrait = GetExistingState(stemP);
		if (auto f = FindExistingArtFile(stemP))
			g.currentArtPortrait = new Gdiplus::Image(Utf8ToWide(*f).c_str());

		std::string stemH = ComputeStem(game.canonicalName, g.artDir, ArtKind::Home);
		g.existingHome = GetExistingState(stemH);
		if (auto f = FindExistingArtFile(stemH))
			g.currentArtHome = new Gdiplus::Image(Utf8ToWide(*f).c_str());

		auto describe = [](const wchar_t* name, ExistingState s)
		{
			const wchar_t* state = s == ExistingState::Matched ? L"set" : s == ExistingState::Skipped ? L"skipped" : L"not set";
			return std::wstring(name) + L" " + state;
		};
		std::wstring counter = std::to_wstring(g.currentIndex + 1) + L" / " + std::to_wstring(g.games.size()) +
			L" - " + describe(L"Library cover:", g.existingPortrait) + L", " + describe(L"Home art:", g.existingHome);
		SetWindowTextW(g.counterLabel, counter.c_str());

		// The button never destroys existing art - its label just reflects
		// that clicking past a game with at least one kind already handled
		// is always safe.
		bool eitherHandled = g.existingPortrait != ExistingState::None || g.existingHome != ExistingState::None;
		SetWindowTextW(g.btnSkip, eitherHandled ? L"Next (keep existing)" : L"Skip this game");

		InvalidateRect(g.hwnd, nullptr, TRUE);
	}

	void ChooseGrid(ArtKind kind, int idx)
	{
		std::vector<SgdbGrid>& results = kind == ArtKind::Portrait ? g.gridResultsPortrait : g.gridResultsHome;
		if (idx < 0 || (size_t)idx >= results.size())
			return;

		MetaGame& game = g.games[g.currentIndex];
		std::string stem = ComputeStem(game.canonicalName, g.artDir, kind);
		std::string ext = fs::path(results[idx].url).extension().string();
		if (ext.empty())
			ext = ".png";

		// Release the previously-loaded preview for this stem, if any,
		// before touching its file on disk - Gdiplus::Image keeps its
		// source file open for as long as the Image object lives, which
		// would otherwise block deleting/overwriting it below (silently:
		// ClearExisting swallows the failure, and even if the download
		// itself still succeeds, this same Image object - not yet
		// reloaded - is what the preview keeps showing until
		// RefreshArtStatus() replaces it further down).
		Gdiplus::Image*& target = kind == ArtKind::Portrait ? g.currentArtPortrait : g.currentArtHome;
		delete target;
		target = nullptr;

		// Overwrite whatever was here before (previously-matched art under
		// a different extension, or a skip marker) rather than leaving it
		// alongside the new pick.
		ClearExisting(stem);

		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		bool ok = g.client->DownloadFile(results[idx].url, stem + ext);
		SetCursor(LoadCursor(nullptr, IDC_ARROW));

		if (ok)
		{
			// Stamp the badge on before the preview below reloads the file.
			ApplyBadge(stem + ext, kind);
			// The SteamGridDB title is plain English and far cleaner than
			// the raw canonicalName - use it as this shortcut's AppName
			// from here on. Doesn't affect the art file just written,
			// since that's keyed by Steam's own appid, not by title.
			// Persisted immediately (not just returned to the caller) so
			// it applies on every future scan regardless of whether this
			// process gets to re-sync shortcuts.vdf itself right now.
			if (!g.pickedGameName.empty())
			{
				game.title = g.pickedGameName;
				TitleOverrides::Set(game.canonicalName, g.pickedGameName);
			}
			g.matchedGames.push_back(game);
			// Stays on this game and in the grid picker (rather than
			// advancing) so picking the other kind for the same game -
			// the common case - never needs a second search.
			RefreshArtStatus();
		}
		else
		{
			SetStatus(L"Download failed - try a different one.");
		}
	}

	void SkipCurrent()
	{
		// Only actually marks-as-skipped (and only ever touches a file)
		// when nothing is there yet for that kind. Picking a NEW grid
		// (ChooseGrid) is the only action that overwrites existing
		// art/skip state - this button must never destroy a previous pick
		// just by clicking past an already-matched game to browse the rest.
		for (ArtKind kind : { ArtKind::Portrait, ArtKind::Home })
		{
			std::string stem = ComputeStem(g.games[g.currentIndex].canonicalName, g.artDir, kind);
			if (GetExistingState(stem) == ExistingState::None)
			{
				std::ofstream marker(stem + ".skip");
			}
		}
		AdvanceToNext();
	}

	void LoadGameState();

	void LoadGameState()
	{
		const MetaGame& game = g.games[g.currentIndex];

		EnableWindow(g.btnPrev, g.currentIndex > 0);

		delete g.coverArt;
		g.coverArt = game.coverArtPath.empty() ? nullptr : new Gdiplus::Image(Utf8ToWide(game.coverArtPath).c_str());

		SetWindowTextW(g.titleLabel, Utf8ToWide(game.title).c_str());

		bool canMatchArt = g.client != nullptr;

		RefreshArtStatus();

		// Art matching needs an API key (to search) - hide the whole
		// search/pick flow and explain why when it's missing, rather than
		// leaving nonfunctional controls visible.
		for (HWND h : { g.editSearch, g.btnSearch, g.listboxGames, g.listviewGridsPortrait, g.listviewGridsHome,
				g.labelGridsPortrait, g.labelGridsHome, g.btnBack })
			ShowWindow(h, SW_HIDE);
		if (canMatchArt)
		{
			for (HWND h : { g.editSearch, g.btnSearch })
				ShowWindow(h, SW_SHOW);
			SwitchMode(Mode::PickingGame);
		}
		else
		{
			g.mode = Mode::PickingGame;
			SetStatus(L"Set a SteamGridDB API key (Settings...) to enable Library art matching.");
		}

		// Pre-fill the search with the confirmed name once there is one (a
		// picked SteamGridDB title, via the overrides), the executable's
		// base name until then.
		const auto overrides = TitleOverrides::Load();
		const std::string& seed = overrides.count(game.canonicalName) != 0
			? game.title
			: ExeSearchSeed(game);
		SetWindowTextW(g.editSearch, Utf8ToWide(seed).c_str());

		InvalidateRect(g.hwnd, nullptr, TRUE);
		if (canMatchArt)
			DoSearch();
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_COMMAND:
			if (LOWORD(wParam) == kIdBtnSearch && HIWORD(wParam) == BN_CLICKED)
				DoSearch();
			else if (LOWORD(wParam) == kIdBtnSkip && HIWORD(wParam) == BN_CLICKED)
				SkipCurrent();
			else if (LOWORD(wParam) == kIdBtnPrev && HIWORD(wParam) == BN_CLICKED)
				GoToPrev();
			else if (LOWORD(wParam) == kIdBtnBack && HIWORD(wParam) == BN_CLICKED)
				SwitchMode(Mode::PickingGame);
			else if (LOWORD(wParam) == kIdBtnClose && HIWORD(wParam) == BN_CLICKED)
				DestroyWindow(hwnd);
			else if (LOWORD(wParam) == kIdBtnSettings && HIWORD(wParam) == BN_CLICKED)
			{
				EnableWindow(hwnd, FALSE);
				ArtMatcherGui::PromptForApiKey();
				EnableWindow(hwnd, TRUE);
				SetForegroundWindow(hwnd);
			}
			else if (LOWORD(wParam) == kIdBtnHelp && HIWORD(wParam) == BN_CLICKED)
			{
				MessageBoxW(hwnd,
					L"Registration happens when this window closes - launch ReviveNext whenever you like, as long as SteamVR is running by the time you close it.\n\n"
					L"Making your games show up in SteamVR's Library (Other Apps tab):\n\n"
					L"1. Have SteamVR running (start it if it is not).\n"
					L"2. Close this window when you are done - that registers your games and arms the automatic refresh.\n"
					L"3. The games appear under Other Apps. If the list does not refresh right away, restart SteamVR once.\n\n"
					L"From then on it is fully automatic: every SteamVR start refreshes the list, and newly installed games appear on their own.\n\n"
					L"(Cover art matching is optional - enter a free SteamGridDB API key via the Settings button in the application window.)",
					L"ReviveNext - Help", MB_ICONINFORMATION);
			}
			else if (LOWORD(wParam) == kIdListboxGames && HIWORD(wParam) == LBN_DBLCLK)
			{
				int sel = (int)SendMessageW(g.listboxGames, LB_GETCURSEL, 0, 0);
				ShowGridsForGame(sel);
			}
			return 0;

		case WM_CTLCOLORSTATIC:
			// The support links paint on an explicitly white background.
			if ((HWND)lParam == g.linkBuyCoffee || (HWND)lParam == g.linkSponsor)
			{
				SetBkColor((HDC)wParam, RGB(255, 255, 255));
				return (INT_PTR)GetStockObject(WHITE_BRUSH);
			}
			break;

		case WM_NOTIFY:
		{
			LPNMHDR hdr = (LPNMHDR)lParam;
			if (hdr->code == NM_CLICK && (hdr->idFrom == kIdLinkBuyCoffee || hdr->idFrom == kIdLinkSponsor))
			{
				// Support links (SysLink) - open in the default browser.
				auto* link = (PNMLINK)lParam;
				ShellExecuteW(hwnd, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
			}
			else if (hdr->code == NM_DBLCLK && (hdr->idFrom == kIdListviewGridsPortrait || hdr->idFrom == kIdListviewGridsHome))
			{
				auto* activate = (LPNMITEMACTIVATE)lParam;
				if (activate->iItem >= 0)
					ChooseGrid(hdr->idFrom == kIdListviewGridsPortrait ? ArtKind::Portrait : ArtKind::Home, activate->iItem);
			}
			return 0;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			Gdiplus::Graphics gr(hdc);

			// The lockup sits in its own band at the top-left; every
			// control below is laid out underneath it (see CreateControls).
			if (Gdiplus::Image* logo = GetLogo())
			{
				// The lockup asset carries 30 px of empty space at its top and
				// bottom, and 20 px at its left - draw only the content so the
				// header band stays tight and the logo lines up with the left
				// edge of the fields below (everything is laid out for that).
				gr.DrawImage(logo,
					Gdiplus::RectF(20.0f, 12.0f, (float)logo->GetWidth() - 20.0f, 100.0f),
					20.0f, 30.0f, (float)logo->GetWidth() - 20.0f, 100.0f,
					Gdiplus::UnitPixel);
			}

			// Contain-fit rather than stretch: Library cover (600x900) and
			// Home art (920x430) have very different aspect ratios, so a
			// fixed box for each would either distort the image or (since
			// both are shown at once, unlike a single toggled preview)
			// need a different box per kind anyway - this fits either
			// cleanly into whatever box it's given, letterboxed if needed.
			auto drawContain = [&](Gdiplus::Image* img, int x, int y, int w, int h, const wchar_t* placeholder)
			{
				Gdiplus::SolidBrush bg(Gdiplus::Color(255, 240, 240, 240));
				gr.FillRectangle(&bg, x, y, w, h);
				if (img && img->GetLastStatus() == Gdiplus::Ok)
				{
					double imgW = img->GetWidth(), imgH = img->GetHeight();
					double scaleW = (double)w / imgW, scaleH = (double)h / imgH;
					double scale = scaleW < scaleH ? scaleW : scaleH; // windows.h's min macro shadows std::min here
					int drawW = (int)(imgW * scale), drawH = (int)(imgH * scale);
					int drawX = x + (w - drawW) / 2, drawY = y + (h - drawH) / 2;
					gr.DrawImage(img, drawX, drawY, drawW, drawH);
				}
				else
				{
					Gdiplus::Font font(L"Segoe UI", 9);
					Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 120, 120, 120));
					Gdiplus::RectF rect((Gdiplus::REAL)x + 8, (Gdiplus::REAL)y + 8, (Gdiplus::REAL)w - 16, (Gdiplus::REAL)h - 16);
					gr.DrawString(placeholder, -1, &font, rect, nullptr, &textBrush);
				}
			};

			if (g.coverArt && g.coverArt->GetLastStatus() == Gdiplus::Ok)
				gr.DrawImage(g.coverArt, kCoverX, kCoverY, kCoverW, kCoverH);
			else
			{
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 220, 220, 220));
				gr.FillRectangle(&brush, kCoverX, kCoverY, kCoverW, kCoverH);
			}

			drawContain(g.currentArtPortrait, kPortraitX, kPortraitY, kPortraitW, kPortraitH, L"No Library\ncover yet");
			drawContain(g.currentArtHome, kHomeX, kHomeY, kHomeW, kHomeH, L"No Home art yet");

			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_CLOSE:
			DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	void CreateControls(HWND hwnd)
	{
		HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		auto setFont = [&](HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE); };

		// The Settings button sits on the lockup's baseline (the logo's
		// content spans y 12-112); the support links occupy the band above
		// it where the button used to be. Emoji glyphs need an emoji-capable
		// font to render at all.
		g.btnSettings = CreateWindowExW(0, L"BUTTON", L"Settings...", WS_CHILD | WS_VISIBLE,
			760, 86, 120, 26, hwnd, (HMENU)(INT_PTR)kIdBtnSettings, nullptr, nullptr);
		g.btnHelp = CreateWindowExW(0, L"BUTTON", L"Help", WS_CHILD | WS_VISIBLE,
			690, 86, 60, 26, hwnd, (HMENU)(INT_PTR)kIdBtnHelp, nullptr, nullptr);

		HFONT emojiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Emoji");
		g.linkBuyCoffee = CreateWindowExW(0, WC_LINK,
			L"<a href=\"https://buymeacoffee.com/itsvrk\">Buy me a coffee \u2615</a>",
			WS_CHILD | WS_VISIBLE, 660, 18, 220, 24, hwnd, (HMENU)(INT_PTR)kIdLinkBuyCoffee, nullptr, nullptr);
		g.linkSponsor = CreateWindowExW(0, WC_LINK,
			L"<a href=\"https://github.com/sponsors/BrianGilbert\">Sponsor on GitHub \u2764</a>",
			WS_CHILD | WS_VISIBLE, 660, 46, 220, 24, hwnd, (HMENU)(INT_PTR)kIdLinkSponsor, nullptr, nullptr);
		for (HWND h : { g.linkBuyCoffee, g.linkSponsor })
		{
			SendMessageW(h, WM_SETFONT, (WPARAM)emojiFont, TRUE);

			// Size each link to its text and right-align it to the window's
			// right margin - SysLink has no text alignment of its own, so a
			// snug rect is what makes it read as right-aligned.
			SIZE ideal = {};
			SendMessageW(h, LM_GETIDEALSIZE, 0, (LPARAM)&ideal);
			int y = h == g.linkBuyCoffee ? 18 : 46;
			MoveWindow(h, 880 - ideal.cx, y, ideal.cx, ideal.cy + 2, TRUE);
		}

		// Captions + cover art, Library cover preview, and Home art preview
		// all sit in a row above (images drawn in WM_PAINT); everything
		// else goes below it since Home art's box is too wide to also fit
		// a label column beside it the way the single portrait preview
		// used to.
		g.captionMeta = CreateWindowExW(0, L"STATIC", L"Meta Art", WS_CHILD | WS_VISIBLE,
			kCoverX, kCaptionY, kCoverW, kCaptionH, hwnd, nullptr, nullptr, nullptr);
		g.captionLibrary = CreateWindowExW(0, L"STATIC", L"Library cover", WS_CHILD | WS_VISIBLE,
			kPortraitX, kCaptionY, kPortraitW, kCaptionH, hwnd, nullptr, nullptr, nullptr);
		g.captionHome = CreateWindowExW(0, L"STATIC", L"Home art", WS_CHILD | WS_VISIBLE,
			kHomeX, kCaptionY, kHomeW, kCaptionH, hwnd, nullptr, nullptr, nullptr);

		g.titleLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			20, 384, 860, 24, hwnd, nullptr, nullptr, nullptr);
		g.counterLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			20, 410, 600, 20, hwnd, nullptr, nullptr, nullptr);

		g.editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
			20, 440, 600, 26, hwnd, (HMENU)(INT_PTR)kIdEditSearch, nullptr, nullptr);
		g.btnSearch = CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE,
			626, 440, 90, 26, hwnd, (HMENU)(INT_PTR)kIdBtnSearch, nullptr, nullptr);

		g.labelGridsPortrait = CreateWindowExW(0, L"STATIC", L"Library cover (600x900):", WS_CHILD,
			20, 476, 410, 20, hwnd, nullptr, nullptr, nullptr);
		g.labelGridsHome = CreateWindowExW(0, L"STATIC", L"Home art (920x430):", WS_CHILD,
			450, 476, 410, 20, hwnd, nullptr, nullptr, nullptr);

		g.listboxGames = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
			20, 500, 860, 220, hwnd, (HMENU)(INT_PTR)kIdListboxGames, nullptr, nullptr);

		// Side by side rather than the single listview this used to be -
		// both kinds' candidates for the same searched game are available
		// at once, so picking one never means losing the other's results.
		g.listviewGridsPortrait = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | LVS_ICON | LVS_SINGLESEL,
			20, 500, 410, 220, hwnd, (HMENU)(INT_PTR)kIdListviewGridsPortrait, nullptr, nullptr);
		g.listviewGridsHome = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | LVS_ICON | LVS_SINGLESEL,
			450, 500, 410, 220, hwnd, (HMENU)(INT_PTR)kIdListviewGridsHome, nullptr, nullptr);

		g.btnBack = CreateWindowExW(0, L"BUTTON", L"< Back to search results", WS_CHILD | WS_VISIBLE,
			20, 730, 200, 28, hwnd, (HMENU)(INT_PTR)kIdBtnBack, nullptr, nullptr);
		// Prev lives here rather than up by the counter - the only other
		// "move to another game" control is this row's Skip/Next button,
		// so keeping both together reads as one pair instead of two
		// separate, overlapping ways to advance.
		g.btnPrev = CreateWindowExW(0, L"BUTTON", L"< Prev", WS_CHILD | WS_VISIBLE,
			630, 730, 80, 28, hwnd, (HMENU)(INT_PTR)kIdBtnPrev, nullptr, nullptr);
		g.btnSkip = CreateWindowExW(0, L"BUTTON", L"Skip this game", WS_CHILD | WS_VISIBLE,
			720, 730, 160, 28, hwnd, (HMENU)(INT_PTR)kIdBtnSkip, nullptr, nullptr);

		g.statusLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			20, 766, 860, 24, hwnd, nullptr, nullptr, nullptr);

		g.finalLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
			20, 500, 860, 220, hwnd, nullptr, nullptr, nullptr);
		g.btnClose = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD,
			720, 730, 160, 28, hwnd, (HMENU)(INT_PTR)kIdBtnClose, nullptr, nullptr);

		for (HWND h : { g.captionMeta, g.captionLibrary, g.captionHome, g.titleLabel, g.counterLabel,
				g.editSearch, g.btnSearch, g.listboxGames,
				g.listviewGridsPortrait, g.listviewGridsHome, g.labelGridsPortrait, g.labelGridsHome,
				g.btnBack, g.btnSkip, g.btnPrev,
				g.statusLabel, g.finalLabel, g.btnClose, g.btnSettings, g.btnHelp })
			setFont(h);
	}
}

std::vector<MetaGame> ArtMatcherGui::Run(const std::vector<MetaGame>& allGames,
	const std::string& injectorPath, const std::string& artDir,
	const std::optional<std::string>& apiKey)
{
	if (allGames.empty())
		return {};

	std::error_code ec;
	fs::create_directories(artDir, ec);

	// Every discovered game is shown, regardless of whether it's already
	// matched or skipped - this is a review/edit tool, not just an
	// initial-setup wizard. LoadGameState() shows each one's current
	// status and disables whatever isn't currently possible for it.
	g.games = allGames;
	g.artDir = artDir;
	g.injectorPath = injectorPath;
	g.currentIndex = 0;
	g.matchedGames.clear();

	static std::optional<SteamGridDb> clientStorage;
	clientStorage.reset();
	if (apiKey)
		clientStorage.emplace(*apiKey);
	g.client = clientStorage ? &*clientStorage : nullptr;

	ULONG_PTR gdiplusToken;
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
	InitCommonControlsEx(&icc);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"ReviveNextArtMatcher";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReviveNext " REVIVENEXT_VERSION_L L" - Manage Library",
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 900, 860, nullptr, nullptr, wc.hInstance, nullptr);

	CreateControls(g.hwnd);
	ShowWindow(g.hwnd, SW_SHOW);
	LoadGameState();

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	delete g.coverArt;
	delete g.currentArtPortrait;
	delete g.currentArtHome;
	if (g.imageListPortrait)
		ImageList_Destroy(g.imageListPortrait);
	if (g.imageListHome)
		ImageList_Destroy(g.imageListHome);
	Gdiplus::GdiplusShutdown(gdiplusToken);

	return std::move(g.matchedGames);
}

namespace
{
	constexpr int kIdEditApiKey = 201;
	constexpr int kIdBtnSaveKey = 202;
	constexpr int kIdBtnSkipKey = 203;
	constexpr int kIdLinkApiKey = 204;

	struct ApiKeyPromptState
	{
		HWND hwnd = nullptr;
		HWND editKey = nullptr;
		std::optional<std::string> result;
	} apiKeyState;

	bool RegWriteSZ(HKEY root, const wchar_t* subkey, const wchar_t* value, const std::wstring& data)
	{
		HKEY key;
		if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
			return false;
		LONG result = RegSetValueExW(key, value, 0, REG_SZ, (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
		return result == ERROR_SUCCESS;
	}

	std::optional<std::string> RegReadSZ(HKEY root, const wchar_t* subkey, const wchar_t* value)
	{
		HKEY key;
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
			return std::nullopt;

		DWORD type = 0, size = 0;
		if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0)
		{
			RegCloseKey(key);
			return std::nullopt;
		}

		std::wstring wresult(size / sizeof(wchar_t), L'\0');
		RegQueryValueExW(key, value, nullptr, nullptr, (LPBYTE)wresult.data(), &size);
		RegCloseKey(key);

		while (!wresult.empty() && wresult.back() == L'\0')
			wresult.pop_back();
		return wresult.empty() ? std::nullopt : std::make_optional(WideToUtf8(wresult));
	}

	LRESULT CALLBACK ApiKeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_COMMAND:
			if (LOWORD(wParam) == kIdBtnSaveKey && HIWORD(wParam) == BN_CLICKED)
			{
				wchar_t buf[256];
				GetWindowTextW(apiKeyState.editKey, buf, 256);
				std::string key = WideToUtf8(buf);
				if (!key.empty())
				{
					if (RegWriteSZ(HKEY_CURRENT_USER, L"Software\\ReviveNext", L"SteamGridDbApiKey", Utf8ToWide(key)))
						apiKeyState.result = key;
					else
						MessageBoxW(hwnd, L"Failed to save the key to the registry.", L"ReviveNext", MB_ICONERROR);
				}
				if (apiKeyState.result)
					DestroyWindow(hwnd);
			}
			else if (LOWORD(wParam) == kIdBtnSkipKey && HIWORD(wParam) == BN_CLICKED)
				DestroyWindow(hwnd);
			return 0;

		case WM_NOTIFY:
		{
			LPNMHDR hdr = (LPNMHDR)lParam;
			if (hdr->idFrom == kIdLinkApiKey && (hdr->code == NM_CLICK || hdr->code == NM_RETURN))
			{
				auto* link = (PNMLINK)lParam;
				ShellExecuteW(nullptr, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
			}
			return 0;
		}

		case WM_CLOSE:
			DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

std::optional<std::string> ArtMatcherGui::PromptForApiKey()
{
	apiKeyState = {};

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
	InitCommonControlsEx(&icc);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = ApiKeyWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"ReviveNextApiKeyPrompt";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	apiKeyState.hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReviveNext - SteamGridDB API Key",
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 520, 300, nullptr, nullptr, wc.hInstance, nullptr);

	HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

	auto existingKey = RegReadSZ(HKEY_CURRENT_USER, L"Software\\ReviveNext", L"SteamGridDbApiKey");

	HWND intro = CreateWindowExW(0, L"STATIC",
		L"ReviveNext uses SteamGridDB to find Library art (the portrait\r\n"
		L"cover images Steam's Library view shows) for your games.\r\n\r\n"
		L"Create a free account and generate an API key at:",
		WS_CHILD | WS_VISIBLE, 20, 20, 460, 80, apiKeyState.hwnd, nullptr, nullptr, nullptr);

	HWND link = CreateWindowExW(0, WC_LINK,
		L"<a href=\"https://www.steamgriddb.com/profile/preferences/api\">https://www.steamgriddb.com/profile/preferences/api</a>",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP,
		20, 100, 460, 20, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdLinkApiKey, nullptr, nullptr);

	HWND keyLabel = CreateWindowExW(0, L"STATIC",
		existingKey ? L"A key is already saved - paste a new one to replace it:" : L"Paste your API key here:",
		WS_CHILD | WS_VISIBLE, 20, 140, 460, 20, apiKeyState.hwnd, nullptr, nullptr, nullptr);
	apiKeyState.editKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", existingKey ? Utf8ToWide(*existingKey).c_str() : L"",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
		20, 162, 460, 26, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdEditApiKey, nullptr, nullptr);

	HWND btnSave = CreateWindowExW(0, L"BUTTON", L"Save",
		WS_CHILD | WS_VISIBLE, 300, 210, 85, 28, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdBtnSaveKey, nullptr, nullptr);
	HWND btnSkip = CreateWindowExW(0, L"BUTTON", existingKey ? L"Cancel" : L"Skip for now",
		WS_CHILD | WS_VISIBLE, 395, 210, 105, 28, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdBtnSkipKey, nullptr, nullptr);

	for (HWND h : { intro, link, keyLabel, apiKeyState.editKey, btnSave, btnSkip })
		SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);

	ShowWindow(apiKeyState.hwnd, SW_SHOW);
	SetFocus(apiKeyState.editKey);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return apiKeyState.result;
}
