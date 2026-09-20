#pragma once

#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace image_output {
using Microsoft::WRL::ComPtr;
using winrt::check_hresult;

enum class OutputFormat : DWORD { Png = 0, Jpeg = 1 };

struct CaptureSettings {
    OutputFormat format = OutputFormat::Png;
    int jpegQuality = 92;
    bool jpegFullChroma = true;
    UINT maxDimension = 0; // Longest edge; zero preserves the original resolution.
    bool openEditor = true;
    UINT delaySeconds = 0;
};

inline constexpr UINT kSizeLimits[] = {0, 1280, 1920, 2560, 3840};

inline CaptureSettings QualityPreset(int index) {
    if (index == 0) return {OutputFormat::Jpeg, 80, false, 1920};
    if (index == 1) return {OutputFormat::Jpeg, 92, true, 0};
    return {};
}

inline int MatchingPreset(const CaptureSettings& settings) {
    for (int index = 0; index < 3; ++index) {
        const auto preset = QualityPreset(index);
        if (settings.format == preset.format && settings.maxDimension == preset.maxDimension &&
            (settings.format == OutputFormat::Png ||
             (settings.jpegQuality == preset.jpegQuality && settings.jpegFullChroma == preset.jpegFullChroma)))
            return index;
    }
    return 3;
}

inline float HalfToFloat(uint16_t half) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(half & 0x8000u)) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    uint32_t mantissa = half & 0x3ffu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            int shift = 0;
            while ((mantissa & 0x400u) == 0) { mantissa <<= 1; ++shift; }
            // Subnormals have exponent 1-bias, not 0-bias.
            bits = sign | (static_cast<uint32_t>(127 - 14 - shift) << 23) |
                   ((mantissa & 0x3ffu) << 13);
        }
    } else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
    else bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    return std::bit_cast<float>(bits);
}

inline uint8_t LinearToSrgbByte(float value) noexcept {
    // NaN and negative values become black; positive infinity saturates to white.
    if (!(value > 0.0f)) return 0;
    value = std::min(value, 1.0f);
    const float encoded = value <= 0.0031308f ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(encoded * 255.0f), 0l, 255l));
}

inline UINT CheckedImageBytes(UINT width, UINT height, UINT stride, UINT bytesPerPixel) {
    if (!width || !height || width > static_cast<UINT>(LONG_MAX) ||
        height > static_cast<UINT>(LONG_MAX) ||
        static_cast<uint64_t>(width) * bytesPerPixel > stride ||
        static_cast<uint64_t>(stride) * height > UINT_MAX)
        throw winrt::hresult_invalid_argument(L"Dimensiones de imagen no válidas");
    return stride * height;
}

inline ComPtr<IWICImagingFactory> ImagingFactory() {
    ComPtr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory)));
    return factory;
}

inline void EncodeWic(IStream* stream, REFGUID container, REFGUID pixelFormat,
                      UINT width, UINT height, UINT stride, const BYTE* pixels, UINT byteCount,
                      float quality = -1.0f, bool fullChroma = true) {
    const bool jpeg = IsEqualGUID(container, GUID_ContainerFormatJpeg);
    if (!pixels || byteCount < CheckedImageBytes(width, height, stride, jpeg ? 3 : 4))
        throw winrt::hresult_invalid_argument();
    auto factory = ImagingFactory();
    ComPtr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(container, nullptr, &encoder));
    check_hresult(encoder->Initialize(stream, WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check_hresult(encoder->CreateNewFrame(&frame, &options));
    if (jpeg) {
        PROPBAG2 properties[2]{};
        properties[0].pstrName = const_cast<wchar_t*>(L"ImageQuality");
        properties[1].pstrName = const_cast<wchar_t*>(L"JpegYCrCbSubsampling");
        VARIANT values[2]{};
        values[0].vt = VT_R4;
        values[0].fltVal = std::clamp(quality, 0.0f, 1.0f);
        values[1].vt = VT_UI1;
        values[1].bVal = static_cast<BYTE>(fullChroma ? WICJpegYCrCbSubsampling444 : WICJpegYCrCbSubsampling420);
        check_hresult(options->Write(2, properties, values));
    }
    check_hresult(frame->Initialize(options.Get()));
    check_hresult(frame->SetSize(width, height));
    check_hresult(frame->SetResolution(96.0, 96.0));
    WICPixelFormatGUID actualFormat = pixelFormat;
    check_hresult(frame->SetPixelFormat(&actualFormat));
    if (!IsEqualGUID(actualFormat, pixelFormat))
        throw winrt::hresult_error(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);

    ComPtr<IWICMetadataQueryWriter> metadata;
    check_hresult(frame->GetMetadataQueryWriter(&metadata));
    PROPVARIANT colorSpace{};
    if (jpeg) {
        colorSpace.vt = VT_UI2;
        colorSpace.uiVal = 1; // EXIF sRGB.
        check_hresult(metadata->SetMetadataByName(L"/app1/ifd/exif/{ushort=40961}", &colorSpace));
    } else {
        colorSpace.vt = VT_UI1;
        colorSpace.bVal = 0; // PNG sRGB, perceptual rendering intent.
        check_hresult(metadata->SetMetadataByName(L"/sRGB/RenderingIntent", &colorSpace));
    }
    check_hresult(frame->WritePixels(height, stride, byteCount, const_cast<BYTE*>(pixels)));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());
}

inline void ResizeSdr(UINT& width, UINT& height, UINT& stride, std::vector<BYTE>& pixels, UINT limit) {
    const UINT byteCount = CheckedImageBytes(width, height, stride, 4);
    if (pixels.size() < byteCount) throw winrt::hresult_invalid_argument();
    const UINT longest = std::max(width, height);
    if (!limit || longest <= limit) return; // Never upscale a small crop.
    const UINT newWidth = std::max(1u, static_cast<UINT>((static_cast<uint64_t>(width) * limit + longest / 2) / longest));
    const UINT newHeight = std::max(1u, static_cast<UINT>((static_cast<uint64_t>(height) * limit + longest / 2) / longest));
    const UINT newStride = newWidth * 4;
    std::vector<BYTE> resized(CheckedImageBytes(newWidth, newHeight, newStride, 4));
    auto factory = ImagingFactory();
    ComPtr<IWICBitmap> bitmap;
    check_hresult(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                                 stride, byteCount, pixels.data(), &bitmap));
    ComPtr<IWICBitmapScaler> scaler;
    check_hresult(factory->CreateBitmapScaler(&scaler));
    check_hresult(scaler->Initialize(bitmap.Get(), newWidth, newHeight, WICBitmapInterpolationModeFant));
    check_hresult(scaler->CopyPixels(nullptr, newStride, static_cast<UINT>(resized.size()), resized.data()));
    pixels = std::move(resized);
    width = newWidth;
    height = newHeight;
    stride = newStride;
}

inline void SaveSdrFile(const std::filesystem::path& path, const CaptureSettings& settings,
                       UINT width, UINT height, UINT stride, const std::vector<BYTE>& pixels) {
    if (pixels.size() < CheckedImageBytes(width, height, stride, 4)) throw winrt::hresult_invalid_argument();
    auto factory = ImagingFactory();
    ComPtr<IWICStream> stream;
    check_hresult(factory->CreateStream(&stream));
    check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    if (settings.format == OutputFormat::Png) {
        EncodeWic(stream.Get(), GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA,
                  width, height, stride, pixels.data(), static_cast<UINT>(pixels.size()));
    } else {
        const UINT jpegStride = width * 3;
        std::vector<BYTE> bgr(CheckedImageBytes(width, height, jpegStride, 3));
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
                std::memcpy(bgr.data() + static_cast<size_t>(y) * jpegStride + x * 3,
                            pixels.data() + static_cast<size_t>(y) * stride + x * 4, 3);
        EncodeWic(stream.Get(), GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR,
                  width, height, jpegStride, bgr.data(), static_cast<UINT>(bgr.size()),
                  settings.jpegQuality / 100.0f, settings.jpegFullChroma);
    }
}

struct GlobalMemoryDeleter { void operator()(void* memory) const noexcept { if (memory) GlobalFree(memory); } };
using GlobalMemory = std::unique_ptr<void, GlobalMemoryDeleter>;

inline GlobalMemory CopyGlobalMemory(const void* data, size_t size) {
    GlobalMemory memory(GlobalAlloc(GMEM_MOVEABLE, size));
    if (!memory) throw std::bad_alloc();
    void* destination = GlobalLock(memory.get());
    if (!destination) throw std::bad_alloc();
    std::memcpy(destination, data, size);
    GlobalUnlock(memory.get());
    return memory;
}

inline std::vector<BYTE> BuildDib(UINT width, UINT height, UINT stride, const BYTE* pixels, bool v5) {
    CheckedImageBytes(width, height, stride, 4);
    if (!pixels) throw winrt::hresult_invalid_argument();
    const UINT dibStride = v5 ? width * 4 : ((width * 3 + 3) & ~3u);
    const UINT pixelBytes = CheckedImageBytes(width, height, dibStride, v5 ? 4 : 3);
    const size_t headerSize = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    std::vector<BYTE> bytes(headerSize + pixelBytes, 0);
    if (v5) {
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = static_cast<LONG>(width);
        header.bV5Height = -static_cast<LONG>(height);
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = pixelBytes;
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
        header.bV5CSType = LCS_sRGB;
        header.bV5Intent = LCS_GM_IMAGES;
        std::memcpy(bytes.data(), &header, sizeof(header));
    } else {
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(width);
        header.biHeight = static_cast<LONG>(height); // Traditional bottom-up, no alpha.
        header.biPlanes = 1;
        header.biBitCount = 24;
        header.biCompression = BI_RGB;
        header.biSizeImage = pixelBytes;
        std::memcpy(bytes.data(), &header, sizeof(header));
    }
    for (UINT y = 0; y < height; ++y) {
        BYTE* destination = bytes.data() + headerSize + static_cast<size_t>(v5 ? y : height - 1 - y) * dibStride;
        const BYTE* source = pixels + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < width; ++x) {
            std::memcpy(destination + x * (v5 ? 4 : 3), source + x * 4, 3);
            if (v5) destination[x * 4 + 3] = 255;
        }
    }
    return bytes;
}

inline GlobalMemory EncodeClipboardPng(UINT width, UINT height, UINT stride, const BYTE* pixels) {
    ComPtr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    EncodeWic(stream.Get(), GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA,
              width, height, stride, pixels, CheckedImageBytes(width, height, stride, 4));
    STATSTG stat{};
    check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
    if (!stat.cbSize.QuadPart || stat.cbSize.QuadPart > UINT_MAX) throw winrt::hresult_invalid_argument();
    HGLOBAL encoded = nullptr;
    check_hresult(GetHGlobalFromStream(stream.Get(), &encoded));
    const void* data = GlobalLock(encoded);
    if (!data) throw std::bad_alloc();
    struct Unlock { HGLOBAL memory; ~Unlock() { GlobalUnlock(memory); } } unlock{encoded};
    return CopyGlobalMemory(data, static_cast<size_t>(stat.cbSize.QuadPart));
}

using PngEncoder = GlobalMemory (*)(UINT, UINT, UINT, const BYTE*);

inline bool CopySdrToClipboard(HWND owner, UINT width, UINT height, UINT stride,
                               const BYTE* pixels, std::wstring& error, PngEncoder encodePng = EncodeClipboardPng) {
    error.clear();
    try {
        if (!owner || !IsWindow(owner)) throw winrt::hresult_invalid_argument(L"Ventana de portapapeles no válida");
        // Prepare everything before opening/emptying the user's clipboard.
        auto dibBytes = BuildDib(width, height, stride, pixels, false);
        auto dib = CopyGlobalMemory(dibBytes.data(), dibBytes.size());
        auto v5Bytes = BuildDib(width, height, stride, pixels, true);
        auto v5 = CopyGlobalMemory(v5Bytes.data(), v5Bytes.size());
        GlobalMemory png;
        const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        try { if (pngFormat) png = encodePng(width, height, stride, pixels); }
        catch (...) { error = L"PNG no disponible; se copiaron los formatos de Windows"; }

        bool opened = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (OpenClipboard(owner)) { opened = true; break; }
            if (attempt < 39) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!opened) { error = L"Otra aplicación mantiene ocupado el portapapeles"; return false; }
        struct Close { ~Close() { CloseClipboard(); } } close;
        if (!EmptyClipboard()) { error = L"No se pudo vaciar el portapapeles"; return false; }
        auto publish = [](UINT format, GlobalMemory& memory) {
            if (!memory || !SetClipboardData(format, memory.get())) return false;
            memory.release(); // Windows owns it only after a successful SetClipboardData.
            return true;
        };
        // PNG first: web/Electron clients and applications that prefer encoded images.
        const bool pngOk = pngFormat && publish(pngFormat, png);
        const bool v5Ok = publish(CF_DIBV5, v5);
        const bool dibOk = publish(CF_DIB, dib);
        if (!pngOk || !v5Ok || !dibOk)
            error = L"No se pudieron publicar todos los formatos de imagen";
        if (!pngOk && !v5Ok && !dibOk) { error = L"No se pudo copiar la imagen"; return false; }
        return true;
    } catch (const winrt::hresult_error& failure) {
        error = failure.message().c_str();
    } catch (...) {
        error = L"No se pudo preparar la imagen para el portapapeles";
    }
    return false;
}
} // namespace image_output
