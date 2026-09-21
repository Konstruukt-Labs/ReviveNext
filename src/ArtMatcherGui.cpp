#include "ArtMatcherGui.h"
#include "SteamGridDb.h"
#include "SteamShortcuts.h"
#include "TitleOverrides.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace
{
	constexpr int kCoverX = 20, kCoverY = 50, kCoverW = 180, kCoverH = 180;
	constexpr int kCurrentArtX = 220, kCurrentArtY = 50, kCurrentArtW = 140, kCurrentArtH = 210;
	constexpr int kLabelsX = 380;
	// Portrait (Library cover) candidate thumbnails; widescreen (Home art)
	// candidates use a different box shape (see kThumbHomeW/H) to keep
	// their very different aspect ratio from looking squashed.
	constexpr int kThumbPortraitW = 96, kThumbPortraitH = 144;
	constexpr int kThumbHomeW = 160, kThumbHomeH = 75;

	constexpr int kIdEditSearch = 101;
	constexpr int kIdBtnSearch = 102;
	constexpr int kIdListboxGames = 103;
	constexpr int kIdListviewGrids = 104;
	constexpr int kIdBtnBack = 105;
	constexpr int kIdBtnSkip = 106;
	constexpr int kIdBtnClose = 107;
	constexpr int kIdBtnSettings = 108;
	constexpr int kIdBtnArtKind = 109;
	constexpr int kIdBtnPrev = 110;
	constexpr int kIdBtnNext = 111;
	constexpr UINT_PTR kTimerCheckSteam = 1;

	enum class Mode { PickingGame, PickingGrid, WaitingForSteamClose, Done };

	// Which of the two Steam-recognized art shapes is currently being
	// matched - see SteamGridDb::GetPortraitGrids/GetWidescreenGrids and
	// ComputeStem for what each one is keyed and searched by.
	enum class ArtKind { Portrait, Home };

	bool IsProcessRunning(const wchar_t* exeName)
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
			return false;

		bool found = false;
		PROCESSENTRY32W entry = { sizeof(entry) };
		if (Process32FirstW(snapshot, &entry))
		{
			do
			{
				if (_wcsicmp(entry.szExeFile, exeName) == 0)
				{
					found = true;
					break;
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return found;
	}

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

	// <gridDir>/<Steam-assigned appid>p for the Library portrait cover,
	// <gridDir>/<Steam-assigned appid> (no suffix) for the widescreen Home
	// art - both filenames Steam itself looks for. The appid comes from
	// Steam (see SteamShortcuts::ReadAssignedAppIds), not anything
	// ReviveNext computes - Steam's own algorithm for it is undocumented
	// and not worth reverse-engineering when Steam will just tell us
	// directly.
	std::string ComputeStem(uint32_t appid, const std::string& gridDir, ArtKind kind)
	{
		std::string name = std::to_string(appid) + (kind == ArtKind::Portrait ? "p" : "");
		return (fs::path(gridDir) / name).string();
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

	HBITMAP DownloadAndScaleThumbnail(const std::string& url, int thumbW, int thumbH)
	{
		auto bytes = SteamGridDb::DownloadBytes(url);
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
		HWND editSearch = nullptr, btnSearch = nullptr, listboxGames = nullptr, listviewGrids = nullptr;
		HWND btnBack = nullptr, btnSkip = nullptr, titleLabel = nullptr, counterLabel = nullptr, statusLabel = nullptr;
		HWND finalLabel = nullptr, btnClose = nullptr, btnSettings = nullptr;
		HWND btnPrev = nullptr, btnNext = nullptr, btnArtKind = nullptr;

		Gdiplus::Image* coverArt = nullptr;
		Gdiplus::Image* currentArt = nullptr; // the art file already saved for the game on screen, if any
		HIMAGELIST imageList = nullptr;

		std::vector<MetaGame> games;
		std::vector<MetaGame> allGames; // full original list, needed for the final re-sync
		size_t currentIndex = 0;
		std::string gridDir, injectorPath, shortcutsPath;
		std::unordered_map<std::string, uint32_t> appIds;
		SteamGridDb* client = nullptr; // null when no API key is configured - search is unavailable, not the whole window

		std::vector<SgdbGame> gameResults;
		std::vector<SgdbGrid> gridResults;
		std::string pickedGameName; // SteamGridDB's title for the game currently shown in the grid picker
		Mode mode = Mode::PickingGame;
		ArtKind artKind = ArtKind::Portrait; // which shape is currently being matched/reviewed

		std::vector<MetaGame> matchedGames; // returned to the caller when the window closes
		bool hasPendingChanges = false; // matched art this session - needs the Steam-closed resync to apply titles

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
		ShowWindow(g.listviewGrids, pickingGame ? SW_HIDE : SW_SHOW);
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

	void ShowGridsForGame(int idx)
	{
		if (idx < 0 || (size_t)idx >= g.gameResults.size())
			return;

		g.pickedGameName = g.gameResults[idx].name;
		bool portrait = g.artKind == ArtKind::Portrait;
		int thumbW = portrait ? kThumbPortraitW : kThumbHomeW;
		int thumbH = portrait ? kThumbPortraitH : kThumbHomeH;

		SetStatus(L"Loading grid art...");
		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		g.gridResults = portrait ? g.client->GetPortraitGrids(g.gameResults[idx].id)
								  : g.client->GetWidescreenGrids(g.gameResults[idx].id);
		if (g.gridResults.size() > 24)
			g.gridResults.resize(24);

		if (g.imageList)
		{
			ImageList_Destroy(g.imageList);
			g.imageList = nullptr;
		}
		g.imageList = ImageList_Create(thumbW, thumbH, ILC_COLOR32, 0, (int)g.gridResults.size() + 1);
		ListView_SetImageList(g.listviewGrids, g.imageList, LVSIL_NORMAL);
		ListView_SetIconSpacing(g.listviewGrids, thumbW + 16, thumbH + 24);
		ListView_DeleteAllItems(g.listviewGrids);

		for (size_t i = 0; i < g.gridResults.size(); ++i)
		{
			HBITMAP hbmp = DownloadAndScaleThumbnail(g.gridResults[i].url, thumbW, thumbH);
			int imageIndex = -1;
			if (hbmp)
			{
				imageIndex = ImageList_Add(g.imageList, hbmp, nullptr);
				DeleteObject(hbmp);
			}

			std::wstring label = std::to_wstring(g.gridResults[i].width) + L"x" + std::to_wstring(g.gridResults[i].height);
			LVITEMW item = {};
			item.mask = LVIF_TEXT | LVIF_IMAGE;
			item.iItem = (int)i;
			item.pszText = label.data();
			item.iImage = imageIndex;
			ListView_InsertItem(g.listviewGrids, &item);
		}
		SetCursor(LoadCursor(nullptr, IDC_ARROW));

		SwitchMode(Mode::PickingGrid);
		SetStatus(g.gridResults.empty() ? L"No portrait grid art submitted for that game yet." : L"Double-click one to use it.");
	}

	void LoadGameState();

	// Re-syncs shortcuts.vdf with every currently-known title override (not
	// just this session's, so an earlier session's picks are never lost).
	// Only safe to call once Steam is confirmed closed.
	void FinishAndResync()
	{
		SetStatus(L"Applying changes...");

		auto overrides = TitleOverrides::Load();
		std::vector<MetaGame> finalGames = g.allGames;
		for (auto& game : finalGames)
			if (auto it = overrides.find(game.canonicalName); it != overrides.end())
				game.title = it->second;

		bool ok = SteamShortcuts::Sync(finalGames, g.injectorPath, g.shortcutsPath);

		g.mode = Mode::Done;
		SetWindowTextW(g.finalLabel, ok
			? L"Done! Your changes are applied. You can start Steam again."
			: L"Something went wrong writing shortcuts.vdf - check ReviveNext's console output.");
		SetStatus(L"");
		ShowWindow(g.btnClose, SW_SHOW);
	}

	void EnterWaitingState()
	{
		g.mode = Mode::WaitingForSteamClose;
		for (HWND h : { g.listboxGames, g.listviewGrids, g.btnBack, g.btnSkip, g.btnPrev, g.btnNext,
				g.editSearch, g.btnSearch, g.titleLabel, g.counterLabel, g.btnSettings, g.btnArtKind })
			ShowWindow(h, SW_HIDE);

		std::wstring msg =
			L"You made changes this session.\r\n\r\n"
			L"To apply them to Steam's Library, fully close Steam now\r\n"
			L"(right-click its tray icon → Exit - just closing the window isn't enough).\r\n\r\n"
			L"This will update automatically once Steam has closed, and you can\r\n"
			L"start Steam again right after.";
		SetWindowTextW(g.finalLabel, msg.c_str());
		ShowWindow(g.finalLabel, SW_SHOW);
		SetStatus(L"Waiting for Steam to close...");

		SetTimer(g.hwnd, kTimerCheckSteam, 1000, nullptr);
	}

	void AdvanceToNext()
	{
		g.currentIndex++;
		if (g.currentIndex >= g.games.size())
		{
			if (g.hasPendingChanges)
				EnterWaitingState();
			else
				DestroyWindow(g.hwnd);
			return;
		}
		LoadGameState();
	}

	// Pure browsing, unlike AdvanceToNext - never triggers the end-of-list
	// finish flow, just moves the cursor. Lets the user jump straight back
	// to one specific game (e.g. one just re-added to the Library, once
	// Steam has assigned it a fresh appid) without re-walking the list.
	void GoToPrev()
	{
		if (g.currentIndex > 0)
		{
			g.currentIndex--;
			LoadGameState();
		}
	}

	void GoToNext()
	{
		if (g.currentIndex + 1 < g.games.size())
		{
			g.currentIndex++;
			LoadGameState();
		}
	}

	void ChooseGrid(int idx)
	{
		if (idx < 0 || (size_t)idx >= g.gridResults.size())
			return;

		MetaGame& game = g.games[g.currentIndex];
		auto appIdIt = g.appIds.find(game.canonicalName);
		if (appIdIt == g.appIds.end())
			return; // shouldn't be reachable - the picker UI is hidden without an appid
		std::string stem = ComputeStem(appIdIt->second, g.gridDir, g.artKind);
		std::string ext = fs::path(g.gridResults[idx].url).extension().string();
		if (ext.empty())
			ext = ".png";

		// Overwrite whatever was here before (previously-matched art under
		// a different extension, or a skip marker) rather than leaving it
		// alongside the new pick.
		ClearExisting(stem);

		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		bool ok = SteamGridDb::DownloadFile(g.gridResults[idx].url, stem + ext);
		SetCursor(LoadCursor(nullptr, IDC_ARROW));

		if (ok)
		{
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
			g.hasPendingChanges = true;
			AdvanceToNext();
		}
		else
		{
			SetStatus(L"Download failed - try a different one.");
		}
	}

	void SkipCurrent()
	{
		// Only actually marks-as-skipped (and only ever touches a file)
		// when nothing is there yet, and only when there's an appid to key
		// it under. Picking a NEW grid (ChooseGrid) is the only action
		// that overwrites existing art/skip state - this button must
		// never destroy a previous pick just by clicking past an
		// already-matched game to browse the rest.
		auto appIdIt = g.appIds.find(g.games[g.currentIndex].canonicalName);
		if (appIdIt != g.appIds.end())
		{
			std::string stem = ComputeStem(appIdIt->second, g.gridDir, g.artKind);
			if (GetExistingState(stem) == ExistingState::None)
			{
				std::ofstream marker(stem + ".skip");
			}
		}
		AdvanceToNext();
	}

	void LoadGameState();

	// Lighter than a full LoadGameState() - only checks whether an appid
	// has newly appeared for the game on screen, and only does anything
	// (a full refresh) if it has. Called on refocus rather than
	// LoadGameState() directly so alt-tabbing back from Steam doesn't
	// wipe a search term the user was mid-typing when nothing actually
	// changed.
	void RefreshOnFocus()
	{
		if (g.games.empty() || (g.mode != Mode::PickingGame && g.mode != Mode::PickingGrid))
			return;

		const std::string& name = g.games[g.currentIndex].canonicalName;
		if (g.appIds.count(name))
			return;

		auto fresh = SteamShortcuts::ReadAssignedAppIds(g.shortcutsPath);
		auto it = fresh.find(name);
		if (it != fresh.end())
		{
			g.appIds[name] = it->second;
			LoadGameState();
		}
	}

	void LoadGameState()
	{
		const MetaGame& game = g.games[g.currentIndex];

		EnableWindow(g.btnPrev, g.currentIndex > 0);
		EnableWindow(g.btnNext, g.currentIndex + 1 < g.games.size());

		delete g.coverArt;
		g.coverArt = game.coverArtPath.empty() ? nullptr : new Gdiplus::Image(Utf8ToWide(game.coverArtPath).c_str());

		SetWindowTextW(g.titleLabel, Utf8ToWide(game.title).c_str());

		// appIds is otherwise just a snapshot from when the window opened,
		// so it never learns about an id Steam assigns later in the same
		// session (e.g. the user cycles Steam - possibly more than once,
		// since a freshly re-added game needs a sync to exist before Steam
		// can assign anything - while this window stays open). Re-checking
		// shortcuts.vdf is cheap, so do it live whenever the game on
		// screen doesn't have one yet, rather than requiring a fresh
		// ReviveNext run to notice.
		if (g.appIds.find(game.canonicalName) == g.appIds.end())
		{
			auto fresh = SteamShortcuts::ReadAssignedAppIds(g.shortcutsPath);
			auto it = fresh.find(game.canonicalName);
			if (it != fresh.end())
				g.appIds[game.canonicalName] = it->second;
		}

		auto appIdIt = g.appIds.find(game.canonicalName);
		bool hasAppId = appIdIt != g.appIds.end();
		bool canMatchArt = hasAppId && g.client != nullptr;

		ExistingState existing = ExistingState::None;
		std::string stem;
		if (hasAppId)
		{
			stem = ComputeStem(appIdIt->second, g.gridDir, g.artKind);
			existing = GetExistingState(stem);
		}

		delete g.currentArt;
		g.currentArt = nullptr;
		if (hasAppId)
			if (auto existingFile = FindExistingArtFile(stem))
				g.currentArt = new Gdiplus::Image(Utf8ToWide(*existingFile).c_str());

		const wchar_t* kindName = g.artKind == ArtKind::Portrait ? L"Library cover" : L"Home art";
		std::wstring counterExtra;
		if (existing == ExistingState::Matched)
			counterExtra += std::wstring(L" - ") + kindName + L" already set (picking a new one replaces it)";
		else if (existing == ExistingState::Skipped)
			counterExtra += std::wstring(L" - ") + kindName + L" previously skipped";
		std::wstring counter = std::to_wstring(g.currentIndex + 1) + L" / " + std::to_wstring(g.games.size()) + counterExtra;
		SetWindowTextW(g.counterLabel, counter.c_str());

		SetWindowTextW(g.btnArtKind, g.artKind == ArtKind::Portrait
			? L"Matching: Library cover (switch to Home art)"
			: L"Matching: Home art (switch to Library cover)");

		// Art matching needs both an API key (to search) and a
		// Steam-assigned appid (to know what file to save to) - hide the
		// whole search/pick flow and explain why when either is missing,
		// rather than leaving nonfunctional controls visible.
		for (HWND h : { g.editSearch, g.btnSearch, g.listboxGames, g.listviewGrids, g.btnBack, g.btnArtKind })
			ShowWindow(h, SW_HIDE);
		ShowWindow(g.btnArtKind, hasAppId ? SW_SHOW : SW_HIDE);
		if (canMatchArt)
		{
			for (HWND h : { g.editSearch, g.btnSearch })
				ShowWindow(h, SW_SHOW);
			SwitchMode(Mode::PickingGame);
		}
		else
		{
			g.mode = Mode::PickingGame;
			SetStatus(!hasAppId
				? L"Steam hasn't assigned this an id yet - launch Steam once after syncing to enable Library art matching."
				: L"Set a SteamGridDB API key (Settings...) to enable Library art matching.");
		}

		// The button never destroys existing art (see SkipCurrent) - its
		// label reflects that so it's clear clicking past an already-
		// handled game is always safe.
		SetWindowTextW(g.btnSkip, existing == ExistingState::None ? L"Skip this game" : L"Next (keep existing)");
		SetWindowTextW(g.editSearch, Utf8ToWide(game.title).c_str());

		InvalidateRect(g.hwnd, nullptr, TRUE);
		if (canMatchArt)
			DoSearch();
	}

	// Switches which of the two art shapes (Library cover vs. Home art)
	// is being matched for the game on screen, then refreshes so the
	// status text, current-art preview, and search results all reflect
	// the newly selected kind.
	void ToggleArtKind()
	{
		g.artKind = g.artKind == ArtKind::Portrait ? ArtKind::Home : ArtKind::Portrait;
		LoadGameState();
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
			else if (LOWORD(wParam) == kIdBtnArtKind && HIWORD(wParam) == BN_CLICKED)
				ToggleArtKind();
			else if (LOWORD(wParam) == kIdBtnPrev && HIWORD(wParam) == BN_CLICKED)
				GoToPrev();
			else if (LOWORD(wParam) == kIdBtnNext && HIWORD(wParam) == BN_CLICKED)
				GoToNext();
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
			else if (LOWORD(wParam) == kIdListboxGames && HIWORD(wParam) == LBN_DBLCLK)
			{
				int sel = (int)SendMessageW(g.listboxGames, LB_GETCURSEL, 0, 0);
				ShowGridsForGame(sel);
			}
			return 0;

		case WM_TIMER:
			if (wParam == kTimerCheckSteam && !IsProcessRunning(L"steam.exe"))
			{
				KillTimer(hwnd, kTimerCheckSteam);
				FinishAndResync();
			}
			return 0;

		case WM_ACTIVATE:
			// Regaining focus (e.g. the user switched to Steam to cycle
			// it, then alt-tabbed back here) is exactly when a
			// Steam-assigned appid is likely to have just appeared -
			// check for that instead of leaving the game on screen stuck
			// showing "no appid yet" until the user happens to navigate
			// away and back.
			if (LOWORD(wParam) != WA_INACTIVE)
				RefreshOnFocus();
			return 0;

		case WM_NOTIFY:
		{
			LPNMHDR hdr = (LPNMHDR)lParam;
			if (hdr->idFrom == kIdListviewGrids && hdr->code == NM_DBLCLK)
			{
				auto* activate = (LPNMITEMACTIVATE)lParam;
				if (activate->iItem >= 0)
					ChooseGrid(activate->iItem);
			}
			return 0;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			Gdiplus::Graphics gr(hdc);
			if (g.coverArt && g.coverArt->GetLastStatus() == Gdiplus::Ok)
				gr.DrawImage(g.coverArt, kCoverX, kCoverY, kCoverW, kCoverH);
			else
			{
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 220, 220, 220));
				gr.FillRectangle(&brush, kCoverX, kCoverY, kCoverW, kCoverH);
			}

			Gdiplus::SolidBrush currentArtBg(Gdiplus::Color(255, 240, 240, 240));
			gr.FillRectangle(&currentArtBg, kCurrentArtX, kCurrentArtY, kCurrentArtW, kCurrentArtH);
			if (g.currentArt && g.currentArt->GetLastStatus() == Gdiplus::Ok)
			{
				// Contain-fit rather than stretch: Library covers (600x900)
				// and Home art (920x430) have very different aspect ratios,
				// and this same box is reused for both depending on
				// g.artKind - stretching either into a fixed 140x210 box
				// would visibly distort it.
				double imgW = g.currentArt->GetWidth(), imgH = g.currentArt->GetHeight();
				double scaleW = kCurrentArtW / imgW, scaleH = kCurrentArtH / imgH;
				double scale = scaleW < scaleH ? scaleW : scaleH; // windows.h's min macro shadows std::min here
				int drawW = (int)(imgW * scale), drawH = (int)(imgH * scale);
				int drawX = kCurrentArtX + (kCurrentArtW - drawW) / 2;
				int drawY = kCurrentArtY + (kCurrentArtH - drawH) / 2;
				gr.DrawImage(g.currentArt, drawX, drawY, drawW, drawH);
			}
			else
			{
				Gdiplus::Font font(L"Segoe UI", 9);
				Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 120, 120, 120));
				Gdiplus::RectF rect((Gdiplus::REAL)kCurrentArtX + 8, (Gdiplus::REAL)kCurrentArtY + 8,
					(Gdiplus::REAL)kCurrentArtW - 16, (Gdiplus::REAL)kCurrentArtH - 16);
				const wchar_t* text = g.artKind == ArtKind::Portrait
					? L"No Library art\nselected yet"
					: L"No Home art\nselected yet";
				gr.DrawString(text, -1, &font, rect, nullptr, &textBrush);
			}
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_CLOSE:
			// Closing mid-session with unsaved picks routes through the
			// same "wait for Steam, then apply" flow rather than losing
			// them - the matcher's own job is done either way, so no
			// separate ReviveNext re-run should be needed to see them.
			if ((g.mode == Mode::PickingGame || g.mode == Mode::PickingGrid) && g.hasPendingChanges)
				EnterWaitingState();
			else
				DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
			if (g.mode == Mode::WaitingForSteamClose)
				KillTimer(hwnd, kTimerCheckSteam);
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	void CreateControls(HWND hwnd)
	{
		HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		auto setFont = [&](HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE); };

		g.btnSettings = CreateWindowExW(0, L"BUTTON", L"Settings...", WS_CHILD | WS_VISIBLE,
			760, 16, 120, 26, hwnd, (HMENU)(INT_PTR)kIdBtnSettings, nullptr, nullptr);

		g.titleLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			kLabelsX, 50, 876 - kLabelsX, 24, hwnd, nullptr, nullptr, nullptr);
		g.counterLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			kLabelsX, 76, 300, 20, hwnd, nullptr, nullptr, nullptr);
		g.btnPrev = CreateWindowExW(0, L"BUTTON", L"< Prev", WS_CHILD | WS_VISIBLE,
			kLabelsX + 306, 72, 80, 24, hwnd, (HMENU)(INT_PTR)kIdBtnPrev, nullptr, nullptr);
		g.btnNext = CreateWindowExW(0, L"BUTTON", L"Next >", WS_CHILD | WS_VISIBLE,
			kLabelsX + 392, 72, 80, 24, hwnd, (HMENU)(INT_PTR)kIdBtnNext, nullptr, nullptr);

		g.editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
			kLabelsX, 106, 386, 26, hwnd, (HMENU)(INT_PTR)kIdEditSearch, nullptr, nullptr);
		g.btnSearch = CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE,
			kLabelsX + 396, 106, 90, 26, hwnd, (HMENU)(INT_PTR)kIdBtnSearch, nullptr, nullptr);
		g.btnArtKind = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE,
			kLabelsX, 144, 486, 26, hwnd, (HMENU)(INT_PTR)kIdBtnArtKind, nullptr, nullptr);

		g.listboxGames = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
			20, 320, 860, 260, hwnd, (HMENU)(INT_PTR)kIdListboxGames, nullptr, nullptr);

		g.listviewGrids = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | LVS_ICON | LVS_SINGLESEL,
			20, 320, 860, 260, hwnd, (HMENU)(INT_PTR)kIdListviewGrids, nullptr, nullptr);

		g.btnBack = CreateWindowExW(0, L"BUTTON", L"< Back to search results", WS_CHILD | WS_VISIBLE,
			20, 590, 200, 28, hwnd, (HMENU)(INT_PTR)kIdBtnBack, nullptr, nullptr);
		g.btnSkip = CreateWindowExW(0, L"BUTTON", L"Skip this game", WS_CHILD | WS_VISIBLE,
			760, 590, 120, 28, hwnd, (HMENU)(INT_PTR)kIdBtnSkip, nullptr, nullptr);

		g.statusLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			20, 626, 860, 24, hwnd, nullptr, nullptr, nullptr);

		g.finalLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
			20, 320, 860, 220, hwnd, nullptr, nullptr, nullptr);
		g.btnClose = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD,
			760, 590, 120, 28, hwnd, (HMENU)(INT_PTR)kIdBtnClose, nullptr, nullptr);

		for (HWND h : { g.titleLabel, g.counterLabel, g.editSearch, g.btnSearch, g.listboxGames,
				g.listviewGrids, g.btnBack, g.btnSkip, g.btnPrev, g.btnNext,
				g.statusLabel, g.finalLabel, g.btnClose, g.btnSettings, g.btnArtKind })
			setFont(h);
	}
}

std::vector<MetaGame> ArtMatcherGui::Run(const std::vector<MetaGame>& allGames,
	const std::unordered_map<std::string, uint32_t>& appIds,
	const std::string& injectorPath, const std::string& shortcutsPath,
	const std::string& gridDir, const std::optional<std::string>& apiKey)
{
	if (allGames.empty())
		return {};

	std::error_code ec;
	fs::create_directories(gridDir, ec);

	// Every discovered game is shown, regardless of whether it's already
	// matched, skipped, or has no Steam-assigned appid yet - this is a
	// review/edit tool, not just an initial-setup wizard. LoadGameState()
	// shows each one's current status and disables whatever isn't
	// currently possible for it.
	g.games = allGames;
	g.allGames = allGames;
	g.gridDir = gridDir;
	g.injectorPath = injectorPath;
	g.shortcutsPath = shortcutsPath;
	g.appIds = appIds;
	g.currentIndex = 0;
	g.matchedGames.clear();
	g.hasPendingChanges = false;
	g.artKind = ArtKind::Portrait;

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
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	RegisterClassW(&wc);

	g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReviveNext - Manage Library",
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 900, 700, nullptr, nullptr, wc.hInstance, nullptr);

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
	delete g.currentArt;
	if (g.imageList)
		ImageList_Destroy(g.imageList);
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
