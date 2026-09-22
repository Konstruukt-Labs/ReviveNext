#include "SteamVrAppRegistration.h"

#include <windows.h>
#include <TlHelp32.h>

#include <openvr.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	// VR_Init would auto-start the whole SteamVR stack (vrmonitor, HMD
	// wakeup) when it isn't already running - far too heavy for a sync
	// tool that may well be run at the desktop. Registration only needs
	// to happen occasionally anyway (SteamVR persists registered
	// applications in its own storage), so only connect when vrserver is
	// already up.
	bool IsSteamVrRunning()
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
				if (_wcsicmp(entry.szExeFile, L"vrserver.exe") == 0)
				{
					found = true;
					break;
				}
			} while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return found;
	}

	void ShowError(const std::string& message)
	{
		MessageBoxA(nullptr, message.c_str(), "ReviveNext", MB_ICONERROR);
	}

	// The matched art for a game in ReviveNext's own art storage, if any:
	// the portrait cover (<canonicalName>p<ext>) preferred over the
	// widescreen art (<canonicalName><ext>) - SteamVR's Library cards are
	// portrait-shaped like Steam's own capsules, so the 600x900 art is
	// what fits them.
	std::string FindArt(const std::string& canonicalName, const std::string& artDir)
	{
		constexpr const char* extensions[] = { ".png", ".jpg", ".jpeg", ".webp" };
		for (const char* suffix : { "p", "" })
		{
			const fs::path stem = fs::path(artDir) / (canonicalName + suffix);
			for (const char* ext : extensions)
			{
				std::error_code ec;
				if (fs::exists(stem.string() + ext, ec))
					return (stem.string() + ext);
			}
		}
		return {};
	}
}

void SteamVrAppRegistration::Sync(const std::vector<MetaGame>& games, const std::string& injectorPath,
	const std::string& artDir, bool quiet)
{
	if (games.empty())
		return;

	const fs::path runtimeDir = fs::path(injectorPath).parent_path();
	const fs::path manifestPath = runtimeDir / "revive.vrmanifest";
	const fs::path actionManifestPath = runtimeDir / "Input" / "action_manifest.json";

	nlohmann::json applications = nlohmann::json::array();
	for (const MetaGame& game : games)
	{
		const std::string exePath =
			(fs::path(game.libraryPath) / "Software" / game.canonicalName / game.launchFile).string();

		// Same argument shape as the game's launch line, so the registered
		// entry stays launch-capable on its own.
		std::string arguments = "/app " + game.canonicalName + " \"" + exePath + "\"";
		if (!game.launchParameters.empty())
			arguments += " " + game.launchParameters;

		nlohmann::json entry = {
			{ "app_key", "revive.app." + game.canonicalName },
			{ "launch_type", "binary" },
			{ "binary_path_windows", injectorPath },
			{ "arguments", arguments },
			{ "action_manifest_path", actionManifestPath.string() },
			// Visible in SteamVR's Library: launching from there is the
			// intended path, giving SteamVR's own app-launch flow
			// (loading screen while the game starts, dashboard on
			// exit). Steam's Library shortcuts remain alongside it.
			{ "is_internal", false },
			{ "strings", { { "en_us", { { "name", game.title } } } } },
		};

		std::string image = FindArt(game.canonicalName, artDir);
		if (image.empty())
			image = game.coverArtPath;
		if (!image.empty())
		{
			// Steam's registry-provided SteamPath uses forward slashes; normalize
			// the whole path to Windows separators so SteamVR's file-URL handling
			// can't stumble over the mixed form.
			entry["image_path"] = fs::path(image).make_preferred().string();
		}

		// AddApplicationManifest updates an existing entry with the same
		// app_key in place, so re-adding on every sync also refreshes
		// paths after a library move.
		applications.push_back(std::move(entry));
	}

	// Register this tool itself as a dashboard-overlay application,
	// hidden (is_internal), with /autoregister as its launch arguments,
	// and marked auto-launch below: SteamVR then starts a headless copy
	// of ReviveNext.exe every time SteamVR starts, which rescans the games
	// and refreshes this registration - see wWinMain in Main.cpp.
	// is_dashboard_overlay is what makes the entry eligible for
	// auto-launch: SteamVR only auto-starts overlay-type applications
	// (the same mechanism old Revive's always-running dashboard used).
	applications.push_back({
		{ "app_key", "revive.revenext" },
		{ "launch_type", "binary" },
		{ "binary_path_windows", (runtimeDir / "ReviveNext.exe").string() },
		{ "arguments", "/autoregister" },
		{ "is_dashboard_overlay", true },
		{ "is_internal", true },
		{ "strings", { { "en_us", { { "name", "ReviveNext" } } } } },
	});

	{
		std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
		out << nlohmann::json({ { "applications", applications } }).dump(4);
		if (!out)
		{
			ShowError("Failed to write " + manifestPath.string() + ".");
			return;
		}
	}

	// Registration itself needs SteamVR running (VR_Init would otherwise
	// auto-start the whole stack, far too heavy for a desktop sync run).
	// Registered entries persist in SteamVR's own storage, so this only
	// has to succeed occasionally - but skipping silently is how a run at
	// the desktop ends with SteamVR's entries stale until the next sync.
	if (!IsSteamVrRunning())
	{
		if (!quiet)
		{
			MessageBoxA(nullptr,
				"SteamVR isn't running, so its Library entries were not refreshed.\n"
				"Run ReviveNext again with SteamVR open whenever games or art change.",
				"ReviveNext", MB_ICONINFORMATION);
		}
		return;
	}

	vr::EVRInitError initError = vr::VRInitError_Unknown;
	vr::VR_Init(&initError, vr::VRApplication_Background);
	if (initError != vr::VRInitError_None)
		return;

	vr::EVRApplicationError appError =
		vr::VRApplications()->AddApplicationManifest(manifestPath.string().c_str());
	vr::VRApplications()->SetApplicationAutoLaunch("revive.revenext", true);
	if (appError != vr::VRApplicationError_None)
	{
		ShowError(std::string("Registering games with SteamVR failed: ") +
			vr::VRApplications()->GetApplicationsErrorNameFromEnum(appError));
	}

	vr::VR_Shutdown();
}

void SteamVrAppRegistration::Unregister(const std::string& injectorPath)
{
	if (!IsSteamVrRunning())
	{
		MessageBoxA(nullptr,
			"SteamVR isn't running. Removal needs a live connection to it -\n"
			"start SteamVR and run ReviveNext.exe /unregister again.",
			"ReviveNext", MB_ICONINFORMATION);
		return;
	}

	vr::EVRInitError initError = vr::VRInitError_Unknown;
	vr::VR_Init(&initError, vr::VRApplication_Background);
	if (initError != vr::VRInitError_None)
	{
		ShowError("Could not connect to SteamVR to remove the entries.");
		return;
	}

	// Disable the tool's auto-launch before removing its entry, so a
	// SteamVR restart can't immediately re-register everything via the
	// /autoregister hook mid-test.
	vr::VRApplications()->SetApplicationAutoLaunch("revive.revenext", false);

	const fs::path manifestPath = fs::path(injectorPath).parent_path() / "revive.vrmanifest";
	vr::EVRApplicationError appError =
		vr::VRApplications()->RemoveApplicationManifest(manifestPath.string().c_str());
	if (appError != vr::VRApplicationError_None)
	{
		ShowError(std::string("Removing the SteamVR entries failed: ") +
			vr::VRApplications()->GetApplicationsErrorNameFromEnum(appError));
	}
	else
	{
		MessageBoxA(nullptr,
			"ReviveNext's applications were removed from SteamVR.\n"
			"The Other Apps list updates immediately.",
			"ReviveNext", MB_ICONINFORMATION);
	}

	vr::VR_Shutdown();
}
