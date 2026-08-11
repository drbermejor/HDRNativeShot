#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <wrl/client.h>

#include "resource.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace winrt;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;
namespace WGD3D = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

constexpr wchar_t kAppName[] = L"NativeHDRShot";
constexpr wchar_t kWindowClass[] = L"NativeHDRShot.HiddenWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCaptureDone = WM_APP + 2;
constexpr UINT kMenuCapture = 100;
constexpr UINT kMenuOpenFolder = 101;
constexpr UINT kMenuSettings = 102;
constexpr UINT kMenuExit = 103;
constexpr UINT kSettingsFormat = 200;
constexpr UINT kSettingsQuality = 201;
constexpr UINT kSettingsQualityValue = 202;
constexpr UINT kSettingsSave = 203;
constexpr UINT kSettingsCancel = 204;
constexpr int kHotkeyPrintScreen = 1;
constexpr int kHotkeyFallback = 2;

enum class OutputFormat : DWORD {
    Png = 0,
    Jpeg = 1,
};

struct CaptureSettings {
    OutputFormat format = OutputFormat::Png;
    int jpegQuality = 92;
};

struct CaptureResult {
    bool ok = false;
    std::filesystem::path sdrPath;
    std::wstring error;
    float peakLinear = 0.0f;
    float sdrWhiteMultiplier = 1.0f;
};

CaptureSettings LoadSettings() noexcept {
    CaptureSettings settings;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NativeHDRShot", L"Format",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value <= 1) {
        settings.format = static_cast<OutputFormat>(value);
    }
    value = 0;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NativeHDRShot", L"JpegQuality",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        settings.jpegQuality = std::clamp(static_cast<int>(value), 50, 100);
    }
    return settings;
}

void SaveSettings(const CaptureSettings& settings) {
    HKEY key = nullptr;
    check_hresult(HRESULT_FROM_WIN32(RegCreateKeyExW(
        HKEY_CURRENT_USER, L"Software\\NativeHDRShot", 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr)));
    const DWORD format = static_cast<DWORD>(settings.format);
    const DWORD quality = static_cast<DWORD>(settings.jpegQuality);
    const LSTATUS formatStatus = RegSetValueExW(
        key, L"Format", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&format), sizeof(format));
    const LSTATUS qualityStatus = RegSetValueExW(
        key, L"JpegQuality", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&quality), sizeof(quality));
    RegCloseKey(key);
    check_hresult(HRESULT_FROM_WIN32(formatStatus));
    check_hresult(HRESULT_FROM_WIN32(qualityStatus));
}

std::wstring HResultText(HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = count && buffer ? std::wstring(buffer, count) : L"Error desconocido";
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    std::wostringstream formatted;
    formatted << result << L" (0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << L")";
    return formatted.str();
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    check_hresult(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw));
    std::filesystem::path value(raw);
    CoTaskMemFree(raw);
    return value;
}

std::filesystem::path OutputRoot() {
    return KnownFolder(FOLDERID_Pictures) / kAppName;
}

std::filesystem::path LogPath() {
    const auto dir = KnownFolder(FOLDERID_LocalAppData) / kAppName;
    std::filesystem::create_directories(dir);
    return dir / L"NativeHDRShot.log";
}

void Log(const std::wstring& message) {
    try {
        const auto path = LogPath();
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 1024) {
            const auto oldPath = path.parent_path() / L"NativeHDRShot.old.log";
            std::error_code ignored;
            std::filesystem::remove(oldPath, ignored);
            std::filesystem::rename(path, oldPath, ignored);
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::wofstream stream(path, std::ios::app);
        stream << std::setfill(L'0') << L'[' << std::setw(4) << now.wYear << L'-'
               << std::setw(2) << now.wMonth << L'-' << std::setw(2) << now.wDay << L' '
               << std::setw(2) << now.wHour << L':' << std::setw(2) << now.wMinute << L':'
               << std::setw(2) << now.wSecond << L'.' << std::setw(3) << now.wMilliseconds
               << L"] " << message << L'\n';
    } catch (...) {
        // Logging must never prevent a capture.
    }
}

std::wstring BaseFilename() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream name;
    name << kAppName << L' ' << std::setfill(L'0') << std::setw(4) << now.wYear << L'-'
         << std::setw(2) << now.wMonth << L'-' << std::setw(2) << now.wDay << L' '
         << std::setw(2) << now.wHour << L'-' << std::setw(2) << now.wMinute << L'-'
         << std::setw(2) << now.wSecond << L'-' << std::setw(3) << now.wMilliseconds;
    return name.str();
}

std::filesystem::path MonthlyOutputDirectory() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream month;
    month << std::setfill(L'0') << std::setw(4) << now.wYear << L'-' << std::setw(2) << now.wMonth;
    const auto path = OutputRoot() / month.str();
    std::filesystem::create_directories(path);
    return path;
}

float HalfToFloat(uint16_t half) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(half & 0x8000u)) << 16;
    uint32_t exponent = (half >> 10) & 0x1fu;
    uint32_t mantissa = half & 0x3ffu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x3ffu;
            const uint32_t floatExponent = static_cast<uint32_t>(127 - 15 - shift);
            bits = sign | (floatExponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

uint8_t LinearToSrgbByte(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    const float encoded = value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(encoded * 255.0f), 0l, 255l));
}

void SaveWic(
    const std::filesystem::path& path,
    REFGUID container,
    REFGUID requestedPixelFormat,
    UINT width,
    UINT height,
    UINT stride,
    const BYTE* pixels,
    UINT byteCount,
    float encoderQuality = -1.0f) {
    ComPtr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    check_hresult(factory->CreateStream(&stream));
    check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    check_hresult(factory->CreateEncoder(container, nullptr, &encoder));
    check_hresult(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check_hresult(encoder->CreateNewFrame(&frame, &options));
    if (encoderQuality >= 0.0f && options) {
        PROPBAG2 property{};
        property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT value{};
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = std::clamp(encoderQuality, 0.0f, 1.0f);
        check_hresult(options->Write(1, &property, &value));
        VariantClear(&value);
    }
    check_hresult(frame->Initialize(options.Get()));
    check_hresult(frame->SetSize(width, height));
    check_hresult(frame->SetResolution(96.0, 96.0));

    WICPixelFormatGUID actualFormat = requestedPixelFormat;
    check_hresult(frame->SetPixelFormat(&actualFormat));
    if (!IsEqualGUID(actualFormat, requestedPixelFormat)) {
        throw hresult_error(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT,
                            L"El códec WIC no admite el formato solicitado");
    }

    check_hresult(frame->WritePixels(height, stride, byteCount, const_cast<BYTE*>(pixels)));
    check_hresult(frame->Commit());
    check_hresult(encoder->Commit());
}

ComPtr<ID3D11Device> CreateDeviceForMonitor(HMONITOR monitor, ComPtr<ID3D11DeviceContext>& context) {
    ComPtr<IDXGIFactory1> factory;
    check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> selected;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                selected = adapter;
                break;
            }
        }
        if (selected) break;
    }

    ComPtr<ID3D11Device> device;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(
        selected.Get(), selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    check_hresult(hr);
    return device;
}

WGD3D::IDirect3DDevice ToWinRtDevice(const ComPtr<ID3D11Device>& device) {
    ComPtr<IDXGIDevice> dxgiDevice;
    check_hresult(device.As(&dxgiDevice));
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    return inspectable.as<WGD3D::IDirect3DDevice>();
}

WGC::GraphicsCaptureItem ItemForMonitor(HMONITOR monitor) {
    auto interop = get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForMonitor(monitor, guid_of<WGC::GraphicsCaptureItem>(), put_abi(item)));
    return item;
}

struct CapturedTexture {
    ComPtr<ID3D11Texture2D> staging;
    UINT width = 0;
    UINT height = 0;
};

CapturedTexture CaptureTexture(HMONITOR monitor) {
    ComPtr<ID3D11DeviceContext> context;
    auto d3dDevice = CreateDeviceForMonitor(monitor, context);
    const auto winrtDevice = ToWinRtDevice(d3dDevice);
    const auto item = ItemForMonitor(monitor);
    const auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) throw hresult_error(E_FAIL, L"El monitor no tiene un tamaño válido");

    auto pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice, WGD::DirectXPixelFormat::R16G16B16A16Float, 1, size);
    auto session = pool.CreateCaptureSession(item);
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}

    struct SharedState {
        std::atomic<bool> completed{false};
        HANDLE eventHandle = nullptr;
        HRESULT hr = E_FAIL;
        CapturedTexture result;
    } state;
    state.eventHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state.eventHandle) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    const auto closeEvent = std::unique_ptr<void, decltype(&CloseHandle)>(state.eventHandle, CloseHandle);

    const auto token = pool.FrameArrived([&](WGC::Direct3D11CaptureFramePool const& sender,
                                              winrt::Windows::Foundation::IInspectable const&) noexcept {
        if (state.completed.exchange(true)) return;
        try {
            auto frame = sender.TryGetNextFrame();
            const auto content = frame.ContentSize();
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> source;
            check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            desc.Width = static_cast<UINT>(content.Width);
            desc.Height = static_cast<UINT>(content.Height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            check_hresult(d3dDevice->CreateTexture2D(&desc, nullptr, &state.result.staging));
            D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
            context->CopySubresourceRegion(state.result.staging.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
            context->Flush();
            state.result.width = desc.Width;
            state.result.height = desc.Height;
            state.hr = S_OK;
        } catch (const hresult_error& error) {
            state.hr = error.code();
        } catch (...) {
            state.hr = E_FAIL;
        }
        SetEvent(state.eventHandle);
    });

    session.StartCapture();
    const DWORD wait = WaitForSingleObject(state.eventHandle, 5000);
    pool.FrameArrived(token);
    session.Close();
    pool.Close();
    if (wait != WAIT_OBJECT_0) throw hresult_error(HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()),
                                                    L"Windows no entregó ningún fotograma en 5 segundos");
    check_hresult(state.hr);
    return std::move(state.result);
}

float SdrWhiteLevelMultiplier(HMONITOR monitor) noexcept;

CaptureResult CaptureMonitor(HMONITOR monitor, const CaptureSettings& settings,
                             const RECT* requestedRegion = nullptr) {
    CaptureResult result;
    try {
        result.sdrWhiteMultiplier = SdrWhiteLevelMultiplier(monitor);
        auto captured = CaptureTexture(monitor);
        ComPtr<ID3D11Device> device;
        captured.staging->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context->Map(captured.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        struct Unmapper {
            ID3D11DeviceContext* context;
            ID3D11Resource* resource;
            ~Unmapper() { context->Unmap(resource, 0); }
        } unmapper{context.Get(), captured.staging.Get()};

        RECT crop{0, 0, static_cast<LONG>(captured.width), static_cast<LONG>(captured.height)};
        if (requestedRegion) {
            crop.left = std::clamp(requestedRegion->left, 0L, static_cast<LONG>(captured.width));
            crop.top = std::clamp(requestedRegion->top, 0L, static_cast<LONG>(captured.height));
            crop.right = std::clamp(requestedRegion->right, crop.left, static_cast<LONG>(captured.width));
            crop.bottom = std::clamp(requestedRegion->bottom, crop.top, static_cast<LONG>(captured.height));
        }
        const UINT outputWidth = static_cast<UINT>(crop.right - crop.left);
        const UINT outputHeight = static_cast<UINT>(crop.bottom - crop.top);
        if (outputWidth == 0 || outputHeight == 0)
            throw hresult_error(E_INVALIDARG, L"La región seleccionada está vacía");

        const UINT hdrStride = outputWidth * 8;
        std::vector<BYTE> hdrPixels(static_cast<size_t>(hdrStride) * outputHeight);
        float peak = 0.0f;
        for (UINT y = 0; y < outputHeight; ++y) {
            const auto* source = static_cast<const BYTE*>(mapped.pData) +
                static_cast<size_t>(mapped.RowPitch) * (y + crop.top) + static_cast<size_t>(crop.left) * 8;
            auto* destination = hdrPixels.data() + static_cast<size_t>(hdrStride) * y;
            std::copy_n(source, hdrStride, destination);
            const auto* halves = reinterpret_cast<const uint16_t*>(destination);
            for (UINT x = 0; x < outputWidth; ++x) {
                const float r = std::max(0.0f, HalfToFloat(halves[x * 4 + 0]));
                const float g = std::max(0.0f, HalfToFloat(halves[x * 4 + 1]));
                const float b = std::max(0.0f, HalfToFloat(halves[x * 4 + 2]));
                if (std::isfinite(r) && std::isfinite(g) && std::isfinite(b))
                    peak = std::max(peak, 0.2126f * r + 0.7152f * g + 0.0722f * b);
            }
        }
        result.peakLinear = peak / result.sdrWhiteMultiplier;

        const auto outputDir = MonthlyOutputDirectory();
        const auto base = BaseFilename();
        result.sdrPath = outputDir / (base + (settings.format == OutputFormat::Png ? L".png" : L".jpg"));

        const UINT sdrStride = outputWidth * 4;
        std::vector<BYTE> sdrPixels(static_cast<size_t>(sdrStride) * outputHeight);
        const bool toneMap = result.peakLinear > 1.01f;
        // Keep shadows untouched and progressively compress HDR highlights into SDR.
        constexpr float knee = 0.20f;
        const float boundedPeak = std::clamp(result.peakLinear, 1.01f, 16.0f);
        const float denominator = 1.0f - std::exp(-(boundedPeak - knee) / (1.0f - knee));
        for (UINT y = 0; y < outputHeight; ++y) {
            const auto* source = reinterpret_cast<const uint16_t*>(hdrPixels.data() + static_cast<size_t>(hdrStride) * y);
            auto* destination = sdrPixels.data() + static_cast<size_t>(sdrStride) * y;
            for (UINT x = 0; x < outputWidth; ++x) {
                float r = std::max(0.0f, HalfToFloat(source[x * 4 + 0])) / result.sdrWhiteMultiplier;
                float g = std::max(0.0f, HalfToFloat(source[x * 4 + 1])) / result.sdrWhiteMultiplier;
                float b = std::max(0.0f, HalfToFloat(source[x * 4 + 2])) / result.sdrWhiteMultiplier;
                const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                if (toneMap && luminance > knee) {
                    const float mappedLuminance = knee + (1.0f - knee) *
                        (1.0f - std::exp(-(luminance - knee) / (1.0f - knee))) / denominator;
                    const float scale = mappedLuminance / luminance;
                    r *= scale;
                    g *= scale;
                    b *= scale;
                }
                destination[x * 4 + 0] = LinearToSrgbByte(b);
                destination[x * 4 + 1] = LinearToSrgbByte(g);
                destination[x * 4 + 2] = LinearToSrgbByte(r);
                destination[x * 4 + 3] = 255;
            }
        }
        if (settings.format == OutputFormat::Png) {
            SaveWic(result.sdrPath, GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA,
                    outputWidth, outputHeight, sdrStride, sdrPixels.data(),
                    static_cast<UINT>(sdrPixels.size()));
        } else {
            const UINT jpegStride = outputWidth * 3;
            std::vector<BYTE> jpegPixels(static_cast<size_t>(jpegStride) * outputHeight);
            for (UINT y = 0; y < outputHeight; ++y) {
                const BYTE* source = sdrPixels.data() + static_cast<size_t>(sdrStride) * y;
                BYTE* destination = jpegPixels.data() + static_cast<size_t>(jpegStride) * y;
                for (UINT x = 0; x < outputWidth; ++x) {
                    destination[x * 3 + 0] = source[x * 4 + 0];
                    destination[x * 3 + 1] = source[x * 4 + 1];
                    destination[x * 3 + 2] = source[x * 4 + 2];
                }
            }
            SaveWic(result.sdrPath, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR,
                    outputWidth, outputHeight, jpegStride, jpegPixels.data(),
                    static_cast<UINT>(jpegPixels.size()), settings.jpegQuality / 100.0f);
        }
        result.ok = true;
    } catch (const hresult_error& error) {
        result.error = error.message() + L": " + HResultText(error.code());
        if (!result.sdrPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(result.sdrPath, ignored);
        }
    } catch (const std::exception& error) {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
        std::wstring wide(needed > 0 ? needed - 1 : 0, L'\0');
        if (needed > 1) MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, wide.data(), needed);
        result.error = wide.empty() ? L"Error inesperado" : wide;
    } catch (...) {
        result.error = L"Error inesperado durante la captura";
    }
    return result;
}

HMONITOR MonitorUnderCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) cursor = {0, 0};
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
}

float SdrWhiteLevelMultiplier(HMONITOR monitor) noexcept {
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return 1.0f;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return 1.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)
        != ERROR_SUCCESS) return 1.0f;

    for (UINT32 index = 0; index < pathCount; ++index) {
        const auto& path = paths[index];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, monitorInfo.szDevice) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            return std::clamp(static_cast<float>(white.SDRWhiteLevel) / 1000.0f, 0.5f, 10.0f);
    }
    return 1.0f;
}

struct RegionSelection {
    bool accepted = false;
    HMONITOR monitor = nullptr;
    RECT region{};
};

struct RegionSelectorState {
    bool dragging = false;
    bool done = false;
    bool accepted = false;
    POINT anchor{};
    POINT current{};
};

RECT NormalizedRect(POINT first, POINT second) {
    return {std::min(first.x, second.x), std::min(first.y, second.y),
            std::max(first.x, second.x), std::max(first.y, second.y)};
}

LRESULT CALLBACK RegionSelectorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RegionSelectorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<RegionSelectorState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message) {
    case WM_LBUTTONDOWN:
        state->dragging = true;
        state->anchor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        state->current = state->anchor;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONUP: {
        if (!state->dragging) return 0;
        state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT selected = NormalizedRect(state->anchor, state->current);
        state->accepted = selected.right - selected.left >= 3 && selected.bottom - selected.top >= 3;
        state->done = true;
        ReleaseCapture();
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN:
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            state->done = true;
            DestroyWindow(hwnd);
        } else if (wParam == VK_RETURN) {
            RECT client{};
            GetClientRect(hwnd, &client);
            state->anchor = {client.left, client.top};
            state->current = {client.right, client.bottom};
            state->accepted = true;
            state->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        constexpr COLORREF transparentKey = RGB(1, 2, 3);
        HBRUSH transparentBrush = CreateSolidBrush(transparentKey);
        FillRect(dc, &client, transparentBrush);
        DeleteObject(transparentBrush);

        if (state->dragging) {
            RECT selected = NormalizedRect(state->anchor, state->current);
            HPEN shadow = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
            HGDIOBJ oldPen = SelectObject(dc, shadow);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, selected.left - 1, selected.top - 1, selected.right + 2, selected.bottom + 2);
            SelectObject(dc, oldPen);
            DeleteObject(shadow);
            HPEN border = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            oldPen = SelectObject(dc, border);
            Rectangle(dc, selected.left, selected.top, selected.right + 1, selected.bottom + 1);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(border);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        state->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

RegionSelection SelectRegion(HMONITOR monitor) {
    RegionSelection selection;
    selection.monitor = monitor;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return selection;

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = RegionSelectorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = L"NativeHDRShot.RegionSelector";
        classRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!classRegistered) return selection;

    RegionSelectorState state;
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    HWND selector = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"NativeHDRShot.RegionSelector", L"Seleccionar región", WS_POPUP,
        info.rcMonitor.left, info.rcMonitor.top, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!selector) return selection;
    SetLayeredWindowAttributes(selector, RGB(1, 2, 3), 255, LWA_COLORKEY);
    ShowWindow(selector, SW_SHOW);
    SetForegroundWindow(selector);
    SetFocus(selector);

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!state.done && message.message == WM_QUIT) PostQuitMessage(static_cast<int>(message.wParam));
    if (IsWindow(selector)) DestroyWindow(selector);
    if (state.accepted) {
        selection.accepted = true;
        selection.region = NormalizedRect(state.anchor, state.current);
    }
    return selection;
}

class App {
public:
    int Run(HINSTANCE instance) {
        instance_ = instance;
        appIcon_ = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        if (!appIcon_) appIcon_ = LoadIconW(nullptr, IDI_APPLICATION);
        const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        taskbarCreated_ = taskbarCreated;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        wc.hIcon = appIcon_;
        wc.hIconSm = appIcon_;
        check_bool(RegisterClassExW(&wc));
        hwnd_ = CreateWindowExW(0, kWindowClass, kAppName, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, this);
        check_bool(hwnd_ != nullptr);
        AddTrayIcon();

        printScreenRegistered_ = RegisterHotKey(hwnd_, kHotkeyPrintScreen, MOD_NOREPEAT, VK_SNAPSHOT) != FALSE;
        fallbackRegistered_ = RegisterHotKey(hwnd_, kHotkeyFallback,
                                             MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12) != FALSE;
        Log(L"Inicio. PrintScreen=" + std::wstring(printScreenRegistered_ ? L"activo" : L"ocupado") +
            L", Ctrl+Shift+F12=" + std::wstring(fallbackRegistered_ ? L"activo" : L"ocupado"));
        if (!printScreenRegistered_) {
            Notify(L"PrintScreen está ocupado", fallbackRegistered_
                ? L"Usa Ctrl+Shift+F12. Puedes desactivar «Usar Impr Pant para abrir Recortes» en Configuración."
                : L"También está ocupado Ctrl+Shift+F12; revisa otras aplicaciones de captura.", NIIF_WARNING);
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RemoveTrayIcon();
        return static_cast<int>(message.wParam);
    }

private:
    static void UpdateSettingsControls(HWND window) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return;
        const HWND format = GetDlgItem(window, kSettingsFormat);
        const HWND slider = GetDlgItem(window, kSettingsQuality);
        const HWND value = GetDlgItem(window, kSettingsQualityValue);
        const bool jpeg = SendMessageW(format, CB_GETCURSEL, 0, 0) == 1;
        EnableWindow(slider, jpeg);
        const int quality = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        std::wstring label = jpeg ? std::to_wstring(quality) + L" %" : L"Sin pérdida";
        SetWindowTextW(value, label.c_str());
    }

    static LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);

        switch (message) {
        case WM_CREATE: {
            Log(L"Ventana de configuración creada");
            HFONT font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            SetPropW(window, L"NativeHDRShot.SettingsFont", font);
            auto createControl = [&](PCWSTR kind, PCWSTR text, DWORD style,
                                     int x, int y, int width, int height, UINT id) {
                HWND control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
                    x, y, width, height, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                    self->instance_, nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                return control;
            };
            createControl(L"STATIC", L"Formato", 0, 24, 24, 110, 24, 0);
            HWND format = createControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                        145, 20, 235, 200, kSettingsFormat);
            SendMessageW(format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG — máxima calidad"));
            SendMessageW(format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"JPEG — archivo más pequeño"));
            SendMessageW(format, CB_SETCURSEL,
                         self->settings_.format == OutputFormat::Png ? 0 : 1, 0);

            createControl(L"STATIC", L"Calidad JPEG", 0, 24, 78, 110, 24, 0);
            HWND slider = createControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                                        140, 70, 190, 42, kSettingsQuality);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 100));
            SendMessageW(slider, TBM_SETTICFREQ, 10, 0);
            SendMessageW(slider, TBM_SETPOS, TRUE, self->settings_.jpegQuality);
            createControl(L"STATIC", L"", SS_CENTER, 335, 78, 55, 24, kSettingsQualityValue);

            createControl(L"BUTTON", L"Guardar", BS_DEFPUSHBUTTON | WS_TABSTOP,
                          205, 140, 86, 32, kSettingsSave);
            createControl(L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP,
                          300, 140, 86, 32, kSettingsCancel);
            UpdateSettingsControls(window);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kSettingsFormat && HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateSettingsControls(window);
            } else if (LOWORD(wParam) == kSettingsSave) {
                CaptureSettings updated;
                updated.format = SendMessageW(GetDlgItem(window, kSettingsFormat), CB_GETCURSEL, 0, 0) == 1
                    ? OutputFormat::Jpeg : OutputFormat::Png;
                updated.jpegQuality = static_cast<int>(SendMessageW(
                    GetDlgItem(window, kSettingsQuality), TBM_GETPOS, 0, 0));
                try {
                    SaveSettings(updated);
                    self->settings_ = updated;
                    self->Notify(L"Configuración guardada",
                        updated.format == OutputFormat::Png
                            ? L"Las capturas se guardarán como PNG sin pérdida."
                            : (L"Las capturas se guardarán como JPEG al " +
                               std::to_wstring(updated.jpegQuality) + L" %."), NIIF_INFO);
                    DestroyWindow(window);
                } catch (const hresult_error& error) {
                    MessageBoxW(window, error.message().c_str(), kAppName, MB_OK | MB_ICONERROR);
                }
            } else if (LOWORD(wParam) == kSettingsCancel) {
                DestroyWindow(window);
            }
            return 0;
        case WM_HSCROLL:
            UpdateSettingsControls(window);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY: {
            Log(L"Ventana de configuración cerrada");
            self->settingsWindow_ = nullptr;
            HFONT font = reinterpret_cast<HFONT>(RemovePropW(window, L"NativeHDRShot.SettingsFont"));
            if (font) DeleteObject(font);
            return 0;
        }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (self) return self->HandleMessage(message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == taskbarCreated_) {
            AddTrayIcon();
            return 0;
        }
        switch (message) {
        case WM_HOTKEY:
            StartCapture();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kMenuCapture) StartCapture();
            else if (LOWORD(wParam) == kMenuOpenFolder) {
                const auto path = OutputRoot();
                std::filesystem::create_directories(path);
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (LOWORD(wParam) == kMenuSettings) {
                ShowSettings();
            } else if (LOWORD(wParam) == kMenuExit) {
                DestroyWindow(hwnd_);
            }
            return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) StartCapture();
            else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case kCaptureDone: {
            std::unique_ptr<CaptureResult> result(reinterpret_cast<CaptureResult*>(lParam));
            captureActive_ = false;
            if (result->ok) {
                std::wostringstream messageText;
                messageText << L"Guardado: " << result->sdrPath.filename().wstring()
                            << L" (pico SDR " << std::fixed << std::setprecision(2) << result->peakLinear
                            << L", blanco SDR x" << result->sdrWhiteMultiplier << L")";
                Log(messageText.str());
                Notify(L"Captura completada", messageText.str(), NIIF_INFO);
            } else {
                Log(L"ERROR: " + result->error);
                Notify(L"No se pudo capturar", result->error, NIIF_ERROR);
            }
            return 0;
        }
        case WM_DESTROY:
            UnregisterHotKey(hwnd_, kHotkeyPrintScreen);
            UnregisterHotKey(hwnd_, kHotkeyFallback);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    void StartCapture() {
        if (captureActive_.exchange(true)) {
            Notify(L"Captura en curso", L"Espera a que termine la captura anterior.", NIIF_INFO);
            return;
        }
        const auto selection = SelectRegion(MonitorUnderCursor());
        if (!selection.accepted) {
            captureActive_ = false;
            Log(L"Selección cancelada");
            return;
        }
        Log(L"Captura solicitada");
        const CaptureSettings settings = settings_;
        std::thread([this, selection, settings] {
            init_apartment(apartment_type::multi_threaded);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            auto result = std::make_unique<CaptureResult>(
                CaptureMonitor(selection.monitor, settings, &selection.region));
            PostMessageW(hwnd_, kCaptureDone, 0, reinterpret_cast<LPARAM>(result.release()));
        }).detach();
    }

    void AddTrayIcon() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = hwnd_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = appIcon_;
        wcscpy_s(tray_.szTip, kAppName);
        Shell_NotifyIconW(NIM_ADD, &tray_);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void RemoveTrayIcon() {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
    }

    void Notify(const std::wstring& title, const std::wstring& text, DWORD flags) {
        tray_.uFlags = NIF_INFO;
        wcsncpy_s(tray_.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(tray_.szInfo, text.c_str(), _TRUNCATE);
        tray_.dwInfoFlags = flags | NIIF_NOSOUND;
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void ShowTrayMenu() {
        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuCapture, L"Capturar región");
        AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Abrir carpeta de capturas");
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"Configuración…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Salir");
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void ShowSettings() {
        Log(L"Abriendo configuración");
        if (settingsWindow_ && IsWindow(settingsWindow_)) {
            ShowWindow(settingsWindow_, SW_RESTORE);
            SetForegroundWindow(settingsWindow_);
            return;
        }
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = SettingsWindowProc;
            wc.hInstance = instance_;
            wc.hIcon = appIcon_;
            wc.hIconSm = appIcon_;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = L"NativeHDRShot.SettingsWindow";
            classRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        if (!classRegistered) {
            Log(L"ERROR registrando ventana de configuración: " + HResultText(HRESULT_FROM_WIN32(GetLastError())));
            return;
        }

        RECT desired{0, 0, 420, 220};
        AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                           WS_EX_DLGMODALFRAME);
        const int width = desired.right - desired.left;
        const int height = desired.bottom - desired.top;
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorUnderCursor(), &monitor);
        const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
        const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
        settingsWindow_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
            L"NativeHDRShot.SettingsWindow", L"NativeHDRShot — Configuración",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height,
            nullptr, nullptr, instance_, this);
        if (settingsWindow_) {
            ShowWindow(settingsWindow_, SW_SHOW);
            SetForegroundWindow(settingsWindow_);
        } else {
            Log(L"ERROR creando ventana de configuración: " + HResultText(HRESULT_FROM_WIN32(GetLastError())));
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HICON appIcon_ = nullptr;
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_ = 0;
    bool printScreenRegistered_ = false;
    bool fallbackRegistered_ = false;
    CaptureSettings settings_ = LoadSettings();
    std::atomic<bool> captureActive_{false};
};

bool HasArgument(PCWSTR wanted) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return false;
    const auto release = std::unique_ptr<void, decltype(&LocalFree)>(args, LocalFree);
    for (int i = 1; i < count; ++i) if (_wcsicmp(args[i], wanted) == 0) return true;
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&commonControls);
        init_apartment(apartment_type::multi_threaded);
        const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\NativeHDRShot.SingleInstance");
        const bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
        const bool captureOnce = HasArgument(L"--capture") || HasArgument(L"--capture-silent");
        if (captureOnce) {
            const auto result = CaptureMonitor(MonitorUnderCursor(), LoadSettings());
            if (result.ok) {
                Log(L"Captura puntual guardada: " + result.sdrPath.wstring());
            } else {
                Log(L"ERROR en captura puntual: " + result.error);
            }
            if (!HasArgument(L"--capture-silent")) {
                MessageBoxW(nullptr,
                    result.ok ? result.sdrPath.c_str() : result.error.c_str(),
                    result.ok ? L"Captura completada" : L"Error de captura",
                    MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));
            }
            if (mutex) CloseHandle(mutex);
            return result.ok ? 0 : 1;
        }
        if (alreadyRunning) {
            MessageBoxW(nullptr, L"NativeHDRShot ya está en ejecución en el área de notificación.",
                        kAppName, MB_OK | MB_ICONINFORMATION);
            if (mutex) CloseHandle(mutex);
            return 0;
        }
        App app;
        const int exitCode = app.Run(instance);
        if (mutex) CloseHandle(mutex);
        return exitCode;
    } catch (const hresult_error& error) {
        const auto text = error.message() + L": " + HResultText(error.code());
        Log(std::wstring(text.c_str()));
        MessageBoxW(nullptr, text.c_str(), kAppName, MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"Error fatal inesperado.", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
}
