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
	constexpr int kThumbW = 96, kThumbH = 144;

	constexpr int kIdEditSearch = 101;
	constexpr int kIdBtnSearch = 102;
	constexpr int kIdListboxGames = 103;
	constexpr int kIdListviewGrids = 104;
	constexpr int kIdBtnBack = 105;
	constexpr int kIdBtnSkip = 106;
	constexpr int kIdBtnClose = 107;
	constexpr UINT_PTR kTimerCheckSteam = 1;

	enum class Mode { PickingGame, PickingGrid, WaitingForSteamClose, Done };

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

	// <gridDir>/<Steam-assigned appid>p - the filename Steam's Library
	// view looks for. The appid comes from Steam itself (see
	// SteamShortcuts::ReadAssignedAppIds), not anything ReviveNext
	// computes - Steam's own algorithm for it is undocumented and not
	// worth reverse-engineering when Steam will just tell us directly.
	std::string ComputeStem(uint32_t appid, const std::string& gridDir)
	{
		return (fs::path(gridDir) / (std::to_string(appid) + "p")).string();
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

	HBITMAP DownloadAndScaleThumbnail(const std::string& url)
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

		Gdiplus::Bitmap thumb(kThumbW, kThumbH, PixelFormat32bppARGB);
		Gdiplus::Graphics g(&thumb);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.DrawImage(&image, 0, 0, kThumbW, kThumbH);

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
		HWND finalLabel = nullptr, btnClose = nullptr;

		Gdiplus::Image* coverArt = nullptr;
		Gdiplus::Image* currentArt = nullptr; // the art file already saved for the game on screen, if any
		HIMAGELIST imageList = nullptr;

		std::vector<MetaGame> games;
		std::vector<MetaGame> allGames; // full original list, needed for the final re-sync
		size_t currentIndex = 0;
		std::string gridDir, injectorPath, shortcutsPath;
		std::unordered_map<std::string, uint32_t> appIds;
		SteamGridDb* client = nullptr;

		std::vector<SgdbGame> gameResults;
		std::vector<SgdbGrid> gridResults;
		std::string pickedGameName; // SteamGridDB's title for the game currently shown in the grid picker
		Mode mode = Mode::PickingGame;

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
		ShowWindow(g.listviewGrids, pickingGame ? SW_HIDE : SW_SHOW);
		ShowWindow(g.btnBack, pickingGame ? SW_HIDE : SW_SHOW);
	}

	void DoSearch()
	{
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

		SetStatus(L"Loading grid art...");
		SetCursor(LoadCursor(nullptr, IDC_WAIT));
		g.gridResults = g.client->GetPortraitGrids(g.gameResults[idx].id);
		if (g.gridResults.size() > 24)
			g.gridResults.resize(24);

		if (g.imageList)
		{
			ImageList_Destroy(g.imageList);
			g.imageList = nullptr;
		}
		g.imageList = ImageList_Create(kThumbW, kThumbH, ILC_COLOR32, 0, (int)g.gridResults.size() + 1);
		ListView_SetImageList(g.listviewGrids, g.imageList, LVSIL_NORMAL);
		ListView_DeleteAllItems(g.listviewGrids);

		for (size_t i = 0; i < g.gridResults.size(); ++i)
		{
			HBITMAP hbmp = DownloadAndScaleThumbnail(g.gridResults[i].url);
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

	// Re-syncs shortcuts.vdf with every currently-known title override
	// (not just this session's picks, so an earlier session's choices are
	// never lost) - only safe to call once Steam is confirmed closed.
	void FinishAndResync()
	{
		SetStatus(L"Applying titles...");

		auto overrides = TitleOverrides::Load();
		std::vector<MetaGame> finalGames = g.allGames;
		for (auto& game : finalGames)
			if (auto it = overrides.find(game.canonicalName); it != overrides.end())
				game.title = it->second;

		bool ok = SteamShortcuts::Sync(finalGames, g.injectorPath, g.shortcutsPath);

		g.mode = Mode::Done;
		SetWindowTextW(g.finalLabel, ok
			? L"Done! Titles updated. You can start Steam again."
			: L"Something went wrong writing shortcuts.vdf - check ReviveNext's console output.");
		SetStatus(L"");
		ShowWindow(g.btnClose, SW_SHOW);
	}

	void EnterWaitingState()
	{
		g.mode = Mode::WaitingForSteamClose;
		for (HWND h : { g.listboxGames, g.listviewGrids, g.btnBack, g.btnSkip, g.editSearch, g.btnSearch, g.titleLabel, g.counterLabel })
			ShowWindow(h, SW_HIDE);

		std::wstring msg = L"Matched " + std::to_wstring(g.matchedGames.size()) +
			L" game(s) this session.\r\n\r\n"
			L"To apply their titles in Steam's Library, fully close Steam now\r\n"
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
			if (g.matchedGames.empty())
				DestroyWindow(g.hwnd);
			else
				EnterWaitingState();
			return;
		}
		LoadGameState();
	}

	void ChooseGrid(int idx)
	{
		if (idx < 0 || (size_t)idx >= g.gridResults.size())
			return;

		MetaGame& game = g.games[g.currentIndex];
		uint32_t appid = g.appIds.at(game.canonicalName);
		std::string stem = ComputeStem(appid, g.gridDir);
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
			AdvanceToNext();
		}
		else
		{
			SetStatus(L"Download failed - try a different one.");
		}
	}

	void SkipCurrent()
	{
		uint32_t appid = g.appIds.at(g.games[g.currentIndex].canonicalName);
		std::string stem = ComputeStem(appid, g.gridDir);

		// Only actually marks-as-skipped (and only ever touches a file)
		// when nothing is there yet. Picking a NEW grid (ChooseGrid) is
		// the only action that overwrites existing art/skip state - this
		// button must never destroy a previous pick just by clicking past
		// an already-matched game to browse the rest.
		if (GetExistingState(stem) == ExistingState::None)
		{
			std::ofstream marker(stem + ".skip");
		}
		AdvanceToNext();
	}

	void LoadGameState()
	{
		const MetaGame& game = g.games[g.currentIndex];

		delete g.coverArt;
		g.coverArt = game.coverArtPath.empty() ? nullptr : new Gdiplus::Image(Utf8ToWide(game.coverArtPath).c_str());

		SetWindowTextW(g.titleLabel, Utf8ToWide(game.title).c_str());

		uint32_t appid = g.appIds.at(game.canonicalName);
		std::string stem = ComputeStem(appid, g.gridDir);
		ExistingState existing = GetExistingState(stem);

		delete g.currentArt;
		g.currentArt = nullptr;
		if (auto existingFile = FindExistingArtFile(stem))
			g.currentArt = new Gdiplus::Image(Utf8ToWide(*existingFile).c_str());

		const wchar_t* existingLabel = existing == ExistingState::Matched ? L" - already has art (picking a new one replaces it)"
			: existing == ExistingState::Skipped ? L" - previously skipped" : L"";
		std::wstring counter = std::to_wstring(g.currentIndex + 1) + L" / " + std::to_wstring(g.games.size()) + existingLabel;
		SetWindowTextW(g.counterLabel, counter.c_str());

		// The button never destroys existing art (see SkipCurrent) - its
		// label reflects that so it's clear clicking past an already-
		// handled game is always safe.
		SetWindowTextW(g.btnSkip, existing == ExistingState::None ? L"Skip this game" : L"Next (keep existing)");
		SetWindowTextW(g.editSearch, Utf8ToWide(game.title).c_str());

		InvalidateRect(g.hwnd, nullptr, TRUE);
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
			else if (LOWORD(wParam) == kIdBtnBack && HIWORD(wParam) == BN_CLICKED)
				SwitchMode(Mode::PickingGame);
			else if (LOWORD(wParam) == kIdBtnClose && HIWORD(wParam) == BN_CLICKED)
				DestroyWindow(hwnd);
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

			if (g.currentArt && g.currentArt->GetLastStatus() == Gdiplus::Ok)
				gr.DrawImage(g.currentArt, kCurrentArtX, kCurrentArtY, kCurrentArtW, kCurrentArtH);
			else
			{
				Gdiplus::SolidBrush brush(Gdiplus::Color(255, 240, 240, 240));
				gr.FillRectangle(&brush, kCurrentArtX, kCurrentArtY, kCurrentArtW, kCurrentArtH);
				Gdiplus::Font font(L"Segoe UI", 9);
				Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 120, 120, 120));
				Gdiplus::RectF rect((Gdiplus::REAL)kCurrentArtX + 8, (Gdiplus::REAL)kCurrentArtY + 8,
					(Gdiplus::REAL)kCurrentArtW - 16, (Gdiplus::REAL)kCurrentArtH - 16);
				gr.DrawString(L"No Library art\nselected yet", -1, &font, rect, nullptr, &textBrush);
			}
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_CLOSE:
			// Closing mid-session with unsaved picks routes through the
			// same "wait for Steam, then apply" flow rather than losing
			// them - the matcher's own job is done either way, so no
			// separate ReviveNext re-run should be needed to see them.
			if ((g.mode == Mode::PickingGame || g.mode == Mode::PickingGrid) && !g.matchedGames.empty())
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

		g.titleLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			kLabelsX, 50, 876 - kLabelsX, 24, hwnd, nullptr, nullptr, nullptr);
		g.counterLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
			kLabelsX, 76, 876 - kLabelsX, 20, hwnd, nullptr, nullptr, nullptr);

		g.editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
			kLabelsX, 106, 386, 26, hwnd, (HMENU)(INT_PTR)kIdEditSearch, nullptr, nullptr);
		g.btnSearch = CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE,
			kLabelsX + 396, 106, 90, 26, hwnd, (HMENU)(INT_PTR)kIdBtnSearch, nullptr, nullptr);

		g.listboxGames = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
			20, 320, 860, 260, hwnd, (HMENU)(INT_PTR)kIdListboxGames, nullptr, nullptr);

		g.listviewGrids = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | LVS_ICON | LVS_SINGLESEL,
			20, 320, 860, 260, hwnd, (HMENU)(INT_PTR)kIdListviewGrids, nullptr, nullptr);
		ListView_SetIconSpacing(g.listviewGrids, kThumbW + 16, kThumbH + 24);

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
				g.listviewGrids, g.btnBack, g.btnSkip, g.statusLabel, g.finalLabel, g.btnClose })
			setFont(h);
	}
}

std::vector<MetaGame> ArtMatcherGui::Run(const std::vector<MetaGame>& allGames,
	const std::unordered_map<std::string, uint32_t>& appIds,
	const std::string& injectorPath, const std::string& shortcutsPath,
	const std::string& gridDir, const std::string& apiKey)
{
	// Every game with an assigned appid is shown, regardless of whether
	// it's already matched or skipped - this is a review/edit tool, not
	// just an initial-setup wizard. LoadGameState() shows each one's
	// current status; only picking a NEW grid image overwrites existing
	// art (ChooseGrid) - clicking past an already-handled game never
	// touches it (SkipCurrent).
	std::vector<MetaGame> workList;
	int notYetAssigned = 0;
	for (auto& game : allGames)
	{
		if (appIds.find(game.canonicalName) == appIds.end())
		{
			notYetAssigned++;
			continue;
		}
		workList.push_back(game);
	}

	if (notYetAssigned > 0)
	{
		printf("%d game(s) don't have a Steam-assigned appid yet - launch Steam once (fully, then you can close it again) after syncing so it can assign one, then run ReviveNext again to match their Library art.\n", notYetAssigned);
	}

	if (workList.empty())
	{
		printf("Nothing new to match (already matched, skipped, or waiting on an appid from Steam).\n");
		return {};
	}

	std::error_code ec;
	fs::create_directories(gridDir, ec);

	g.games = std::move(workList);
	g.allGames = allGames;
	g.gridDir = gridDir;
	g.injectorPath = injectorPath;
	g.shortcutsPath = shortcutsPath;
	g.appIds = appIds;
	g.currentIndex = 0;
	g.matchedGames.clear();

	static SteamGridDb client(apiKey);
	g.client = &client;

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

	g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"ReviveNext - Match Library Art",
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

	HWND intro = CreateWindowExW(0, L"STATIC",
		L"ReviveNext uses SteamGridDB to find Library art (the portrait\r\n"
		L"cover images Steam's Library view shows) for your games.\r\n\r\n"
		L"Create a free account and generate an API key at:",
		WS_CHILD | WS_VISIBLE, 20, 20, 460, 80, apiKeyState.hwnd, nullptr, nullptr, nullptr);

	HWND link = CreateWindowExW(0, WC_LINK,
		L"<a href=\"https://www.steamgriddb.com/profile/preferences/api\">https://www.steamgriddb.com/profile/preferences/api</a>",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP,
		20, 100, 460, 20, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdLinkApiKey, nullptr, nullptr);

	HWND keyLabel = CreateWindowExW(0, L"STATIC", L"Paste your API key here:",
		WS_CHILD | WS_VISIBLE, 20, 140, 460, 20, apiKeyState.hwnd, nullptr, nullptr, nullptr);
	apiKeyState.editKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
		20, 162, 460, 26, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdEditApiKey, nullptr, nullptr);

	HWND btnSave = CreateWindowExW(0, L"BUTTON", L"Save",
		WS_CHILD | WS_VISIBLE, 300, 210, 85, 28, apiKeyState.hwnd, (HMENU)(INT_PTR)kIdBtnSaveKey, nullptr, nullptr);
	HWND btnSkip = CreateWindowExW(0, L"BUTTON", L"Skip for now",
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
