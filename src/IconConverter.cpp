#include "IconConverter.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <filesystem>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

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
}

bool IconConverter::ConvertToPng(const std::string& sourcePath, const std::string& destPngPath)
{
	std::error_code ec;
	if (fs::exists(destPngPath, ec))
	{
		auto srcTime = fs::last_write_time(sourcePath, ec);
		auto dstTime = fs::last_write_time(destPngPath, ec);
		if (!ec && dstTime >= srcTime)
			return true; // already converted, source hasn't changed since
	}

	fs::create_directories(fs::path(destPngPath).parent_path(), ec);

	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		return false;

	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromFilename(Utf8ToWide(sourcePath).c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnDemand, &decoder)))
		return false;

	ComPtr<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, &frame)))
		return false;

	ComPtr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateFormatConverter(&converter)))
		return false;
	if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
		nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		return false;

	ComPtr<IWICStream> stream;
	if (FAILED(factory->CreateStream(&stream)))
		return false;
	if (FAILED(stream->InitializeFromFilename(Utf8ToWide(destPngPath).c_str(), GENERIC_WRITE)))
		return false;

	ComPtr<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
		return false;
	if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
		return false;

	ComPtr<IWICBitmapFrameEncode> encodeFrame;
	if (FAILED(encoder->CreateNewFrame(&encodeFrame, nullptr)))
		return false;
	if (FAILED(encodeFrame->Initialize(nullptr)))
		return false;

	UINT width = 0, height = 0;
	converter->GetSize(&width, &height);
	encodeFrame->SetSize(width, height);
	WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
	encodeFrame->SetPixelFormat(&format);

	if (FAILED(encodeFrame->WriteSource(converter.Get(), nullptr)))
		return false;
	if (FAILED(encodeFrame->Commit()))
		return false;
	if (FAILED(encoder->Commit()))
		return false;

	return true;
}
