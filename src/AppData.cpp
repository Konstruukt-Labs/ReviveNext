#include "AppData.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>

namespace
{
	namespace fs = std::filesystem;
}

namespace AppData
{
	std::optional<std::string> Dir()
	{
		wchar_t* base = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
			return std::nullopt;
		// make_preferred: fs::path::string() can otherwise end up with
		// mixed separators (a forward slash from the append), which
		// SteamVR refuses to persist as a registered manifest path.
		fs::path dir = (fs::path(base) / "ReviveNext").make_preferred();
		CoTaskMemFree(base);

		std::error_code ec;
		fs::create_directories(dir, ec);
		if (ec)
			return std::nullopt;
		return dir.string();
	}

	std::optional<std::string> SubDir(const char* name)
	{
		auto dir = Dir();
		if (!dir)
			return std::nullopt;

		fs::path sub = (fs::path(*dir) / name).make_preferred();
		std::error_code ec;
		fs::create_directories(sub, ec);
		if (ec)
			return std::nullopt;
		return sub.string();
	}
}
