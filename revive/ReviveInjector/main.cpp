#include <string>
#include <codecvt>
#include <vector>

#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <Shlobj.h>
#include <Shlwapi.h>
#include <TlHelp32.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#include <openvr.h>
#include <detours/detours.h>
#include "../version.h"

extern FILE* g_LogFile;
#define LOG(x, ...) do { if (g_LogFile) { fprintf(g_LogFile, x, __VA_ARGS__); fflush(g_LogFile); } \
					printf(x, __VA_ARGS__); } while(0)

FILE* g_LogFile = NULL;

// Processes the injector should never try to hook. The injector runs
// against every process launch while its SteamVR driver is active, and
// without this list it will occasionally attempt (harmlessly, but
// noisily/slowly) to inject into browsers, chat apps, launchers, etc.
static const wchar_t* const s_BlockedProcesses[] = {
	L"roblox", L"robloxplayerbeta", L"robloxstudiobeta",
	L"chrome", L"firefox", L"msedge", L"opera", L"brave",
	L"steam", L"steamwebhelper",
	L"discord", L"spotify",
	L"explorer",
};

// Returns true if cmdLine's executable basename (case-insensitive, extension
// and any trailing arguments stripped) matches an entry in s_BlockedProcesses.
static bool IsBlockedProcess(const wchar_t* cmdLine)
{
	const wchar_t* start = wcsrchr(cmdLine, L'\\');
	start = start ? start + 1 : cmdLine;

	wchar_t base[MAX_PATH];
	wcsncpy(base, start, MAX_PATH - 1);
	base[MAX_PATH - 1] = L'\0';

	wchar_t* sp = wcschr(base, L' ');
	if (sp) *sp = L'\0';
	wchar_t* dot = wcsrchr(base, L'.');
	if (dot) *dot = L'\0';

	for (const wchar_t* name : s_BlockedProcesses)
	{
		if (_wcsicmp(base, name) == 0)
			return true;
	}
	return false;
}

// Reads a REG_SZ value, returning an empty string if the key/value is
// missing or of the wrong type. Used only for the optional manual overrides
// below - none of this is required for normal operation.
static std::wstring RegReadSZ(HKEY root, LPCWSTR subkey, LPCWSTR value, DWORD flags = KEY_READ)
{
	HKEY key;
	if (RegOpenKeyExW(root, subkey, 0, flags, &key) != ERROR_SUCCESS)
		return L"";
	DWORD type = 0, size = 0;
	if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0)
	{
		RegCloseKey(key);
		return L"";
	}
	std::wstring result(size / sizeof(wchar_t), L'\0');
	RegQueryValueExW(key, value, nullptr, nullptr, (PBYTE)result.data(), &size);
	RegCloseKey(key);
	while (!result.empty() && result.back() == L'\0')
		result.pop_back();
	return result;
}

// True if a process named exeName (e.g. L"OVRServer_x64.exe") is currently running.
static bool IsProcessRunning(const wchar_t* exeName)
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

bool GetOculusBasePath(PWCHAR path, DWORD length)
{
	LONG error = ERROR_SUCCESS;

	HKEY oculusKey;
	error = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\Oculus VR, LLC\\Oculus", 0, KEY_READ | KEY_WOW64_32KEY, &oculusKey);
	if (error != ERROR_SUCCESS)
	{
		LOG("Unable to open Oculus key.");
		return false;
	}
	error = RegQueryValueEx(oculusKey, L"Base", NULL, NULL, (PBYTE)path, &length);
	if (error != ERROR_SUCCESS)
	{
		LOG("Unable to read Base path.");
		return false;
	}
	RegCloseKey(oculusKey);

	return true;
}

// True when path is a volume GUID path in either Win32 extended
// ("\\?\Volume{...}") or NT object manager ("\??\Volume{...}") syntax.
static bool IsVolumeGuidPath(const wchar_t* path)
{
	if (!path)
		return false;

	return path[0] == L'\\' &&
		(path[1] == L'\\' || path[1] == L'?') &&
		path[2] == L'?' &&
		path[3] == L'\\' &&
		_wcsnicmp(path + 4, L"Volume{", 7) == 0;
}

// Resolves an Oculus library "Path" registry value - a volume GUID path plus
// sub-path (e.g. "\\?\Volume{...}\Software\...") - to a mounted path.
// Non-GUID values pass through unchanged, and volumes that fail to resolve
// fall back to the raw value instead of leaving the output empty. Ported
// from Revived PR #1 (ThePhaseless); the sub-path append is bounded by the
// space actually left in outPath rather than the full buffer length.
static bool ResolveVolumePath(const wchar_t* volumePath, wchar_t* outPath, DWORD length)
{
	if (!volumePath || !outPath || length == 0)
		return false;

	if (IsVolumeGuidPath(volumePath) && wcslen(volumePath) >= 49)
	{
		// Volume GUID strings are 49 chars: \\?\Volume{guid}\. The volume
		// APIs strictly require the "\\?\" prefix, so normalize NT "\??\".
		DWORD total = 0;
		WCHAR volume[50] = L"\\\\?\\";
		wcsncpy(volume + 4, volumePath + 4, 45);
		volume[49] = L'\0';

		outPath[0] = L'\0';
		if (GetVolumePathNamesForVolumeNameW(volume, outPath, length, &total) && outPath[0] != L'\0')
		{
			const wchar_t* subPath = volumePath + 49;
			while (*subPath == L'\\' || *subPath == L'/')
				subPath++;
			size_t used = wcslen(outPath);
			wcsncat(outPath, subPath, length - used - 1);
		}
		else
		{
			wcsncpy(outPath, volumePath, length - 1);
			outPath[length - 1] = L'\0';
		}
	}
	else
	{
		wcsncpy(outPath, volumePath, length - 1);
		outPath[length - 1] = L'\0';
	}
	return true;
}

bool GetLibraryPath(PWCHAR path, DWORD length, PWCHAR guid)
{
	LONG error = ERROR_SUCCESS;

	// Open the libraries key
	WCHAR keyPath[MAX_PATH] = { L"Software\\Oculus VR, LLC\\Oculus\\Libraries\\" };
	HKEY oculusKey;

	// Open the library key
	wcsncat(keyPath, guid, MAX_PATH);
	error = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &oculusKey);
	if (error != ERROR_SUCCESS)
	{
		LOG("Unable to open Library path key.");
		return false;
	}

	// Get the volume path to this library
	DWORD pathSize;
	error = RegQueryValueExW(oculusKey, L"Path", NULL, NULL, NULL, &pathSize);
	PWCHAR volumePath = (PWCHAR)malloc(pathSize);
	error = RegQueryValueExW(oculusKey, L"Path", NULL, NULL, (PBYTE)volumePath, &pathSize);
	RegCloseKey(oculusKey);
	if (error != ERROR_SUCCESS)
	{
		free(volumePath);
		LOG("Unable to read Library path.");
		return false;
	}

	// Resolve the volume path to a mount point
	ResolveVolumePath(volumePath, path, length);
	free(volumePath);

	return true;
}

bool GetDefaultLibraryPath(PWCHAR path, DWORD length)
{
	LONG error = ERROR_SUCCESS;

	// Open the libraries key
	WCHAR keyPath[MAX_PATH] = { L"Software\\Oculus VR, LLC\\Oculus\\Libraries\\" };
	HKEY oculusKey;
	error = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &oculusKey);
	if (error != ERROR_SUCCESS)
	{
		LOG("Unable to open Libraries key.");
		return false;
	}

	// Get the default library
	WCHAR guid[40] = { L'\0' };
	DWORD guidSize = sizeof(guid);
	error = RegQueryValueExW(oculusKey, L"DefaultLibrary", NULL, NULL, (PBYTE)guid, &guidSize);
	RegCloseKey(oculusKey);
	if (error != ERROR_SUCCESS)
	{
		LOG("Unable to read DefaultLibrary guid.");
		return false;
	}

	return GetLibraryPath(path, length, guid);
}

class StringArray
{
public:
	void add(const std::string& str)
	{
		strings.push_back(str);
		ptrs.push_back(strings.back().c_str());
	}

	void clear()
	{
		strings.clear();
		ptrs.clear();
	}

	const char** c_str()
	{
		return ptrs.data();
	}

	bool empty()
	{
		return ptrs.empty();
	}

	size_t size()
	{
		return ptrs.size();
	}

private:
	std::vector<std::string> strings;
	std::vector<const char*> ptrs;
};

// Starts Meta Horizon's background runtime (OVRServer_x64.exe) directly if
// it isn't already running. This is what games' bundled Oculus Platform SDK
// talks to for its entitlement check, and unlike LibOVRRT itself, Revive has
// no substitute for it - the process just needs to be alive when the game
// asks.
//
// This deliberately launches OVRServer_x64.exe itself rather than either of
// the two things that normally start it:
//  - The registered "OVRService" Windows service, which wraps the same exe
//    but starts it as LocalSystem (`sc qc OVRService`), with no access to
//    the current user's Oculus login session - an entitlement check against
//    a service-started instance fails exactly as if nothing were running.
//  - OVRServiceLauncher.exe, the thin watchdog Meta Horizon Link's own
//    Startup-folder shortcut points at, which requires elevation (its own
//    manifest - CreateProcessW on it fails with ERROR_ELEVATION_REQUIRED,
//    and unlike ShellExecute, CreateProcessW never auto-elevates). Launching
//    OVRServer_x64.exe directly needs no elevation and runs as whichever
//    user is running ReviveInjector.exe, i.e. the interactively logged-in
//    user - confirmed against this machine: the same exe run this same way
//    (double-clicking its own Startup-folder shortcut) is what fixed
//    entitlement checks here, with no UAC prompt.
//
// If the executable can't be found, this silently does nothing and the game
// proceeds exactly as if this function didn't exist.
static void EnsureOVRServiceRunning()
{
	if (IsProcessRunning(L"OVRServer_x64.exe"))
	{
		LOG("OVR service already running\n");
		return;
	}

	wchar_t base[MAX_PATH] = { 0 };
	std::wstring baseOverride = RegReadSZ(HKEY_CURRENT_USER, L"Software\\Revive", L"MetaHorizonPathOverride");
	if (!baseOverride.empty() && PathFileExistsW(baseOverride.c_str()))
	{
		wcsncpy(base, baseOverride.c_str(), MAX_PATH - 1);
		size_t len = wcslen(base);
		if (len > 0 && base[len - 1] != L'\\')
			wcsncat(base, L"\\", MAX_PATH);
	}
	else if (!GetOculusBasePath(base, MAX_PATH))
	{
		LOG("EnsureOVRServiceRunning: could not resolve Oculus base path\n");
		return;
	}

	std::wstring launcherDir = std::wstring(base) + L"Support\\oculus-runtime";
	std::wstring launcherPath = launcherDir + L"\\OVRServer_x64.exe";
	if (!PathFileExistsW(launcherPath.c_str()))
	{
		LOG("EnsureOVRServiceRunning: %ls does not exist\n", launcherPath.c_str());
		return;
	}

	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	// CREATE_NO_WINDOW: OVRServer_x64.exe is a console-subsystem binary;
	// without this it pops up a console window every time it's cold-started
	// (visible whenever something without a console of its own - e.g.
	// SteamVR's LaunchApplication flow - starts the injector).
	if (CreateProcessW(launcherPath.c_str(), nullptr, nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, launcherDir.c_str(), &si, &pi))
	{
		LOG("EnsureOVRServiceRunning: launched %ls (pid %lu)\n", launcherPath.c_str(), pi.dwProcessId);
		CloseHandle(pi.hThread);

		// Give it a few seconds to establish its IPC broker before the game
		// (which will ask it to verify entitlement almost immediately) starts.
		// There's no documented signal for "IPC broker ready" to poll for -
		// OVRServer_x64.exe is closed-source - so this still waits the full
		// window on the expected/successful path. What we CAN detect is the
		// process dying before that window elapses (e.g. a broken Oculus
		// install), so bail out early in that case instead of always paying
		// the full wait for a launch that's already failed.
		if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0)
		{
			DWORD exitCode = 0;
			GetExitCodeProcess(pi.hProcess, &exitCode);
			LOG("EnsureOVRServiceRunning: process exited early (code %lu)\n", exitCode);
		}
		CloseHandle(pi.hProcess);
	}
	else
	{
		LOG("EnsureOVRServiceRunning: CreateProcessW failed, error %lu\n", GetLastError());
	}
}

int wmain(int argc, wchar_t *argv[]) {
	if (argc < 2) {
		printf("usage: ReviveInjector.exe <executable path>\n");
		return -1;
	}

	WCHAR LogPath[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, LogPath)))
	{
		wcsncat(LogPath, L"\\Revive", MAX_PATH);
		
		BOOL exists = PathFileExists(LogPath);
		if (!exists)
			exists = CreateDirectory(LogPath, NULL);

		wcsncat(LogPath, L"\\ReviveInjector.txt", MAX_PATH);
		if (exists)
			g_LogFile = _wfopen(LogPath, L"w");
	}

	LOG("Launched injector with: %ls\n", GetCommandLine());

	// Environment snapshot, one block per install: which Revive build is
	// logging, which Meta Horizon driver / SteamVR / OpenXR runtime / GPU
	// driver it ran against. Purely diagnostic - support tickets quote this.
	LOG("Env: Revive version = " REV_VERSION_STRING "\n");

	std::wstring oculusVer = RegReadSZ(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Oculus VR, LLC\\Oculus", L"DriverVersion", KEY_READ | KEY_WOW64_32KEY);
	if (oculusVer.empty())
		LOG("Env: Meta Horizon driver = unknown\n");
	else
		LOG("Env: Meta Horizon driver = %ls\n", oculusVer.c_str());

	if (vr::VR_IsRuntimeInstalled())
	{
		char steamvrPath[MAX_PATH] = {};
		uint32_t pathLen = 0;
		vr::VR_GetRuntimePath(steamvrPath, MAX_PATH, &pathLen);
		LOG("Env: SteamVR = installed, runtime path = %s\n", steamvrPath);
	}
	else
	{
		LOG("Env: SteamVR = not installed\n");
	}

	std::wstring xrRuntime = RegReadSZ(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
	if (xrRuntime.empty())
		LOG("Env: OpenXR runtime = not installed\n");
	else
		LOG("Env: OpenXR runtime = %ls\n", xrRuntime.c_str());

	{
		std::string gpuDriver = "unknown";
		IDXGIFactory* factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
		{
			IDXGIAdapter* adapter = nullptr;
			if (SUCCEEDED(factory->EnumAdapters(0, &adapter)))
			{
				LARGE_INTEGER ver;
				if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &ver)))
				{
					char buf[64];
					sprintf_s(buf, "%u.%u.%u.%u",
						(UINT)((ver.QuadPart >> 48) & 0xFFFF),
						(UINT)((ver.QuadPart >> 32) & 0xFFFF),
						(UINT)((ver.QuadPart >> 16) & 0xFFFF),
						(UINT)(ver.QuadPart & 0xFFFF));
					gpuDriver = buf;
				}
				adapter->Release();
			}
			factory->Release();
		}
		LOG("Env: GPU driver = %s\n", gpuDriver.c_str());
	}


	char moduleDir[MAX_PATH];
	GetModuleFileNameA(NULL, moduleDir, MAX_PATH);
	PathRemoveFileSpecA(moduleDir);

	bool debug = false;
	StringArray dlls;
	std::string appKey;
	wchar_t path[MAX_PATH] = { 0 };
	for (int i = 1; i < argc; i++)
	{
		if (wcscmp(argv[i], L"/openxr") == 0)
		{
			dlls.add(moduleDir + std::string("\\LibReviveXR64.dll"));
		}
		else if (wcscmp(argv[i], L"/openvr") == 0)
		{
			dlls.add(moduleDir + std::string("\\openvr_api64.dll"));
			dlls.add(moduleDir + std::string("\\LibRevive64.dll"));
		}
		else if (wcscmp(argv[i], L"/proxy") == 0)
		{
			dlls.add(moduleDir + std::string("\\LibOVRProxy64.dll"));
		}
		else if (wcscmp(argv[i], L"/app") == 0)
		{
			appKey = "revive.app." + std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t>().to_bytes(argv[++i]);
		}
		else if (wcscmp(argv[i], L"/base") == 0)
		{
			// HKCU\Software\Revive\MetaHorizonPathOverride, when set and pointing
			// at a real directory, takes precedence over the registry lookup -
			// same override ReviveOverlay.exe reads for the same purpose (e.g.
			// Meta Horizon Link was uninstalled but the games were kept).
			std::wstring baseOverride = RegReadSZ(HKEY_CURRENT_USER, L"Software\\Revive", L"MetaHorizonPathOverride");
			if (!baseOverride.empty() && PathFileExistsW(baseOverride.c_str()))
			{
				wcsncpy(path, baseOverride.c_str(), MAX_PATH - 1);
				// The settings UI doesn't require a trailing backslash, but the
				// registry-provided base path always has one and everything
				// downstream concatenates onto path assuming it's there.
				size_t len = wcslen(path);
				if (len > 0 && path[len - 1] != L'\\')
					wcsncat(path, L"\\", MAX_PATH);
			}
			else if (!GetOculusBasePath(path, MAX_PATH))
			{
				return -1;
			}
		}
		else if (wcscmp(argv[i], L"/library") == 0)
		{
			// Same override mechanism as /base above, but for
			// GamesLibraryPathOverride. The library GUID argument is still
			// consumed either way; it's just ignored when the override applies.
			++i;
			std::wstring libraryOverride = RegReadSZ(HKEY_CURRENT_USER, L"Software\\Revive", L"GamesLibraryPathOverride");
			if (!libraryOverride.empty() && PathFileExistsW(libraryOverride.c_str()))
			{
				wcsncpy(path, libraryOverride.c_str(), MAX_PATH - 1);
			}
			else if (!GetLibraryPath(path, MAX_PATH, argv[i]))
			{
				if (!GetDefaultLibraryPath(path, MAX_PATH))
				{
					return -1;
				}
			}
			wcsncat(path, L"\\", MAX_PATH);
		}
		else if (wcscmp(argv[i], L"/debug") == 0)
		{
			debug = true;
		}
		else
		{
			// Concatenate all other arguments
			wcsncat(path, argv[i], MAX_PATH);
			wcsncat(path, L" ", MAX_PATH);
		}
	}

	if (*path && IsBlockedProcess(path))
	{
		const wchar_t* name = wcsrchr(path, L'\\');
		LOG("Injection skipped: %ls is in the non-game blocklist\n", name ? name + 1 : path);
		return 0;
	}

	EnsureOVRServiceRunning();

	if (dlls.empty())
	{
		// Manual override: set HKCU\Software\Revive\RuntimePreference (REG_SZ)
		// to "OpenVR" or "OpenXR" to force a runtime. Any other value, or the
		// key/value being absent (the default for everyone), falls back to
		// the original auto-detection below - this override never changes
		// behavior unless someone explicitly sets it.
		std::wstring runtimePref = RegReadSZ(HKEY_CURRENT_USER, L"Software\\Revive", L"RuntimePreference");

		bool useOpenVR;
		if (runtimePref == L"OpenVR")
			useOpenVR = true;
		else if (runtimePref == L"OpenXR")
			useOpenVR = false;
		else
			useOpenVR = vr::VR_IsRuntimeInstalled();

		if (useOpenVR)
		{
			dlls.add(moduleDir + std::string("\\openvr_api64.dll"));
			dlls.add(moduleDir + std::string("\\LibRevive64.dll"));
		}
		else
		{
			dlls.add(moduleDir + std::string("\\LibReviveXR64.dll"));
		}
	}

	LOG("Command for injector is: %ls\n", path);

	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	wchar_t workingDir[MAX_PATH];
	wcsncpy(workingDir, path, MAX_PATH);

	// Remove extension
	wchar_t* ext = wcsstr(workingDir, L".exe");
	if (ext)
		*ext = L'\0';

	// Remove filename
	wchar_t* file = wcsrchr(workingDir, L'\\');
	if (file)
		*file = L'\0';

	if (!DetourCreateProcessWithDlls(NULL, path, NULL, NULL, FALSE,
		debug ? CREATE_SUSPENDED | DEBUG_ONLY_THIS_PROCESS : 0,
		NULL, (file && ext) ? workingDir : NULL, &si, &pi,
		(DWORD)dlls.size(), dlls.c_str(), NULL))
	{
		LOG("Failed to create process\n");
		return -1;
	}

	if (debug)
	{
		if (!DebugActiveProcessStop(pi.dwProcessId))
		{
			LOG("Failed to stop debugging\n");
			return -1;
		}

		if (ResumeThread(pi.hThread) == -1)
		{
			LOG("Failed to resume process\n");
			return -1;
		}
	}

	LOG("Succesfully injected!\n");

	if (!appKey.empty())
	{
		vr::EVRInitError err;
		vr::VR_Init(&err, vr::VRApplication_Utility);
		if (err == vr::VRInitError_None)
		{
			if (vr::VRApplications()->IdentifyApplication(pi.dwProcessId, appKey.c_str()) == vr::VRApplicationError_None)
				LOG("Identified application as: %s\n", appKey.c_str());
			vr::VR_Shutdown();
		}
	}
	return 0;
}
