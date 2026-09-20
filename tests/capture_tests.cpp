// Compile the real application pipeline into a separate, non-elevated test host.
// wWinMain is not invoked, so no hotkeys, resident instance or settings are changed.
#include "../src/main.cpp"
#include <iostream>

void RequireCapture(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CaptureSmokeTests() {
    const auto monitor = MonitorUnderCursor();
    CaptureSettings settings;
    auto desktop = CaptureVirtualDesktop(settings, monitor);
    if (!desktop.ok) throw winrt::hresult_error(E_FAIL, desktop.error);
    RequireCapture(desktop.pixelWidth && desktop.pixelHeight && !desktop.sdrPixels.empty(), "virtual desktop pixels");
    RequireCapture(desktop.pixelStride == desktop.pixelWidth * 4, "virtual desktop row pitch");
    std::cout << "Desktop capture: " << desktop.pixelWidth << 'x' << desktop.pixelHeight << '\n';

    HWND window = CreateWindowExW(0, L"STATIC", L"NativeHDRShot capture test", WS_OVERLAPPEDWINDOW,
                                  50, 50, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RequireCapture(window != nullptr, "test window");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    const auto captured = CaptureMonitor(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), settings,
                                         nullptr, nullptr, true, window);
    DestroyWindow(window);
    if (!captured.ok) throw winrt::hresult_error(E_FAIL, captured.error);
    RequireCapture(captured.pixelWidth > 0 && captured.pixelWidth <= 320 &&
                   captured.pixelHeight > 0 && captured.pixelHeight <= 240, "WGC window dimensions");
    std::cout << "Window capture: " << captured.pixelWidth << 'x' << captured.pixelHeight << '\n';
    // Pixel data remains in memory only. No personal screenshots or clipboard writes.
}

void SelectorTests() {
    HWINSTA station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    RequireCapture(station && SetProcessWindowStation(station), "private test station");
    HDESK desktop = CreateDesktopW(L"CaptureTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    RequireCapture(desktop && SetThreadDesktop(desktop), "private test desktop");
    WNDCLASSW wc{};
    wc.lpfnWndProc = RegionSelectorProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NativeHDRShot.TestSelector";
    RequireCapture(RegisterClassW(&wc) != 0, "selector test class");
    auto makeWindow = [&](RegionSelectorState& state) {
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"Selector test", WS_POPUP,
                                      0, 0, 800, 600, nullptr, nullptr, wc.hInstance, &state);
        RequireCapture(window != nullptr, "selector fixture");
        return window;
    };
    RegionSelectorState region;
    HWND window = makeWindow(region);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 100));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(30, 25));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(30, 25));
    const auto bounds = NormalizedRect(region.anchor, region.current);
    RequireCapture(region.accepted && bounds.left == 30 && bounds.top == 25 &&
                   bounds.right == 120 && bounds.bottom == 100, "reverse region selection");

    RegionSelectorState click;
    window = makeWindow(click);
    SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(30, 25));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(30, 25));
    RequireCapture(!click.accepted && IsWindow(window), "small click stays open");
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    RequireCapture(click.done && !click.accepted && !IsWindow(window), "escape cancels");

    RegionSelectorState full;
    window = makeWindow(full);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    RequireCapture(full.accepted && full.current.x == 800 && full.current.y == 600, "desktop Enter selection");

    RegionSelectorState target;
    target.windows.push_back({reinterpret_cast<HWND>(1), {50, 40, 400, 300}});
    window = makeWindow(target);
    SendMessageW(window, WM_COMMAND, 501, 0);
    SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(80, 70));
    RequireCapture(target.accepted && target.selectedWindow == reinterpret_cast<HWND>(1) &&
                   target.anchor.x == 50 && target.current.x == 400, "window hit testing and mode switch");

    RegionSelectorState outside;
    outside.windows = target.windows;
    window = makeWindow(outside);
    SendMessageW(window, WM_COMMAND, 501, 0);
    SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(600, 500));
    RequireCapture(!outside.accepted && IsWindow(window), "window mode ignores empty desktop");
    DestroyWindow(window);
    std::cout << "PASS: selector modes, hit testing, reverse drag, accidental clicks and cancel\n";
}

int wmain(int argc, wchar_t** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (argc > 1 && std::wstring(argv[1]) == L"--capture") {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            CaptureSmokeTests();
            std::cout << "PASS: real WGC desktop and window capture\n";
        } else SelectorTests();
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::wcerr << L"FAIL HRESULT " << std::hex << static_cast<unsigned>(error.code()) << L": " << error.message().c_str() << L'\n';
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; }
    return 1;
}
