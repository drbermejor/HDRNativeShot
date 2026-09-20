#include "../src/capture_editor.h"
#include <iostream>
#include <fstream>
#include <future>
#include <numeric>

using namespace image_output;
using capture_editor::Image;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Image TestImage(UINT width, UINT height) {
    Image image{width, height, width * 4, {}};
    image.pixels.resize(static_cast<size_t>(image.stride) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            auto* p = image.pixels.data() + static_cast<size_t>(y) * image.stride + x * 4;
            p[0] = static_cast<BYTE>((x * 255) / width);
            p[1] = static_cast<BYTE>((y * 255) / height);
            p[2] = static_cast<BYTE>((x / 7 + y / 11) % 2 ? 230 : 30);
            p[3] = 255;
        }
    }
    return image;
}

Image Decode(IStream* stream) {
    LARGE_INTEGER start{};
    check_hresult(stream->Seek(start, STREAM_SEEK_SET, nullptr));
    auto factory = ImagingFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    check_hresult(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    check_hresult(decoder->GetFrame(0, &frame));
    ComPtr<IWICFormatConverter> convert;
    check_hresult(factory->CreateFormatConverter(&convert));
    check_hresult(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                      nullptr, 0, WICBitmapPaletteTypeCustom));
    Image image;
    check_hresult(frame->GetSize(&image.width, &image.height));
    image.stride = image.width * 4;
    image.pixels.resize(static_cast<size_t>(image.stride) * image.height);
    check_hresult(convert->CopyPixels(nullptr, image.stride, static_cast<UINT>(image.pixels.size()), image.pixels.data()));
    return image;
}

void HalfTests() {
    for (UINT bits = 0; bits <= 65535; ++bits) {
        const int exponent = (bits >> 10) & 31;
        const int mantissa = bits & 1023;
        const float actual = HalfToFloat(static_cast<uint16_t>(bits));
        if (exponent == 31) Require(mantissa ? std::isnan(actual) : std::isinf(actual), "half nonfinite");
        else {
            float expected = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15)
                                      : std::ldexp(static_cast<float>(mantissa), -24);
            if (bits & 32768) expected = -expected;
            Require(std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(expected), "half exhaustive conversion");
        }
    }
    Require(LinearToSrgbByte(0) == 0 && LinearToSrgbByte(1) == 255, "sRGB endpoints");
    Require(LinearToSrgbByte(0.5f) == 188, "sRGB transfer");
    Require(LinearToSrgbByte(NAN) == 0 && LinearToSrgbByte(INFINITY) == 255, "nonfinite input");
}

void DibTests() {
    for (UINT width : {1u, 2u, 3u, 7u, 32u}) {
        auto image = TestImage(width, 5);
        // Include a source row pitch larger than the packed width.
        const UINT paddedStride = image.stride + 12;
        std::vector<BYTE> padded(paddedStride * image.height, 99);
        for (UINT y = 0; y < image.height; ++y)
            std::memcpy(padded.data() + y * paddedStride, image.pixels.data() + y * image.stride, image.stride);
        for (bool v5 : {false, true}) {
            const auto dib = BuildDib(width, image.height, paddedStride, padded.data(), v5);
            const size_t header = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
            const UINT stride = v5 ? width * 4 : (width * 3 + 3) & ~3u;
            for (UINT y = 0; y < image.height; ++y)
                for (UINT x = 0; x < width; ++x) {
                    const BYTE* pixel = dib.data() + header + (v5 ? y : image.height - 1 - y) * stride + x * (v5 ? 4 : 3);
                    Require(std::memcmp(pixel, image.pixels.data() + y * image.stride + x * 4, v5 ? 4 : 3) == 0,
                            "DIB pixels, orientation, alpha or row pitch");
                }
            if (!v5) for (UINT y = 0; y < image.height; ++y)
                for (UINT x = width * 3; x < stride; ++x) Require(dib[header + y * stride + x] == 0, "DIB padding");
        }
    }
    bool rejected = false;
    try { BuildDib(100, 2, 20, nullptr, false); } catch (...) { rejected = true; }
    Require(rejected, "invalid image rejected");
}

void EncodingTests(const std::filesystem::path& directory) {
    auto source = TestImage(3840, 2160);
    uint32_t random = 0x12345678;
    for (size_t i = 0; i < source.pixels.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            random = random * 1664525u + 1013904223u;
            source.pixels[i + c] = static_cast<BYTE>(std::clamp(static_cast<int>(source.pixels[i + c]) +
                static_cast<int>((random >> 24) & 31) - 16, 0, 255));
        }
    }
    uintmax_t sizes[3]{};
    for (int index = 0; index < 3; ++index) {
        auto settings = QualityPreset(index);
        auto image = source;
        ResizeSdr(image.width, image.height, image.stride, image.pixels, settings.maxDimension);
        Require(index != 0 || (image.width == 1920 && image.height == 1080), "light dimensions");
        const auto path = directory / (std::to_wstring(index) + (index == 2 ? L".png" : L".jpg"));
        SaveSdrFile(path, settings, image.width, image.height, image.stride, image.pixels);
        sizes[index] = std::filesystem::file_size(path);
        auto factory = ImagingFactory();
        ComPtr<IWICStream> stream;
        check_hresult(factory->CreateStream(&stream));
        check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_READ));
        const auto decoded = Decode(stream.Get());
        Require(decoded.width == image.width && decoded.height == image.height, "encoded dimensions");
        if (index == 2) Require(decoded.pixels == image.pixels, "PNG lossless roundtrip");
        else {
            std::ifstream file(path, std::ios::binary);
            const std::vector<BYTE> bytes((std::istreambuf_iterator<char>(file)), {});
            bool found = false;
            for (size_t i = 0; i + 11 < bytes.size(); ++i) {
                if (bytes[i] == 0xff && bytes[i + 1] == 0xc0) {
                    Require(bytes[i + 11] == (index == 0 ? 0x22 : 0x11), "JPEG actual 420/444 sampling");
                    found = true;
                    break;
                }
            }
            Require(found, "JPEG SOF marker");
        }
    }
    Require(sizes[0] < sizes[1], "light preset must reduce size versus balanced");
    std::cout << "Synthetic 4K image sizes: light=" << sizes[0] << ", balanced=" << sizes[1]
              << ", PNG=" << sizes[2] << " bytes\n";
    auto smallImage = TestImage(101, 79);
    const auto original = smallImage;
    ResizeSdr(smallImage.width, smallImage.height, smallImage.stride, smallImage.pixels, 1920);
    Require(smallImage.pixels == original.pixels && smallImage.width == 101, "no upscale");
    auto portrait = TestImage(99, 300);
    ResizeSdr(portrait.width, portrait.height, portrait.stride, portrait.pixels, 100);
    Require(portrait.width == 33 && portrait.height == 100, "portrait aspect ratio");
    auto memory = EncodeClipboardPng(smallImage.width, smallImage.height, smallImage.stride, smallImage.pixels.data());
    ComPtr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(memory.get(), FALSE, &stream));
    Require(Decode(stream.Get()).pixels == smallImage.pixels, "clipboard PNG roundtrip");
}

void EditorTests() {
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    Require(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "GDI+ startup");
    const auto source = TestImage(100, 100);
    capture_editor::History history;
    history.current = source;
    capture_editor::Stroke stroke;
    stroke.tool = capture_editor::Tool::Redact;
    stroke.points = {{10, 10}, {80, 80}};
    history.Commit(capture_editor::ApplyStroke(history.current, stroke));
    const auto* pixel = history.current.pixels.data() + 50 * history.current.stride + 50 * 4;
    Require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255, "redaction opaque pixels");
    Require(history.Undo() && history.current.pixels == source.pixels, "undo restore original");
    Require(history.Redo() && history.current.pixels != source.pixels, "redo restore edit");
    history.Commit(capture_editor::Crop(history.current, {20, 30, 60, 90}));
    Require(history.current.width == 40 && history.current.height == 60, "crop dimensions");
    Require(history.Undo() && history.current.width == 100, "undo crop");
    history.Commit(source);
    Require(!history.Redo(), "new edit discards redo");
    for (auto tool : {capture_editor::Tool::Pen, capture_editor::Tool::Highlight, capture_editor::Tool::Arrow,
                     capture_editor::Tool::Rectangle, capture_editor::Tool::Text}) {
        stroke.tool = tool;
        stroke.text = L"Test";
        const auto annotated = capture_editor::ApplyStroke(source, stroke);
        Require(annotated.pixels != source.pixels, "annotation must change exported pixels");
        for (size_t i = 3; i < annotated.pixels.size(); i += 4)
            Require(annotated.pixels[i] == 255, "annotations preserve opaque alpha");
    }
    Gdiplus::GdiplusShutdown(token);
}

void ClipboardTests() {
    // Windows isolates each station's clipboard. Never touch the interactive station.
    HWINSTA station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    if (!station) check_hresult(HRESULT_FROM_WIN32(GetLastError()));
    if (!SetProcessWindowStation(station)) check_hresult(HRESULT_FROM_WIN32(GetLastError()));
    HDESK desktop = CreateDesktopW(L"Test", nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS |
                                   DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP, nullptr);
    Require(desktop && SetThreadDesktop(desktop), "private desktop");
    HWND window = CreateWindowExW(0, L"STATIC", L"Test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Require(window != nullptr, "clipboard owner");
    const auto image = TestImage(7, 5);
    std::wstring error;
    auto copy = [&] { return CopySdrToClipboard(window, image.width, image.height, image.stride, image.pixels.data(), error); };
    Require(copy() && error.empty(), "publish all clipboard formats");
    Require(OpenClipboard(window) != FALSE, "open clipboard for read");
    const UINT png = RegisterClipboardFormatW(L"PNG");
    Require(EnumClipboardFormats(0) == png, "PNG preferred enumeration order");
    Require(GetClipboardData(CF_DIBV5) && GetClipboardData(CF_DIB) && GetClipboardData(CF_BITMAP), "native and synthesized formats");
    ComPtr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(GetClipboardData(png), FALSE, &stream));
    Require(Decode(stream.Get()).pixels == image.pixels, "clipboard PNG decodes correctly");
    stream.Reset();
    CloseClipboard();
    std::promise<void> locked;
    auto ready = locked.get_future();
    std::thread contender([&] {
        if (!SetThreadDesktop(desktop) || !OpenClipboard(nullptr)) { locked.set_exception(std::make_exception_ptr(std::runtime_error("clipboard contender"))); return; }
        locked.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CloseClipboard();
    });
    try { ready.get(); Require(copy(), "retry clipboard contention beyond old 250ms limit"); }
    catch (...) { contender.join(); throw; }
    contender.join();
    // Inject an optional encoder failure; publication still uses the real Windows clipboard.
    std::wstring fallbackError;
    const bool fallbackCopied = CopySdrToClipboard(window, image.width, image.height, image.stride,
        image.pixels.data(), fallbackError, [](UINT, UINT, UINT, const BYTE*) -> GlobalMemory {
            throw winrt::hresult_error(E_FAIL, L"Test encoder failure");
        });
    Require(fallbackCopied && !fallbackError.empty() && IsClipboardFormatAvailable(CF_DIB) &&
            !IsClipboardFormatAvailable(png), "PNG failure preserves native fallback");
    const DWORD sequence = GetClipboardSequenceNumber();
    Require(!CopySdrToClipboard(nullptr, 7, 5, 28, image.pixels.data(), error), "invalid owner rejected");
    Require(sequence == GetClipboardSequenceNumber(), "validation preserves previous clipboard");
    Require(copy(), "restore full payload");
    DestroyWindow(window);
    Require(IsClipboardFormatAvailable(png) != FALSE, "clipboard survives owner destruction");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    HWND editor = capture_editor::Editor::Open(TestImage(1000, 620), {}, L"test.png");
    Require(editor != nullptr, "editor window creation");
    Require(SendMessageW(GetDlgItem(editor, 314), CB_GETCOUNT, 0, 0) == 7 &&
            SendMessageW(GetDlgItem(editor, 314), CB_GETCURSEL, 0, 0) == 0, "editor tools populated");
    Require(SendMessageW(GetDlgItem(editor, 316), CB_GETCURSEL, 0, 0) == 1 &&
            GetDlgItem(editor, 317), "editor thickness and text controls");
    RECT client{};
    GetClientRect(editor, &client);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Require(bitmap && pixels, "editor rendering surface");
    auto previous = SelectObject(dc, bitmap);
    SendMessageW(editor, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    struct RenderContext { HWND window; HDC dc; } context{editor, dc};
    EnumChildWindows(editor, [](HWND child, LPARAM data) -> BOOL {
        const auto& target = *reinterpret_cast<RenderContext*>(data);
        if (GetParent(child) != target.window) return TRUE;
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, target.window, reinterpret_cast<POINT*>(&bounds), 2);
        const int saved = SaveDC(target.dc);
        SetViewportOrgEx(target.dc, bounds.left, bounds.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(target.dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(target.dc, saved);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    GdiFlush();
    std::vector<BYTE> rendered(static_cast<BYTE*>(pixels), static_cast<BYTE*>(pixels) + client.right * client.bottom * 4);
    for (size_t i = 3; i < rendered.size(); i += 4) rendered[i] = 255;
    const auto renderPath = std::filesystem::temp_directory_path() / L"NativeHDRShot-editor-test.png";
    SaveSdrFile(renderPath, {}, client.right, client.bottom, client.right * 4, rendered);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyWindow(editor);
    std::wcout << L"Editor test render: " << renderPath.c_str() << L'\n';
    // Process exit releases the private station and desktop handles.
}

int wmain(int argc, wchar_t** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (argc > 1 && std::wstring(argv[1]) == L"--clipboard") {
            ClipboardTests();
            std::cout << "PASS: isolated clipboard publication, decoding, fallback, contention and lifetime\n";
            return 0;
        }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (argc > 1 && std::wstring(argv[1]) == L"--editor") {
            INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&common);
            HWND window = capture_editor::Editor::Open(TestImage(1000, 620), {}, std::filesystem::current_path() / L"editor-test.png");
            MSG message{};
            while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (capture_editor::Editor::Translate(message)) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return 0;
        }
        const auto directory = std::filesystem::temp_directory_path() / (L"NativeHDRShot-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        HalfTests();
        DibTests();
        EncodingTests(directory);
        EditorTests();
        std::cout << "PASS: 65536 half values, DIB layout, encoders, presets, resize, editor and history\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"FAIL HRESULT " << std::hex << static_cast<unsigned>(error.code()) << L": " << error.message().c_str() << L'\n';
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; }
    return 1;
}
