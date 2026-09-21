#include "SteamGridDb.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
	std::wstring Utf8ToWide(const std::string& s)
	{
		if (s.empty())
			return {};
		int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
		std::wstring result(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), result.data(), len);
		return result;
	}

	// Percent-encodes a search term for use as a URL path segment.
	std::string UrlEncode(const std::string& s)
	{
		static const char* hex = "0123456789ABCDEF";
		std::string out;
		for (unsigned char c : s)
		{
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				out += (char)c;
			else
			{
				out += '%';
				out += hex[c >> 4];
				out += hex[c & 0xF];
			}
		}
		return out;
	}

	// Issues a GET request to https://host/pathAndQuery with an optional
	// bearer token, and returns the raw response body. std::nullopt on any
	// failure (network error, non-2xx status, etc.) - callers treat that
	// as "no result" rather than propagating an error, since this is all
	// optional polish on top of a sync that already succeeded.
	std::optional<std::string> HttpsGet(const std::wstring& host, const std::wstring& pathAndQuery, const std::string& bearerToken)
	{
		HINTERNET session = WinHttpOpen(L"ReviveNext/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
			return std::nullopt;

		HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (!connect)
		{
			WinHttpCloseHandle(session);
			return std::nullopt;
		}

		HINTERNET request = WinHttpOpenRequest(connect, L"GET", pathAndQuery.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (!request)
		{
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return std::nullopt;
		}

		std::optional<std::string> result;
		std::wstring headers;
		if (!bearerToken.empty())
			headers = L"Authorization: Bearer " + Utf8ToWide(bearerToken) + L"\r\n";

		if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
				headers.empty() ? 0 : (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			&& WinHttpReceiveResponse(request, nullptr))
		{
			DWORD statusCode = 0, statusSize = sizeof(statusCode);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
				WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

			if (statusCode >= 200 && statusCode < 300)
			{
				std::string body;
				DWORD available = 0;
				while (WinHttpQueryDataAvailable(request, &available) && available > 0)
				{
					std::string chunk(available, '\0');
					DWORD read = 0;
					if (!WinHttpReadData(request, chunk.data(), available, &read))
						break;
					body.append(chunk.data(), read);
				}
				result = std::move(body);
			}
		}

		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return result;
	}
}

std::vector<SgdbGame> SteamGridDb::SearchGames(const std::string& term)
{
	std::vector<SgdbGame> games;
	auto body = HttpsGet(L"www.steamgriddb.com", L"/api/v2/search/autocomplete/" + Utf8ToWide(UrlEncode(term)), m_apiKey);
	if (!body)
		return games;

	try
	{
		json response = json::parse(*body);
		if (!response.value("success", false) || !response.contains("data"))
			return games;

		for (auto& item : response["data"])
			games.push_back({ item.value("id", (int64_t)0), item.value("name", "") });
	}
	catch (const json::exception&)
	{
	}
	return games;
}

std::vector<SgdbGrid> SteamGridDb::GetPortraitGrids(int64_t gameId)
{
	std::vector<SgdbGrid> grids;
	std::wstring path = L"/api/v2/grids/game/" + std::to_wstring(gameId) + L"?dimensions=600x900";
	auto body = HttpsGet(L"www.steamgriddb.com", path, m_apiKey);
	if (!body)
		return grids;

	try
	{
		json response = json::parse(*body);
		if (!response.value("success", false) || !response.contains("data"))
			return grids;

		for (auto& item : response["data"])
		{
			SgdbGrid grid;
			grid.id = item.value("id", (int64_t)0);
			grid.url = item.value("url", "");
			grid.width = item.value("width", 0);
			grid.height = item.value("height", 0);
			if (item.contains("author") && item["author"].is_object())
				grid.author = item["author"].value("name", "");
			if (!grid.url.empty())
				grids.push_back(std::move(grid));
		}
	}
	catch (const json::exception&)
	{
	}
	return grids;
}

std::optional<std::string> SteamGridDb::DownloadBytes(const std::string& url)
{
	URL_COMPONENTS components = { sizeof(components) };
	wchar_t host[256] = {}, path[2048] = {};
	components.lpszHostName = host;
	components.dwHostNameLength = _countof(host);
	components.lpszUrlPath = path;
	components.dwUrlPathLength = _countof(path);

	std::wstring wideUrl = Utf8ToWide(url);
	if (!WinHttpCrackUrl(wideUrl.c_str(), (DWORD)wideUrl.size(), 0, &components))
		return std::nullopt;

	auto body = HttpsGet(host, path, "");
	if (!body || body->empty())
		return std::nullopt;
	return body;
}

bool SteamGridDb::DownloadFile(const std::string& url, const std::string& destPath)
{
	auto body = DownloadBytes(url);
	if (!body)
		return false;

	std::ofstream file(destPath, std::ios::binary | std::ios::trunc);
	if (!file)
		return false;
	file.write(body->data(), (std::streamsize)body->size());
	return (bool)file;
}
