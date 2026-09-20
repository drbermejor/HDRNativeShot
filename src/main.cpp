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
#include "image_output.h"
#include "capture_editor.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace image_output;
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
constexpr UINT kHookCaptureRequested = WM_APP + 3;
constexpr UINT kPreviewReady = WM_APP + 4;
constexpr UINT kRegionSelectionFinalize = WM_APP + 50;
constexpr UINT_PTR kCaptureNotificationTimer = 1;
constexpr UINT_PTR kRegionTopmostTimer = 2;
constexpr UINT_PTR kCaptureDelayTimer = 3;
constexpr UINT kMenuCapture = 100;
constexpr UINT kMenuOpenFolder = 101;
constexpr UINT kMenuSettings = 102;
constexpr UINT kMenuExit = 103;
constexpr UINT kMenuRecoverPrintScreen = 104;
constexpr UINT kMenuWindow = 105;
constexpr UINT kMenuMonitor = 106;
constexpr UINT kMenuDesktop = 107;
constexpr UINT kMenuEditor = 108;
constexpr UINT kMenuAutoEditor = 109;
constexpr UINT kMenuCancelDelay = 110;
constexpr UINT kMenuDelayBase = 120;
enum class CaptureMode { Region, Window, Monitor, Desktop };
constexpr UINT kSettingsFormat = 200;
constexpr UINT kSettingsQuality = 201;
constexpr UINT kSettingsQualityValue = 202;
constexpr UINT kSettingsSave = 203;
constexpr UINT kSettingsCancel = 204;
constexpr UINT kSettingsPrintScreenStatus = 205;
constexpr UINT kSettingsRecoverPrintScreen = 206;
constexpr UINT kSettingsPreset = 207;
constexpr UINT kSettingsSize = 208;
constexpr UINT kSettingsChroma = 209;
constexpr int kHotkeyPrintScreen = 1;
constexpr int kHotkeyFallback = 2;

struct CaptureResult {
    bool ok = false;
    bool clipboardCopied = false;
    std::filesystem::path sdrPath;
    std::wstring error;
    std::wstring clipboardError;
    float peakLinear = 0.0f;
    float sdrWhiteMultiplier = 1.0f;
    UINT pixelWidth = 0;
    UINT pixelHeight = 0;
    UINT pixelStride = 0;
    std::vector<BYTE> sdrPixels;
    RECT captureBounds{};
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
        settings.jpegQuality = static_cast<int>(std::clamp(value, 30ul, 100ul));
    }
    value = 0;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NativeHDRShot", L"JpegFullChroma",
                    RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value <= 1)
        settings.jpegFullChroma = value != 0;
    value = 0;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NativeHDRShot", L"MaxDimension",
                    RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        std::find(std::begin(kSizeLimits), std::end(kSizeLimits), value) != std::end(kSizeLimits))
        settings.maxDimension = value;
    value = 1;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NativeHDRShot", L"OpenEditor",
                    RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value <= 1)
        settings.openEditor = value != 0;
    value = 0;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NativeHDRShot", L"DelaySeconds",
                    RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        (value == 0 || value == 3 || value == 5 || value == 10)) settings.delaySeconds = value;
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
    const DWORD chroma = settings.jpegFullChroma ? 1 : 0;
    const DWORD dimension = settings.maxDimension;
    const LSTATUS chromaStatus = RegSetValueExW(
        key, L"JpegFullChroma", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&chroma), sizeof(chroma));
    const LSTATUS dimensionStatus = RegSetValueExW(
        key, L"MaxDimension", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dimension), sizeof(dimension));
    const DWORD editor = settings.openEditor ? 1 : 0;
    const DWORD delay = settings.delaySeconds;
    const LSTATUS editorStatus = RegSetValueExW(
        key, L"OpenEditor", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&editor), sizeof(editor));
    const LSTATUS delayStatus = RegSetValueExW(
        key, L"DelaySeconds", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&delay), sizeof(delay));
    RegCloseKey(key);
    check_hresult(HRESULT_FROM_WIN32(editorStatus));
    check_hresult(HRESULT_FROM_WIN32(delayStatus));
    check_hresult(HRESULT_FROM_WIN32(chromaStatus));
    check_hresult(HRESULT_FROM_WIN32(dimensionStatus));
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

bool IsProcessElevated() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
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

CapturedTexture CaptureTexture(HMONITOR monitor, HWND window = nullptr) {
    ComPtr<ID3D11DeviceContext> context;
    auto d3dDevice = CreateDeviceForMonitor(monitor, context);
    const auto winrtDevice = ToWinRtDevice(d3dDevice);
    auto item = ItemForMonitor(monitor);
    if (window) {
        auto interop = get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        item = nullptr;
        check_hresult(interop->CreateForWindow(window, guid_of<WGC::GraphicsCaptureItem>(), put_abi(item)));
    }
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
        ~SharedState() { if (eventHandle) CloseHandle(eventHandle); }
    };
    auto state = std::make_shared<SharedState>();
    state->eventHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->eventHandle) throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));

    const auto token = pool.FrameArrived([state, d3dDevice, context](WGC::Direct3D11CaptureFramePool const& sender,
                                              winrt::Windows::Foundation::IInspectable const&) noexcept {
        if (state->completed.exchange(true)) return;
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) throw hresult_error(E_FAIL, L"Fotograma vacío");
            const auto content = frame.ContentSize();
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> source;
            check_hresult(access->GetInterface(IID_PPV_ARGS(&source)));
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            if (content.Width <= 0 || content.Height <= 0 ||
                static_cast<UINT>(content.Width) > desc.Width || static_cast<UINT>(content.Height) > desc.Height)
                throw hresult_error(HRESULT_FROM_WIN32(ERROR_RETRY), L"La pantalla cambió de tamaño; vuelve a capturar");
            desc.Width = static_cast<UINT>(content.Width);
            desc.Height = static_cast<UINT>(content.Height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            check_hresult(d3dDevice->CreateTexture2D(&desc, nullptr, &state->result.staging));
            D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
            context->CopySubresourceRegion(state->result.staging.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
            context->Flush();
            state->result.width = desc.Width;
            state->result.height = desc.Height;
            state->hr = S_OK;
        } catch (const hresult_error& error) {
            state->hr = error.code();
        } catch (...) {
            state->hr = E_FAIL;
        }
        SetEvent(state->eventHandle);
    });

    session.StartCapture();
    const DWORD wait = WaitForSingleObject(state->eventHandle, 5000);
    pool.FrameArrived(token);
    session.Close();
    pool.Close();
    if (wait != WAIT_OBJECT_0) throw hresult_error(HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()),
                                                    L"Windows no entregó ningún fotograma en 5 segundos");
    check_hresult(state->hr);
    return std::move(state->result);
}

float SdrWhiteLevelMultiplier(HMONITOR monitor) noexcept;

CaptureResult CaptureMonitor(HMONITOR monitor, const CaptureSettings& settings,
                             const RECT* requestedRegion = nullptr,
                             HWND clipboardOwner = nullptr,
                             bool deferSave = false, HWND window = nullptr) {
    CaptureResult result;
    try {
        result.sdrWhiteMultiplier = SdrWhiteLevelMultiplier(monitor);
        auto captured = CaptureTexture(monitor, window);
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
        UINT outputWidth = static_cast<UINT>(crop.right - crop.left);
        UINT outputHeight = static_cast<UINT>(crop.bottom - crop.top);
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

        UINT sdrStride = outputWidth * 4;
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
        if (deferSave) {
            result.pixelWidth = outputWidth;
            result.pixelHeight = outputHeight;
            result.pixelStride = sdrStride;
            result.sdrPixels = std::move(sdrPixels);
            MONITORINFO monitorInfo{sizeof(monitorInfo)};
            if (GetMonitorInfoW(monitor, &monitorInfo)) result.captureBounds = monitorInfo.rcMonitor;
            result.sdrPath.clear();
            result.ok = true;
            return result;
        }
        ResizeSdr(outputWidth, outputHeight, sdrStride, sdrPixels, settings.maxDimension);
        result.sdrPath = MonthlyOutputDirectory() /
            (BaseFilename() + (settings.format == OutputFormat::Png ? L".png" : L".jpg"));
        SaveSdrFile(result.sdrPath, settings, outputWidth, outputHeight, sdrStride, sdrPixels);
        result.clipboardCopied = CopySdrToClipboard(
            clipboardOwner, outputWidth, outputHeight, sdrStride, sdrPixels.data(), result.clipboardError);
        result.pixelWidth = outputWidth;
        result.pixelHeight = outputHeight;
        result.pixelStride = sdrStride;
        result.sdrPixels = std::move(sdrPixels);
        result.ok = true;
    } catch (const hresult_error& error) {
        result.error = error.message() + L": " + HResultText(error.code());
        if (!result.sdrPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(result.sdrPath, ignored);
        }
    } catch (const std::exception& error) {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
        std::wstring wide(needed > 0 ? needed : 0, L'\0');
        if (needed > 1) MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, wide.data(), needed);
        if (!wide.empty()) wide.pop_back();
        result.error = wide.empty() ? L"Error inesperado" : wide;
    } catch (...) {
        result.error = L"Error inesperado durante la captura";
    }
    return result;
}

struct MonitorDescriptor {
    HMONITOR handle = nullptr;
    RECT bounds{};
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT bounds, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<MonitorDescriptor>*>(data);
    monitors->push_back({monitor, *bounds});
    return TRUE;
}

CaptureResult CaptureVirtualDesktop(const CaptureSettings& settings, HMONITOR referenceMonitor) {
    CaptureResult result;
    try {
        const auto started = std::chrono::steady_clock::now();
        std::vector<MonitorDescriptor> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                                 reinterpret_cast<LPARAM>(&monitors)) || monitors.empty()) {
            throw hresult_error(HRESULT_FROM_WIN32(GetLastError()),
                                L"No se pudieron enumerar los monitores");
        }

        RECT desktop = monitors.front().bounds;
        for (const auto& monitor : monitors) {
            desktop.left = std::min(desktop.left, monitor.bounds.left);
            desktop.top = std::min(desktop.top, monitor.bounds.top);
            desktop.right = std::max(desktop.right, monitor.bounds.right);
            desktop.bottom = std::max(desktop.bottom, monitor.bounds.bottom);
        }
        const LONG desktopWidth = desktop.right - desktop.left;
        const LONG desktopHeight = desktop.bottom - desktop.top;
        if (desktopWidth <= 0 || desktopHeight <= 0)
            throw hresult_error(E_INVALIDARG, L"El escritorio virtual está vacío");

        result.pixelWidth = static_cast<UINT>(desktopWidth);
        result.pixelHeight = static_cast<UINT>(desktopHeight);
        result.pixelStride = result.pixelWidth * 4;
        result.captureBounds = desktop;
        result.sdrPixels.resize(static_cast<size_t>(result.pixelStride) * result.pixelHeight);
        for (size_t pixel = 0; pixel < result.sdrPixels.size(); pixel += 4) {
            result.sdrPixels[pixel + 0] = 18;
            result.sdrPixels[pixel + 1] = 18;
            result.sdrPixels[pixel + 2] = 18;
            result.sdrPixels[pixel + 3] = 255;
        }

        std::vector<std::future<CaptureResult>> captures;
        captures.reserve(monitors.size());
        for (const auto& monitor : monitors) {
            const HMONITOR handle = monitor.handle;
            captures.push_back(std::async(std::launch::async, [handle, settings] {
                init_apartment(apartment_type::multi_threaded);
                return CaptureMonitor(handle, settings, nullptr, nullptr, true);
            }));
        }

        for (size_t monitorIndex = 0; monitorIndex < monitors.size(); ++monitorIndex) {
            const auto& monitor = monitors[monitorIndex];
            CaptureResult part = captures[monitorIndex].get();
            if (!part.ok) {
                result.error = L"No se pudo congelar uno de los monitores: " + part.error;
                return result;
            }

            const LONG destinationLeft = monitor.bounds.left - desktop.left;
            const LONG destinationTop = monitor.bounds.top - desktop.top;
            const UINT destinationWidth = static_cast<UINT>(monitor.bounds.right - monitor.bounds.left);
            const UINT destinationHeight = static_cast<UINT>(monitor.bounds.bottom - monitor.bounds.top);
            if (destinationWidth == 0 || destinationHeight == 0 ||
                part.pixelWidth == 0 || part.pixelHeight == 0) {
                result.error = L"Uno de los monitores devolvió un fotograma vacío";
                return result;
            }

            if (destinationWidth == part.pixelWidth && destinationHeight == part.pixelHeight) {
                for (UINT y = 0; y < destinationHeight; ++y) {
                    const BYTE* source = part.sdrPixels.data() +
                        static_cast<size_t>(part.pixelStride) * y;
                    BYTE* destination = result.sdrPixels.data() +
                        static_cast<size_t>(result.pixelStride) * (destinationTop + y) +
                        static_cast<size_t>(destinationLeft) * 4;
                    std::copy_n(source, static_cast<size_t>(destinationWidth) * 4, destination);
                }
            } else {
                for (UINT y = 0; y < destinationHeight; ++y) {
                    const UINT sourceY = static_cast<UINT>(
                        static_cast<uint64_t>(y) * part.pixelHeight / destinationHeight);
                    BYTE* destination = result.sdrPixels.data() +
                        static_cast<size_t>(result.pixelStride) * (destinationTop + y) +
                        static_cast<size_t>(destinationLeft) * 4;
                    for (UINT x = 0; x < destinationWidth; ++x) {
                        const UINT sourceX = static_cast<UINT>(
                            static_cast<uint64_t>(x) * part.pixelWidth / destinationWidth);
                        const BYTE* source = part.sdrPixels.data() +
                            static_cast<size_t>(part.pixelStride) * sourceY +
                            static_cast<size_t>(sourceX) * 4;
                        std::copy_n(source, 4, destination + static_cast<size_t>(x) * 4);
                    }
                }
            }

            result.peakLinear = std::max(result.peakLinear, part.peakLinear);
            if (monitor.handle == referenceMonitor)
                result.sdrWhiteMultiplier = part.sdrWhiteMultiplier;
        }

        result.ok = true;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        Log(L"Fotograma virtual compuesto con " + std::to_wstring(monitors.size()) +
            L" monitores en " + std::to_wstring(elapsed) + L" ms");
    } catch (const hresult_error& error) {
        result.error = error.message() + L": " + HResultText(error.code());
    } catch (...) {
        result.error = L"Error inesperado al componer el escritorio virtual";
    }
    return result;
}

CaptureResult SavePreparedCrop(const CaptureResult& prepared, const RECT& requestedRegion,
                               const CaptureSettings& settings, HWND clipboardOwner) {
    CaptureResult result;
    result.peakLinear = prepared.peakLinear;
    result.sdrWhiteMultiplier = prepared.sdrWhiteMultiplier;
    try {
        if (!prepared.ok || prepared.sdrPixels.empty() || prepared.pixelStride == 0)
            throw hresult_error(E_INVALIDARG, L"El fotograma previo no contiene píxeles SDR");

        RECT crop{};
        crop.left = std::clamp(requestedRegion.left, 0L, static_cast<LONG>(prepared.pixelWidth));
        crop.top = std::clamp(requestedRegion.top, 0L, static_cast<LONG>(prepared.pixelHeight));
        crop.right = std::clamp(requestedRegion.right, crop.left, static_cast<LONG>(prepared.pixelWidth));
        crop.bottom = std::clamp(requestedRegion.bottom, crop.top, static_cast<LONG>(prepared.pixelHeight));
        UINT width = static_cast<UINT>(crop.right - crop.left);
        UINT height = static_cast<UINT>(crop.bottom - crop.top);
        if (width == 0 || height == 0)
            throw hresult_error(E_INVALIDARG, L"La región seleccionada está vacía");

        UINT stride = width * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
        for (UINT y = 0; y < height; ++y) {
            const BYTE* source = prepared.sdrPixels.data() +
                static_cast<size_t>(prepared.pixelStride) * (y + crop.top) +
                static_cast<size_t>(crop.left) * 4;
            std::copy_n(source, stride, pixels.data() + static_cast<size_t>(stride) * y);
        }

        ResizeSdr(width, height, stride, pixels, settings.maxDimension);
        result.sdrPath = MonthlyOutputDirectory() /
            (BaseFilename() + (settings.format == OutputFormat::Png ? L".png" : L".jpg"));
        SaveSdrFile(result.sdrPath, settings, width, height, stride, pixels);
        result.clipboardCopied = CopySdrToClipboard(
            clipboardOwner, width, height, stride, pixels.data(), result.clipboardError);
        result.pixelWidth = width;
        result.pixelHeight = height;
        result.pixelStride = stride;
        result.sdrPixels = std::move(pixels);
        result.ok = true;
    } catch (const hresult_error& error) {
        result.error = error.message() + L": " + HResultText(error.code());
        if (!result.sdrPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(result.sdrPath, ignored);
        }
    } catch (...) {
        result.error = L"Error inesperado al guardar la región congelada";
    }
    return result;
}

HMONITOR MonitorUnderCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) cursor = {0, 0};
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
}

HMONITOR CaptureTargetMonitor(bool& usedFullscreenWindow) {
    usedFullscreenWindow = false;
    const HWND foreground = GetForegroundWindow();
    if (foreground && IsWindowVisible(foreground) && !IsIconic(foreground)) {
        const HMONITOR foregroundMonitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        RECT windowRect{};
        RECT intersection{};
        if (foregroundMonitor && GetMonitorInfoW(foregroundMonitor, &monitorInfo) &&
            GetWindowRect(foreground, &windowRect) &&
            IntersectRect(&intersection, &windowRect, &monitorInfo.rcMonitor)) {
            const int64_t monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
            const int64_t monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
            const int64_t coveredWidth = intersection.right - intersection.left;
            const int64_t coveredHeight = intersection.bottom - intersection.top;
            const int64_t monitorArea = monitorWidth * monitorHeight;
            const int64_t coveredArea = coveredWidth * coveredHeight;
            if (monitorArea > 0 && coveredArea * 100 >= monitorArea * 90) {
                usedFullscreenWindow = true;
                return foregroundMonitor;
            }
        }
    }
    return MonitorUnderCursor();
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
    HWND window = nullptr;
    bool accepted = false;
    HMONITOR monitor = nullptr;
    RECT region{};
};

struct WindowTarget { HWND window; RECT bounds; };
struct RegionSelectorState {
    CaptureMode mode = CaptureMode::Region;
    std::vector<WindowTarget> windows;
    HWND selectedWindow = nullptr;
    RECT hover{};
    bool dragging = false;
    bool done = false;
    bool accepted = false;
    POINT anchor{};
    POINT current{};
    const CaptureResult* preview = nullptr;
    bool foregroundRecovered = false;
};

RegionSelectorState* gActiveRegionState = nullptr;
HWND gActiveRegionWindow = nullptr;

RECT NormalizedRect(POINT first, POINT second) {
    return {std::min(first.x, second.x), std::min(first.y, second.y),
            std::max(first.x, second.x), std::max(first.y, second.y)};
}

POINT RegionClientPoint(HWND window, POINT screenPoint) {
    ScreenToClient(window, &screenPoint);
    return screenPoint;
}

bool BringToForeground(HWND window) {
    const HWND foreground = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = foregroundThread && foregroundThread != currentThread &&
        AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;

    ShowWindowAsync(window, SW_SHOW);
    BringWindowToTop(window);
    SwitchToThisWindow(window, TRUE);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);

    if (attached) AttachThreadInput(currentThread, foregroundThread, FALSE);
    return GetForegroundWindow() == window;
}

LRESULT CALLBACK RegionMouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || !gActiveRegionState || !IsWindow(gActiveRegionWindow))
        return CallNextHookEx(nullptr, code, wParam, lParam);

    auto* state = gActiveRegionState;
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    const POINT point = RegionClientPoint(gActiveRegionWindow, event->pt);
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));

    switch (wParam) {
    case WM_LBUTTONDOWN:
        state->dragging = true;
        state->anchor = point;
        state->current = point;
        InvalidateRect(gActiveRegionWindow, nullptr, TRUE);
        return 1;
    case WM_MOUSEMOVE:
        state->current = point;
        InvalidateRect(gActiveRegionWindow, nullptr, TRUE);
        // Let Windows update the pointer position; only mouse buttons are consumed.
        return CallNextHookEx(nullptr, code, wParam, lParam);
    case WM_LBUTTONUP: {
        if (!state->dragging) return 1;
        state->current = point;
        const RECT selected = NormalizedRect(state->anchor, state->current);
        if (selected.right - selected.left >= 3 && selected.bottom - selected.top >= 3) {
            state->accepted = true;
            state->done = true;
            PostMessageW(gActiveRegionWindow, kRegionSelectionFinalize, 0, 0);
        } else {
            // Ignore an accidental click; keep the selector open for a real drag.
            state->dragging = false;
            InvalidateRect(gActiveRegionWindow, nullptr, TRUE);
        }
        return 1;
    }
    case WM_RBUTTONDOWN:
        state->done = true;
        PostMessageW(gActiveRegionWindow, kRegionSelectionFinalize, 0, 0);
        return 1;
    default:
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
}

void UpdateSelectionHover(RegionSelectorState& state, HWND window) {
    POINT point = state.current;
    ClientToScreen(window, &point);
    RECT screen{};
    state.selectedWindow = nullptr;
    if (state.mode == CaptureMode::Window) {
        for (const auto& target : state.windows) {
            if (PtInRect(&target.bounds, point)) {
                screen = target.bounds;
                state.selectedWindow = target.window;
                break;
            }
        }
    } else if (state.mode == CaptureMode::Monitor) {
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info)) screen = info.rcMonitor;
    }
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&screen), 2);
    RECT client{};
    GetClientRect(window, &client);
    IntersectRect(&state.hover, &screen, &client);
}

BOOL CALLBACK CollectWindowTarget(HWND window, LPARAM data) {
    if (!IsWindowVisible(window) || IsIconic(window) || window == GetShellWindow()) return TRUE;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
        GetWindowRect(window, &bounds);
    if (bounds.right > bounds.left && bounds.bottom > bounds.top)
        reinterpret_cast<std::vector<WindowTarget>*>(data)->push_back({window, bounds});
    return TRUE;
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
    case WM_COMMAND:
        if (LOWORD(wParam) >= 500 && LOWORD(wParam) <= 502) {
            state->mode = static_cast<CaptureMode>(LOWORD(wParam) - 500);
            state->dragging = false;
            UpdateSelectionHover(*state, hwnd);
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (LOWORD(wParam) == 503) SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        else if (LOWORD(wParam) == 504) SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        return 0;
    case WM_LBUTTONDOWN:
        state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state->mode != CaptureMode::Region) {
            UpdateSelectionHover(*state, hwnd);
            if (!IsRectEmpty(&state->hover)) {
                state->anchor = {state->hover.left, state->hover.top};
                state->current = {state->hover.right, state->hover.bottom};
                state->accepted = true;
                state->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        state->dragging = true;
        state->anchor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        state->current = state->anchor;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state->mode != CaptureMode::Region) UpdateSelectionHover(*state, hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONUP: {
        if (!state->dragging) return 0;
        state->current = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT selected = NormalizedRect(state->anchor, state->current);
        ReleaseCapture();
        if (selected.right - selected.left >= 3 && selected.bottom - selected.top >= 3) {
            state->accepted = true;
            state->done = true;
            DestroyWindow(hwnd);
        } else {
            state->dragging = false;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'R' || wParam == 'V' || wParam == 'P') {
            SendMessageW(hwnd, WM_COMMAND, wParam == 'R' ? 500 : wParam == 'V' ? 501 : 502, 0);
        } else if (wParam == VK_ESCAPE) {
            state->done = true;
            DestroyWindow(hwnd);
        } else if (wParam == VK_RETURN) {
            RECT client{};
            GetClientRect(hwnd, &client);
            state->selectedWindow = nullptr;
            state->anchor = {client.left, client.top};
            state->current = {client.right, client.bottom};
            state->accepted = true;
            state->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kRegionTopmostTimer) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
            if (GetForegroundWindow() != hwnd) {
                const bool recovered = BringToForeground(hwnd);
                if (recovered && !state->foregroundRecovered) {
                    state->foregroundRecovered = true;
                    Log(L"Selector: primer plano recuperado frente a otra ventana");
                }
            }
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return state->preview ? MA_ACTIVATE : MA_NOACTIVATE;
    case kRegionSelectionFinalize:
        DestroyWindow(hwnd);
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
        if (state->preview && !state->preview->sdrPixels.empty()) {
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = static_cast<LONG>(state->preview->pixelWidth);
            bitmap.bmiHeader.biHeight = -static_cast<LONG>(state->preview->pixelHeight);
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, client.right, client.bottom,
                          0, 0, state->preview->pixelWidth, state->preview->pixelHeight,
                          state->preview->sdrPixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
        } else {
            constexpr COLORREF transparentKey = RGB(1, 2, 3);
            HBRUSH transparentBrush = CreateSolidBrush(transparentKey);
            FillRect(dc, &client, transparentBrush);
            DeleteObject(transparentBrush);
        }

        if (state->dragging || state->mode != CaptureMode::Region) {
            RECT selected = state->dragging ? NormalizedRect(state->anchor, state->current) : state->hover;
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

        HPEN crossShadow = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
        HGDIOBJ oldPen = SelectObject(dc, crossShadow);
        MoveToEx(dc, state->current.x - 10, state->current.y, nullptr);
        LineTo(dc, state->current.x + 11, state->current.y);
        MoveToEx(dc, state->current.x, state->current.y - 10, nullptr);
        LineTo(dc, state->current.x, state->current.y + 11);
        SelectObject(dc, oldPen);
        DeleteObject(crossShadow);
        HPEN cross = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        oldPen = SelectObject(dc, cross);
        MoveToEx(dc, state->current.x - 10, state->current.y, nullptr);
        LineTo(dc, state->current.x + 11, state->current.y);
        MoveToEx(dc, state->current.x, state->current.y - 10, nullptr);
        LineTo(dc, state->current.x, state->current.y + 11);
        SelectObject(dc, oldPen);
        DeleteObject(cross);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kRegionTopmostTimer);
        state->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

RegionSelection SelectRegion(HMONITOR monitor, const CaptureResult* preview = nullptr, CaptureMode mode = CaptureMode::Region) {
    RegionSelection selection;
    selection.monitor = monitor;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return selection;
    RECT selectorBounds = info.rcMonitor;
    if (preview && preview->captureBounds.right > preview->captureBounds.left &&
        preview->captureBounds.bottom > preview->captureBounds.top) {
        selectorBounds = preview->captureBounds;
    }

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
    state.preview = preview;
    state.mode = mode;
    EnumWindows(CollectWindowTarget, reinterpret_cast<LPARAM>(&state.windows));
    const int width = selectorBounds.right - selectorBounds.left;
    const int height = selectorBounds.bottom - selectorBounds.top;
    const DWORD selectorExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        (preview ? 0 : WS_EX_NOACTIVATE | WS_EX_LAYERED);
    HWND selector = CreateWindowExW(
        selectorExStyle,
        L"NativeHDRShot.RegionSelector", L"Seleccionar captura", WS_POPUP | WS_CLIPCHILDREN,
        selectorBounds.left, selectorBounds.top, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!selector) return selection;
    const UINT dpi = GetDpiForWindow(selector);
    const int buttonWidth = MulDiv(110, dpi, 96);
    const int toolbarX = info.rcMonitor.left - selectorBounds.left +
        std::max(0L, (info.rcMonitor.right - info.rcMonitor.left - buttonWidth * 5) / 2);
    const int toolbarY = info.rcMonitor.top - selectorBounds.top + MulDiv(16, dpi, 96);
    UINT index = 0;
    for (PCWSTR label : {L"Región (R)", L"Ventana (V)", L"Pantalla (P)", L"Todo (Enter)", L"Cancelar (Esc)"}) {
        HWND button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            toolbarX + static_cast<int>(index) * buttonWidth, toolbarY, buttonWidth, MulDiv(34, dpi, 96),
            selector, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(500 + index)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        ++index;
    }

    RECT previousClip{};
    const bool restoreClip = GetClipCursor(&previousClip) != FALSE;
    ClipCursor(nullptr);

    POINT cursor{};
    GetCursorPos(&cursor);
    state.current = RegionClientPoint(selector, cursor);
    UpdateSelectionHover(state, selector);

    const HWND previousForeground = GetForegroundWindow();
    bool minimizedPreviousWindow = false;
    gActiveRegionState = &state;
    gActiveRegionWindow = selector;
    HHOOK mouseHook = nullptr;
    if (!preview) {
        mouseHook = SetWindowsHookExW(
            WH_MOUSE_LL, RegionMouseHookProc, GetModuleHandleW(nullptr), 0);
        Log(mouseHook ? L"Selector: hook de ratón activo, ventana original en primer plano" :
                        L"AVISO selector: no se pudo instalar el hook de ratón; se usa la entrada de ventana");
    }

    UINT showFlags = SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
    if (preview || !mouseHook) {
        const LONG_PTR exStyle = GetWindowLongPtrW(selector, GWL_EXSTYLE);
        SetWindowLongPtrW(selector, GWL_EXSTYLE, exStyle & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
        showFlags = SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_FRAMECHANGED;
    }

    if (!preview) SetLayeredWindowAttributes(selector, RGB(1, 2, 3), 255, LWA_COLORKEY);
    SetWindowPos(selector, HWND_TOPMOST, selectorBounds.left, selectorBounds.top, width, height,
                 showFlags);
    if (preview || !mouseHook) {
        bool foregroundAcquired = BringToForeground(selector);
        if (preview && !foregroundAcquired && IsWindow(previousForeground) &&
            previousForeground != selector) {
            minimizedPreviousWindow = ShowWindowAsync(previousForeground, SW_MINIMIZE) != FALSE;
            foregroundAcquired = BringToForeground(selector);
        }
        if (preview) {
            Log(foregroundAcquired
                ? (minimizedPreviousWindow
                    ? L"Selector congelado activo; se restaurará la ventana anterior"
                    : L"Selector congelado activo en primer plano")
                : L"AVISO selector: Windows no concedió el primer plano");
        }
    }
    SetTimer(selector, kRegionTopmostTimer, 100, nullptr);

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!state.done && message.message == WM_QUIT) PostQuitMessage(static_cast<int>(message.wParam));
    if (mouseHook) UnhookWindowsHookEx(mouseHook);
    gActiveRegionState = nullptr;
    gActiveRegionWindow = nullptr;
    if (IsWindow(selector)) DestroyWindow(selector);
    if (preview && IsWindow(previousForeground)) {
        if (minimizedPreviousWindow || IsIconic(previousForeground))
            ShowWindowAsync(previousForeground, SW_RESTORE);
        BringToForeground(previousForeground);
    }
    if (restoreClip) ClipCursor(&previousClip);
    // The frozen preview can take focus safely because the source frame already exists.
    if (state.accepted) {
        selection.accepted = true;
        selection.window = state.mode == CaptureMode::Window ? state.selectedWindow : nullptr;
        selection.region = NormalizedRect(state.anchor, state.current);
        if (preview && width > 0 && height > 0) {
            selection.region.left = MulDiv(selection.region.left, preview->pixelWidth, width);
            selection.region.top = MulDiv(selection.region.top, preview->pixelHeight, height);
            selection.region.right = MulDiv(selection.region.right, preview->pixelWidth, width);
            selection.region.bottom = MulDiv(selection.region.bottom, preview->pixelHeight, height);
        }
    }
    return selection;
}

struct CaptureNotificationState {
    bool clipboardCopied = false;
};

LRESULT CALLBACK CaptureNotificationProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<CaptureNotificationState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<CaptureNotificationState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_TIMER:
        if (wParam == kCaptureNotificationTimer) DestroyWindow(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = GetDpiForWindow(hwnd);

        HBRUSH background = CreateSolidBrush(RGB(28, 31, 36));
        FillRect(dc, &client, background);
        DeleteObject(background);

        const int accentWidth = MulDiv(5, dpi, 96);
        RECT accent{0, 0, accentWidth, client.bottom};
        HBRUSH accentBrush = CreateSolidBrush(RGB(54, 201, 124));
        FillRect(dc, &accent, accentBrush);

        const int iconX = MulDiv(32, dpi, 96);
        const int iconY = client.bottom / 2;
        const int iconRadius = MulDiv(14, dpi, 96);
        HGDIOBJ oldBrush = SelectObject(dc, accentBrush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, iconX - iconRadius, iconY - iconRadius,
                iconX + iconRadius, iconY + iconRadius);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(accentBrush);

        HPEN checkPen = CreatePen(PS_SOLID, std::max(2, MulDiv(2, dpi, 96)), RGB(255, 255, 255));
        oldPen = SelectObject(dc, checkPen);
        MoveToEx(dc, iconX - MulDiv(7, dpi, 96), iconY, nullptr);
        LineTo(dc, iconX - MulDiv(2, dpi, 96), iconY + MulDiv(5, dpi, 96));
        LineTo(dc, iconX + MulDiv(8, dpi, 96), iconY - MulDiv(6, dpi, 96));
        SelectObject(dc, oldPen);
        DeleteObject(checkPen);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HFONT titleFont = CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_SEMIBOLD,
                                     FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT detailFont = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL,
                                      FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const int textLeft = MulDiv(58, dpi, 96);
        RECT titleRect{textLeft, MulDiv(13, dpi, 96), client.right - MulDiv(12, dpi, 96), client.bottom};
        HGDIOBJ oldFont = SelectObject(dc, titleFont);
        DrawTextW(dc, L"Captura creada", -1, &titleRect, DT_SINGLELINE | DT_NOPREFIX);

        SetTextColor(dc, RGB(197, 203, 211));
        RECT detailRect{textLeft, MulDiv(39, dpi, 96), client.right - MulDiv(12, dpi, 96), client.bottom};
        SelectObject(dc, detailFont);
        const wchar_t* detail = state && state->clipboardCopied
            ? L"Guardada y copiada al portapapeles"
            : L"Guardada; portapapeles no disponible";
        DrawTextW(dc, detail, -1, &detailRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        DeleteObject(titleFont);
        DeleteObject(detailFont);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(hwnd, kCaptureNotificationTimer);
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ShowCaptureNotification(HMONITOR monitor, bool clipboardCopied) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = CaptureNotificationProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NativeHDRShot.CaptureNotification";
        classRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!classRegistered) return;

    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitor, &info)) return;
    }

    auto* state = new CaptureNotificationState{clipboardCopied};
    HWND notification = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"NativeHDRShot.CaptureNotification", L"Captura creada", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), state);
    if (!notification) {
        delete state;
        return;
    }

    const UINT dpi = GetDpiForWindow(notification);
    const int width = MulDiv(330, dpi, 96);
    const int height = MulDiv(72, dpi, 96);
    const int margin = MulDiv(18, dpi, 96);
    const int x = info.rcWork.right - width - margin;
    const int y = info.rcWork.top + margin;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                     MulDiv(14, dpi, 96), MulDiv(14, dpi, 96));
    if (!SetWindowRgn(notification, region, FALSE)) DeleteObject(region);
    SetLayeredWindowAttributes(notification, 0, 242, LWA_ALPHA);
    SetWindowPos(notification, HWND_TOPMOST, x, y, width, height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetTimer(notification, kCaptureNotificationTimer, 2200, nullptr);
}

class App {
public:
    ~App() { if (captureWorker_.joinable()) captureWorker_.join(); }
    int Run(HINSTANCE instance) {
        instance_ = instance;
        appIcon_ = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        if (!appIcon_) appIcon_ = LoadIconW(nullptr, IDI_APPLICATION);
        warningIcon_ = LoadIconW(nullptr, IDI_WARNING);
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

        fallbackRegistered_ = RegisterHotKey(hwnd_, kHotkeyFallback,
                                             MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F11) != FALSE;
        RecoverPrintScreenControl(false);
        Log(L"Inicio. Hook PrintScreen=" + std::wstring(HasPrintScreenControl() ? L"activo" : L"inactivo") +
            L", reserva RegisterHotKey=" + std::wstring(printScreenRegistered_ ? L"activa" : L"ocupada") +
            L", Ctrl+Shift+F11=" + std::wstring(fallbackRegistered_ ? L"activo" : L"ocupado") +
            L", integridad=" + std::wstring(IsProcessElevated() ? L"elevada" : L"estándar"));
        if (!HasPrintScreenControl()) {
            Notify(L"Impr Pant sin protección", printScreenRegistered_
                ? L"La captura sigue disponible, pero Windows podría adelantarse. Usa «Recuperar Impr Pant» en Configuración."
                : (fallbackRegistered_
                    ? L"Usa Ctrl+Shift+F11 y pulsa «Recuperar Impr Pant» en Configuración."
                    : L"Los atajos están ocupados; pulsa «Recuperar Impr Pant» en Configuración."), NIIF_WARNING);
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (capture_editor::Editor::Translate(message)) continue;
            if (settingsWindow_ && IsDialogMessageW(settingsWindow_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RemoveTrayIcon();
        return static_cast<int>(message.wParam);
    }

private:
    static CaptureSettings ReadQualityControls(HWND window) {
        CaptureSettings settings;
        settings.format = SendMessageW(GetDlgItem(window, kSettingsFormat), CB_GETCURSEL, 0, 0) == 1
            ? OutputFormat::Jpeg : OutputFormat::Png;
        settings.jpegQuality = static_cast<int>(SendMessageW(GetDlgItem(window, kSettingsQuality), TBM_GETPOS, 0, 0));
        settings.jpegFullChroma = SendMessageW(GetDlgItem(window, kSettingsChroma), CB_GETCURSEL, 0, 0) == 0;
        const auto sizeIndex = SendMessageW(GetDlgItem(window, kSettingsSize), CB_GETCURSEL, 0, 0);
        if (sizeIndex >= 0 && sizeIndex < static_cast<LRESULT>(std::size(kSizeLimits)))
            settings.maxDimension = kSizeLimits[sizeIndex];
        return settings;
    }

    static void SetQualityControls(HWND window, const CaptureSettings& settings) {
        SendMessageW(GetDlgItem(window, kSettingsFormat), CB_SETCURSEL, settings.format == OutputFormat::Png ? 0 : 1, 0);
        SendMessageW(GetDlgItem(window, kSettingsQuality), TBM_SETPOS, TRUE, settings.jpegQuality);
        SendMessageW(GetDlgItem(window, kSettingsChroma), CB_SETCURSEL, settings.jpegFullChroma ? 0 : 1, 0);
        const auto position = std::find(std::begin(kSizeLimits), std::end(kSizeLimits), settings.maxDimension);
        SendMessageW(GetDlgItem(window, kSettingsSize), CB_SETCURSEL, position - std::begin(kSizeLimits), 0);
    }

    static void UpdateSettingsControls(HWND window) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return;
        const HWND format = GetDlgItem(window, kSettingsFormat);
        const HWND slider = GetDlgItem(window, kSettingsQuality);
        const HWND value = GetDlgItem(window, kSettingsQualityValue);
        const bool jpeg = SendMessageW(format, CB_GETCURSEL, 0, 0) == 1;
        EnableWindow(slider, jpeg);
        EnableWindow(GetDlgItem(window, kSettingsChroma), jpeg);
        SendMessageW(GetDlgItem(window, kSettingsPreset), CB_SETCURSEL, MatchingPreset(ReadQualityControls(window)), 0);
        const int quality = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        std::wstring label = jpeg ? std::to_wstring(quality) + L" %" : L"Sin pérdida";
        SetWindowTextW(value, label.c_str());

        const HWND status = GetDlgItem(window, kSettingsPrintScreenStatus);
        if (status) SetWindowTextW(status, self->PrintScreenStatusText().c_str());
        const HWND recover = GetDlgItem(window, kSettingsRecoverPrintScreen);
        if (recover) SetWindowTextW(recover,
            self->HasPrintScreenControl() ? L"Renovar control de Impr Pant" : L"Recuperar Impr Pant");
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
            const UINT dpi = GetDpiForWindow(window);
            SetPropW(window, L"NativeHDRShot.SettingsDpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(dpi)));
            HFONT font = CreateFontW(-MulDiv(17, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            SetPropW(window, L"NativeHDRShot.SettingsFont", font);
            auto createControl = [&](PCWSTR kind, PCWSTR text, DWORD style,
                                     int x, int y, int width, int height, UINT id) {
                HWND control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
                    MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(width, dpi, 96), MulDiv(height, dpi, 96),
                    window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                    self->instance_, nullptr);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                return control;
            };
            createControl(L"STATIC", L"Perfil", 0, 24, 24, 125, 24, 0);
            HWND preset = createControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                       165, 20, 365, 200, kSettingsPreset);
            for (PCWSTR name : {L"Ligera — compartir", L"Equilibrada — detalle", L"Máxima — PNG original", L"Personalizada"})
                SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            createControl(L"STATIC", L"Formato", 0, 24, 66, 125, 24, 0);
            HWND format = createControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                       165, 62, 365, 200, kSettingsFormat);
            for (PCWSTR name : {L"PNG — sin pérdida", L"JPEG — archivo más pequeño"})
                SendMessageW(format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            createControl(L"STATIC", L"Calidad JPEG", 0, 24, 110, 125, 24, 0);
            HWND slider = createControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                                       160, 103, 250, 42, kSettingsQuality);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(30, 100));
            SendMessageW(slider, TBM_SETTICFREQ, 10, 0);
            createControl(L"STATIC", L"", SS_CENTER, 418, 110, 112, 24, kSettingsQualityValue);
            createControl(L"STATIC", L"Color JPEG", 0, 24, 154, 125, 24, 0);
            HWND chroma = createControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                       165, 150, 365, 200, kSettingsChroma);
            for (PCWSTR name : {L"Máximo detalle de color (4:4:4)", L"Menor tamaño (4:2:0)"})
                SendMessageW(chroma, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            createControl(L"STATIC", L"Lado máximo", 0, 24, 196, 125, 24, 0);
            HWND size = createControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                     165, 192, 365, 200, kSettingsSize);
            for (PCWSTR name : {L"Original — todos los píxeles", L"1280 px", L"1920 px", L"2560 px", L"3840 px"})
                SendMessageW(size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            SetQualityControls(window, self->settings_);
            createControl(L"STATIC", L"El límite de tamaño se aplica al archivo y al pegar. No amplía recortes pequeños. JPEG al 100 % sigue teniendo pérdida.",
                          0, 24, 232, 506, 46, 0);
            createControl(L"STATIC", L"Impr Pant", 0, 24, 288, 125, 24, 0);
            createControl(L"STATIC", L"", SS_LEFT, 165, 288, 365, 24, kSettingsPrintScreenStatus);
            createControl(L"BUTTON", L"Recuperar Impr Pant", BS_PUSHBUTTON | WS_TABSTOP,
                          165, 317, 365, 32, kSettingsRecoverPrintScreen);
            createControl(L"BUTTON", L"Guardar", BS_DEFPUSHBUTTON | WS_TABSTOP,
                          348, 370, 86, 32, kSettingsSave);
            createControl(L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP,
                          444, 370, 86, 32, kSettingsCancel);
            UpdateSettingsControls(window);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kSettingsPreset && HIWORD(wParam) == CBN_SELCHANGE) {
                const int index = static_cast<int>(SendMessageW(GetDlgItem(window, kSettingsPreset), CB_GETCURSEL, 0, 0));
                if (index < 3) SetQualityControls(window, QualityPreset(index));
                UpdateSettingsControls(window);
            } else if ((LOWORD(wParam) == kSettingsFormat || LOWORD(wParam) == kSettingsSize ||
                        LOWORD(wParam) == kSettingsChroma) && HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateSettingsControls(window);
            } else if (LOWORD(wParam) == kSettingsRecoverPrintScreen) {
                self->RecoverPrintScreenControl(true);
                UpdateSettingsControls(window);
            } else if (LOWORD(wParam) == kSettingsSave) {
                CaptureSettings updated = ReadQualityControls(window);
                updated.openEditor = self->settings_.openEditor;
                updated.delaySeconds = self->settings_.delaySeconds;
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
        case WM_DPICHANGED: {
            const UINT oldDpi = static_cast<UINT>(reinterpret_cast<UINT_PTR>(GetPropW(window, L"NativeHDRShot.SettingsDpi")));
            const UINT dpi = HIWORD(wParam);
            HFONT font = CreateFontW(-MulDiv(17, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                RECT bounds{};
                GetWindowRect(child, &bounds);
                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
                MoveWindow(child, MulDiv(bounds.left, dpi, oldDpi), MulDiv(bounds.top, dpi, oldDpi),
                           MulDiv(bounds.right - bounds.left, dpi, oldDpi),
                           MulDiv(bounds.bottom - bounds.top, dpi, oldDpi), TRUE);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
            DeleteObject(reinterpret_cast<HFONT>(GetPropW(window, L"NativeHDRShot.SettingsFont")));
            SetPropW(window, L"NativeHDRShot.SettingsFont", font);
            SetPropW(window, L"NativeHDRShot.SettingsDpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(dpi)));
            const auto* bounds = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY: {
            Log(L"Ventana de configuración cerrada");
            self->settingsWindow_ = nullptr;
            RemovePropW(window, L"NativeHDRShot.SettingsDpi");
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
        case kHookCaptureRequested:
            StartCapture();
            return 0;
        case WM_HOTKEY:
            if (static_cast<int>(wParam) == kHotkeyPrintScreen && keyboardHook_) {
                UnhookWindowsHookEx(keyboardHook_);
                keyboardHook_ = nullptr;
                printScreenKeyDown_ = false;
                Log(L"AVISO: el hook de Impr Pant dejó de interceptar; se mantiene la reserva del atajo");
                UpdateTrayStatus();
                Notify(L"Impr Pant sin protección",
                       L"La captura funciona mediante reserva, pero Windows podría adelantarse. Usa «Recuperar Impr Pant».",
                       NIIF_WARNING);
                UpdateSettingsControls(settingsWindow_);
            }
            StartCapture();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kMenuCapture || LOWORD(wParam) == kMenuWindow ||
                LOWORD(wParam) == kMenuMonitor || LOWORD(wParam) == kMenuDesktop) {
                if (!captureActive_) {
                    captureMode_ = LOWORD(wParam) == kMenuWindow ? CaptureMode::Window :
                        LOWORD(wParam) == kMenuMonitor ? CaptureMode::Monitor :
                        LOWORD(wParam) == kMenuDesktop ? CaptureMode::Desktop : CaptureMode::Region;
                }
                StartCapture();
            } else if (LOWORD(wParam) >= kMenuDelayBase && LOWORD(wParam) <= kMenuDelayBase + 10) {
                settings_.delaySeconds = LOWORD(wParam) - kMenuDelayBase;
                SaveSettings(settings_);
            } else if (LOWORD(wParam) == kMenuAutoEditor) {
                settings_.openEditor = !settings_.openEditor;
                SaveSettings(settings_);
            } else if (LOWORD(wParam) == kMenuEditor) OpenLastEditor();
            else if (LOWORD(wParam) == kMenuCancelDelay && delayPending_) {
                KillTimer(hwnd_, kCaptureDelayTimer);
                delayPending_ = false;
                captureActive_ = false;
            }
            else if (LOWORD(wParam) == kMenuOpenFolder) {
                const auto path = OutputRoot();
                std::filesystem::create_directories(path);
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (LOWORD(wParam) == kMenuSettings) {
                ShowSettings();
            } else if (LOWORD(wParam) == kMenuRecoverPrintScreen) {
                RecoverPrintScreenControl(true);
            } else if (LOWORD(wParam) == kMenuExit) {
                if (captureActive_ && !delayPending_) {
                    Notify(L"Captura en curso", L"Termina o cancela la captura antes de salir.", NIIF_INFO);
                    return 0;
                }
                if (editorWindow_ && IsWindow(editorWindow_)) {
                    SendMessageW(editorWindow_, WM_CLOSE, 0, 0);
                    if (IsWindow(editorWindow_)) return 0;
                }
                DestroyWindow(hwnd_);
            }
            return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) StartCapture();
            else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case WM_TIMER:
            if (wParam == kCaptureDelayTimer) {
                KillTimer(hwnd_, kCaptureDelayTimer);
                delayPending_ = false;
                StartCaptureNow();
            }
            return 0;
        case kPreviewReady: {
            if (captureWorker_.joinable()) captureWorker_.join();
            std::unique_ptr<CaptureResult> prepared(reinterpret_cast<CaptureResult*>(lParam));
            if (!prepared->ok) {
                captureActive_ = false;
                Log(L"ERROR preparando selector: " + prepared->error);
                Notify(L"No se pudo capturar", prepared->error, NIIF_ERROR);
                return 0;
            }

            Log(L"Fotograma congelado listo; abriendo selector");
            RegionSelection selection;
            if (captureMode_ == CaptureMode::Desktop) {
                selection.accepted = true;
                selection.region = {0, 0, static_cast<LONG>(prepared->pixelWidth), static_cast<LONG>(prepared->pixelHeight)};
            } else selection = SelectRegion(previewMonitor_, prepared.get(), captureMode_);
            if (!selection.accepted) {
                captureActive_ = false;
                Log(L"Selección cancelada");
                return 0;
            }

            Log(L"Región seleccionada; guardando fotograma congelado");
            const CaptureSettings settings = activeSettings_;
            captureWorker_ = std::thread([this, selection, settings, prepared = std::move(prepared)]() mutable {
                init_apartment(apartment_type::multi_threaded);
                auto result = std::make_unique<CaptureResult>(
                    selection.window
                        ? CaptureMonitor(MonitorFromWindow(selection.window, MONITOR_DEFAULTTONEAREST), settings,
                                         nullptr, hwnd_, false, selection.window)
                        : SavePreparedCrop(*prepared, selection.region, settings, hwnd_));
                if (PostMessageW(hwnd_, kCaptureDone, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
            });
            return 0;
        }
        case kCaptureDone: {
            if (captureWorker_.joinable()) captureWorker_.join();
            std::unique_ptr<CaptureResult> result(reinterpret_cast<CaptureResult*>(lParam));
            captureActive_ = false;
            if (result->ok) {
                std::wostringstream messageText;
                messageText << L"Guardado: " << result->sdrPath.filename().wstring()
                            << L" (pico SDR " << std::fixed << std::setprecision(2) << result->peakLinear
                            << L", blanco SDR x" << result->sdrWhiteMultiplier << L")"
                            << (result->clipboardCopied ? L" · copiada al portapapeles" : L" · sin portapapeles");
                Log(messageText.str());
                if (!result->clipboardError.empty()) Log(L"AVISO portapapeles: " + result->clipboardError);
                Notify(result->clipboardCopied ? L"Captura completada" : L"Captura guardada",
                       messageText.str(), result->clipboardCopied ? NIIF_INFO : NIIF_WARNING);
                ShowCaptureNotification(previewMonitor_, result->clipboardCopied);
                lastCapture_ = std::move(result);
                lastCaptureSettings_ = activeSettings_;
                if (activeSettings_.openEditor) OpenLastEditor();
            } else {
                Log(L"ERROR: " + result->error);
                Notify(L"No se pudo capturar", result->error, NIIF_ERROR);
            }
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kCaptureDelayTimer);
            ReleasePrintScreenControl();
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
        activeSettings_ = settings_;
        if (activeSettings_.delaySeconds) {
            delayPending_ = true;
            if (!SetTimer(hwnd_, kCaptureDelayTimer, activeSettings_.delaySeconds * 1000, nullptr)) {
                delayPending_ = false;
                captureActive_ = false;
                Notify(L"Error de retardo", L"No se pudo programar la captura.", NIIF_ERROR);
                return;
            }
            Notify(L"Captura programada", L"Se capturará en " + std::to_wstring(activeSettings_.delaySeconds) +
                   L" segundos. Puedes cancelarla desde el icono de bandeja.", NIIF_INFO);
            return;
        }
        StartCaptureNow();
    }

    void OpenLastEditor() {
        if (!lastCapture_) return;
        if (editorWindow_ && IsWindow(editorWindow_)) {
            SendMessageW(editorWindow_, WM_CLOSE, 0, 0);
            if (IsWindow(editorWindow_)) return;
        }
        capture_editor::Image image{lastCapture_->pixelWidth, lastCapture_->pixelHeight,
                                   lastCapture_->pixelStride, lastCapture_->sdrPixels};
        editorWindow_ = capture_editor::Editor::Open(std::move(image), lastCaptureSettings_, lastCapture_->sdrPath);
    }

    void StartCaptureNow() {
        bool usedFullscreenWindow = false;
        previewMonitor_ = CaptureTargetMonitor(usedFullscreenWindow);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        GetMonitorInfoW(previewMonitor_, &monitorInfo);
        std::wostringstream targetMessage;
        targetMessage << L"Preparando selector multimonitor; referencia "
                      << monitorInfo.rcMonitor.left << L',' << monitorInfo.rcMonitor.top << L' '
                      << monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left << L'x'
                      << monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top
                      << (usedFullscreenWindow ? L" (ventana a pantalla completa)" : L" (cursor)");
        Log(targetMessage.str());
        const CaptureSettings settings = activeSettings_;
        const HMONITOR monitor = previewMonitor_;
        if (captureWorker_.joinable()) captureWorker_.join();
        captureWorker_ = std::thread([this, monitor, settings] {
            init_apartment(apartment_type::multi_threaded);
            auto result = std::make_unique<CaptureResult>(
                CaptureVirtualDesktop(settings, monitor));
            if (PostMessageW(hwnd_, kPreviewReady, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        });
    }

    void AddTrayIcon() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = hwnd_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = HasPrintScreenControl() ? appIcon_ : warningIcon_;
        wcsncpy_s(tray_.szTip, TrayTooltip().c_str(), _TRUNCATE);
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

    bool HasPrintScreenControl() const noexcept {
        return keyboardHook_ != nullptr;
    }

    std::wstring PrintScreenStatusText() const {
        if (HasPrintScreenControl()) return L"Protegido por hook nativo";
        if (printScreenRegistered_) return L"Reserva activa; sin protección";
        return L"No disponible";
    }

    std::wstring TrayTooltip() const {
        if (HasPrintScreenControl()) return L"NativeHDRShot — Impr Pant protegido";
        if (printScreenRegistered_) return L"NativeHDRShot — Impr Pant sin protección";
        return L"NativeHDRShot — Impr Pant no disponible";
    }

    void UpdateTrayStatus() {
        if (!hwnd_) return;
        tray_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.hIcon = HasPrintScreenControl() ? appIcon_ : warningIcon_;
        wcsncpy_s(tray_.szTip, TrayTooltip().c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void ReleasePrintScreenControl() noexcept {
        if (keyboardHook_) {
            UnhookWindowsHookEx(keyboardHook_);
            keyboardHook_ = nullptr;
        }
        if (hookOwner_ == this) hookOwner_ = nullptr;
        printScreenKeyDown_ = false;
    }

    bool RecoverPrintScreenControl(bool showResult) {
        ReleasePrintScreenControl();
        if (printScreenRegistered_) {
            UnregisterHotKey(hwnd_, kHotkeyPrintScreen);
            printScreenRegistered_ = false;
        }

        hookOwner_ = this;
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, instance_, 0);
        if (!keyboardHook_) {
            hookOwner_ = nullptr;
            Log(L"ERROR instalando hook de Impr Pant: " + HResultText(HRESULT_FROM_WIN32(GetLastError())));
        } else {
            Log(L"Hook nativo de Impr Pant instalado");
        }

        // This reservation keeps capture working if Windows silently removes the hook.
        printScreenRegistered_ = RegisterHotKey(hwnd_, kHotkeyPrintScreen, MOD_NOREPEAT, VK_SNAPSHOT) != FALSE;
        UpdateTrayStatus();
        UpdateSettingsControls(settingsWindow_);

        if (showResult) {
            if (HasPrintScreenControl()) {
                Notify(L"Impr Pant recuperado", L"NativeHDRShot vuelve a interceptar y proteger la tecla.", NIIF_INFO);
            } else {
                Notify(L"No se pudo recuperar Impr Pant",
                       printScreenRegistered_
                           ? L"La reserva funciona, pero Windows todavía podría adelantarse."
                           : L"Otra aplicación mantiene ocupado el atajo. Cierra el otro capturador e inténtalo de nuevo.",
                       NIIF_WARNING);
            }
        }
        return HasPrintScreenControl();
    }

    static bool HasPrintScreenModifiers() noexcept {
        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    }

    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
        App* self = hookOwner_;
        if (code == HC_ACTION && self) {
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if (gActiveRegionWindow && IsWindow(gActiveRegionWindow) &&
                (event->vkCode == VK_ESCAPE || event->vkCode == VK_RETURN)) {
                if (keyDown) PostMessageW(gActiveRegionWindow, WM_KEYDOWN, event->vkCode, 0);
                return 1;
            }
            if (event->vkCode == VK_SNAPSHOT) {
                if (keyDown && !HasPrintScreenModifiers()) {
                    if (!self->printScreenKeyDown_.exchange(true)) {
                        PostMessageW(self->hwnd_, kHookCaptureRequested, 0, 0);
                    }
                    return 1;
                }
                if (keyUp && self->printScreenKeyDown_.exchange(false)) return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    void ShowTrayMenu() {
        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuCapture, L"Capturar región");
        AppendMenuW(menu, MF_STRING, kMenuWindow, L"Capturar ventana");
        AppendMenuW(menu, MF_STRING, kMenuMonitor, L"Capturar pantalla");
        AppendMenuW(menu, MF_STRING, kMenuDesktop, L"Capturar escritorio completo");
        HMENU delays = CreatePopupMenu();
        for (UINT seconds : {0u, 3u, 5u, 10u}) {
            const std::wstring label = seconds ? std::to_wstring(seconds) + L" segundos" : L"Sin retardo";
            AppendMenuW(delays, MF_STRING | (settings_.delaySeconds == seconds ? MF_CHECKED : 0),
                        kMenuDelayBase + seconds, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(delays), L"Retardo");
        if (delayPending_) AppendMenuW(menu, MF_STRING, kMenuCancelDelay, L"Cancelar captura programada");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (settings_.openEditor ? MF_CHECKED : 0), kMenuAutoEditor, L"Abrir editor después de capturar");
        AppendMenuW(menu, MF_STRING | (lastCapture_ ? 0 : MF_GRAYED), kMenuEditor, L"Editar última captura…");
        AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Abrir carpeta de capturas");
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"Configuración…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, PrintScreenStatusText().c_str());
        if (!HasPrintScreenControl()) {
            AppendMenuW(menu, MF_STRING, kMenuRecoverPrintScreen, L"Recuperar Impr Pant");
        }
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

        RECT desired{0, 0, 555, 425};
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
            const UINT dpi = GetDpiForWindow(settingsWindow_);
            RECT scaled{0, 0, MulDiv(555, dpi, 96), MulDiv(425, dpi, 96)};
            AdjustWindowRectExForDpi(&scaled, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                                    WS_EX_DLGMODALFRAME, dpi);
            const int scaledWidth = scaled.right - scaled.left;
            const int scaledHeight = scaled.bottom - scaled.top;
            SetWindowPos(settingsWindow_, nullptr,
                monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - scaledWidth) / 2,
                monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - scaledHeight) / 2,
                scaledWidth, scaledHeight, SWP_NOZORDER | SWP_NOACTIVATE);
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
    HICON warningIcon_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_ = 0;
    HMONITOR previewMonitor_ = nullptr;
    bool printScreenRegistered_ = false;
    bool fallbackRegistered_ = false;
    CaptureSettings settings_ = LoadSettings();
    CaptureSettings activeSettings_ = settings_;
    CaptureSettings lastCaptureSettings_ = settings_;
    CaptureMode captureMode_ = CaptureMode::Region;
    bool delayPending_ = false;
    std::thread captureWorker_;
    std::unique_ptr<CaptureResult> lastCapture_;
    HWND editorWindow_ = nullptr;
    std::atomic<bool> captureActive_{false};
    std::atomic<bool> printScreenKeyDown_{false};
    inline static App* hookOwner_ = nullptr;
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
            HWND clipboardOwner = CreateWindowExW(
                WS_EX_TOOLWINDOW, L"STATIC", kAppName, WS_POPUP,
                0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
            const auto result = CaptureMonitor(MonitorUnderCursor(), LoadSettings(), nullptr, clipboardOwner);
            if (clipboardOwner) DestroyWindow(clipboardOwner);
            if (result.ok) {
                Log(L"Captura puntual guardada: " + result.sdrPath.wstring() +
                    (result.clipboardCopied ? L" · copiada al portapapeles" : L" · sin portapapeles"));
                if (!result.clipboardError.empty()) Log(L"AVISO portapapeles: " + result.clipboardError);
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
