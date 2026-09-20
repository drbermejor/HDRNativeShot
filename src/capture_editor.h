#pragma once

#include "image_output.h"
#include <commctrl.h>
#include <windowsx.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <deque>
#include <functional>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace capture_editor {
using namespace image_output;

struct Image {
    UINT width = 0, height = 0, stride = 0;
    std::vector<BYTE> pixels;
};

inline Image Crop(const Image& source, RECT region) {
    region.left = std::clamp(region.left, 0L, static_cast<LONG>(source.width));
    region.top = std::clamp(region.top, 0L, static_cast<LONG>(source.height));
    region.right = std::clamp(region.right, region.left, static_cast<LONG>(source.width));
    region.bottom = std::clamp(region.bottom, region.top, static_cast<LONG>(source.height));
    Image result;
    result.width = region.right - region.left;
    result.height = region.bottom - region.top;
    result.stride = result.width * 4;
    result.pixels.resize(CheckedImageBytes(result.width, result.height, result.stride, 4));
    for (UINT y = 0; y < result.height; ++y)
        std::memcpy(result.pixels.data() + static_cast<size_t>(y) * result.stride,
                    source.pixels.data() + static_cast<size_t>(y + region.top) * source.stride + region.left * 4,
                    result.stride);
    return result;
}

class History {
public:
    Image current;
    void Commit(Image next) {
        undo_.push_back(std::move(current));
        current = std::move(next);
        redo_.clear();
        size_t bytes = 0;
        for (const auto& item : undo_) bytes += item.pixels.size();
        while (undo_.size() > 1 && (bytes > 256ull * 1024 * 1024 || undo_.size() > 20)) {
            bytes -= undo_.front().pixels.size();
            undo_.pop_front();
        }
    }
    bool Undo() { return Move(undo_, redo_); }
    bool Redo() { return Move(redo_, undo_); }
private:
    bool Move(std::deque<Image>& from, std::deque<Image>& to) {
        if (from.empty()) return false;
        to.push_back(std::move(current));
        current = std::move(from.back());
        from.pop_back();
        return true;
    }
    std::deque<Image> undo_, redo_;
};

enum class Tool { Pen, Highlight, Arrow, Rectangle, Text, Redact, Crop };

struct Stroke {
    Tool tool = Tool::Pen;
    std::vector<Gdiplus::PointF> points;
    Gdiplus::Color color{255, 235, 55, 65};
    float size = 4.0f;
    std::wstring text;
};

inline void DrawStroke(Gdiplus::Graphics& graphics, const Stroke& stroke) {
    if (stroke.points.empty()) return;
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const auto first = stroke.points.front(), last = stroke.points.back();
    const Gdiplus::RectF bounds(std::min(first.X, last.X), std::min(first.Y, last.Y),
                              std::abs(last.X - first.X), std::abs(last.Y - first.Y));
    Gdiplus::Pen pen(stroke.color, stroke.size);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (stroke.tool == Tool::Pen) {
        if (stroke.points.size() == 1) {
            Gdiplus::SolidBrush brush(stroke.color);
            graphics.FillEllipse(&brush, first.X - stroke.size / 2, first.Y - stroke.size / 2, stroke.size, stroke.size);
        } else graphics.DrawLines(&pen, stroke.points.data(), static_cast<INT>(stroke.points.size()));
    } else if (stroke.tool == Tool::Highlight) {
        Gdiplus::SolidBrush brush(Gdiplus::Color(85, 255, 220, 0));
        graphics.FillRectangle(&brush, bounds);
    } else if (stroke.tool == Tool::Redact) {
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 0, 0));
        // An opaque fill, never blur: the exported pixels contain no hidden original.
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        graphics.FillRectangle(&brush, bounds);
    } else if (stroke.tool == Tool::Rectangle) graphics.DrawRectangle(&pen, bounds);
    else if (stroke.tool == Tool::Arrow) {
        Gdiplus::AdjustableArrowCap arrow(4.0f, 5.0f, TRUE);
        pen.SetCustomEndCap(&arrow);
        graphics.DrawLine(&pen, first, last);
    } else if (stroke.tool == Tool::Text) {
        Gdiplus::Font font(L"Segoe UI", std::max(14.0f, stroke.size * 5), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush brush(stroke.color);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.DrawString(stroke.text.c_str(), -1, &font, first, &brush);
    } else if (stroke.tool == Tool::Crop) {
        pen.SetDashStyle(Gdiplus::DashStyleDash);
        graphics.DrawRectangle(&pen, bounds);
    }
}

inline Image ApplyStroke(const Image& source, const Stroke& stroke) {
    if (stroke.points.empty()) return source;
    if (stroke.tool == Tool::Crop) {
        const auto first = stroke.points.front(), last = stroke.points.back();
        return Crop(source, {static_cast<LONG>(std::floor(std::min(first.X, last.X))),
                            static_cast<LONG>(std::floor(std::min(first.Y, last.Y))),
                            static_cast<LONG>(std::ceil(std::max(first.X, last.X))),
                            static_cast<LONG>(std::ceil(std::max(first.Y, last.Y)))});
    }
    Image result = source;
    {
        Gdiplus::Bitmap bitmap(result.width, result.height, result.stride, PixelFormat32bppARGB, result.pixels.data());
        Gdiplus::Graphics graphics(&bitmap);
        DrawStroke(graphics, stroke);
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    return result;
}

class Editor {
public:
    static HWND Open(Image image, CaptureSettings settings, const std::filesystem::path& path) {
        static ULONG_PTR token = [] {
            ULONG_PTR value = 0;
            Gdiplus::GdiplusStartupInput input;
            if (Gdiplus::GdiplusStartup(&value, &input, nullptr) != Gdiplus::Ok)
                throw winrt::hresult_error(E_FAIL, L"No se pudo iniciar el editor");
            return value;
        }();
        (void)token;
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"NativeHDRShot.Editor";
        RegisterClassW(&wc);
        auto editor = std::make_unique<Editor>();
        editor->history_.current = std::move(image);
        editor->settings_ = settings;
        editor->path_ = path;
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = std::min(1120L, monitor.rcWork.right - monitor.rcWork.left);
        const int height = std::min(800L, monitor.rcWork.bottom - monitor.rcWork.top);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"NativeHDRShot — Editor",
                                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, monitor.rcWork.left + 30, monitor.rcWork.top + 30,
                                      width, height, nullptr, nullptr, wc.hInstance, editor.get());
        if (window) {
            editor->selfOwned_ = true;
            editor.release();
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
        }
        return window;
    }

    static bool Translate(MSG& message) {
        if (message.message != WM_KEYDOWN) return false;
        HWND root = GetAncestor(message.hwnd, GA_ROOT);
        wchar_t className[64]{};
        GetClassNameW(root, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"NativeHDRShot.Editor") != 0) return false;
        if (message.wParam == VK_TAB) return IsDialogMessageW(root, &message) != FALSE;
        if (!(GetKeyState(VK_CONTROL) & 0x8000)) return false;
        // In the text field, normal text selection/copy/undo must keep working.
        if (GetDlgCtrlID(message.hwnd) == Text && message.wParam != 'S') return false;
        UINT command = 0;
        if (message.wParam == 'C') command = Copy;
        if (message.wParam == 'S') command = Save;
        if (message.wParam == 'Z') command = Undo;
        if (message.wParam == 'Y') command = Redo;
        if (!command) return false;
        SendMessageW(root, WM_COMMAND, command, 0);
        return true;
    }

private:
    enum : UINT { Copy = 310, Save, Undo, Redo, Tools, Color, Thickness, Text, Status };
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    History history_;
    CaptureSettings settings_;
    std::filesystem::path path_;
    Stroke stroke_;
    bool drawing_ = false, dirty_ = false, selfOwned_ = false;
    COLORREF color_ = RGB(235, 55, 65);
    float scale_ = 1;
    Gdiplus::PointF origin_{};
    int toolbarHeight_ = 94;
    UINT dpi_ = 96;

    int Px(int value) const { return MulDiv(value, dpi_, 96); }

    void Layout() {
        RECT client{};
        GetClientRect(window_, &client);
        toolbarHeight_ = Px(94);
        const auto& image = history_.current;
        scale_ = std::max(0.001f, std::min(static_cast<float>(std::max(1L, client.right - Px(32))) / image.width,
            static_cast<float>(std::max(1L, client.bottom - toolbarHeight_ - Px(42))) / image.height));
        scale_ = std::min(scale_, 1.0f);
        origin_ = {(client.right - image.width * scale_) / 2,
                   toolbarHeight_ + (client.bottom - toolbarHeight_ - Px(28) - image.height * scale_) / 2};
        MoveWindow(GetDlgItem(window_, Status), Px(12), client.bottom - Px(26), client.right - Px(24), Px(24), TRUE);
    }

    void StatusText(const std::wstring& text = L"") {
        std::wstring message = text.empty()
            ? std::to_wstring(history_.current.width) + L" × " + std::to_wstring(history_.current.height) +
              L" px · " + std::to_wstring(static_cast<int>(scale_ * 100)) + L" % · " +
              (dirty_ ? L"Cambios sin guardar" : L"Captura guardada")
            : text;
        SetWindowTextW(GetDlgItem(window_, Status), message.c_str());
    }

    void CreateControls() {
        dpi_ = GetDpiForWindow(window_);
        font_ = CreateFontW(-Px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto control = [&](PCWSTR kind, PCWSTR text, DWORD style, int x, int y, int width, int height, UINT id) {
            HWND child = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
                Px(x), Px(y), Px(width), Px(height), window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            return child;
        };
        control(L"BUTTON", L"Copiar  Ctrl+C", WS_TABSTOP, 12, 10, 130, 30, Copy);
        control(L"BUTTON", L"Guardar como…", WS_TABSTOP, 150, 10, 140, 30, Save);
        control(L"BUTTON", L"Deshacer", WS_TABSTOP, 298, 10, 98, 30, Undo);
        control(L"BUTTON", L"Rehacer", WS_TABSTOP, 404, 10, 98, 30, Redo);
        HWND tools = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 12, 52, 150, 260, Tools);
        for (PCWSTR name : {L"Lápiz", L"Resaltador", L"Flecha", L"Rectángulo", L"Texto", L"Ocultar (negro)", L"Recortar"})
            SendMessageW(tools, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(tools, CB_SETCURSEL, 0, 0);
        control(L"BUTTON", L"Color…", WS_TABSTOP, 170, 50, 84, 30, Color);
        HWND thickness = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 262, 52, 82, 220, Thickness);
        for (PCWSTR name : {L"2 px", L"4 px", L"8 px", L"16 px", L"32 px"})
            SendMessageW(thickness, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(thickness, CB_SETCURSEL, 1, 0);
        control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 356, 52, 270, 27, Text);
        SendMessageW(GetDlgItem(window_, Text), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Escribe el texto aquí"));
        SendMessageW(GetDlgItem(window_, Text), EM_SETLIMITTEXT, 500, 0);
        control(L"STATIC", L"", SS_LEFT, 12, 0, 600, 24, Status);
        Layout();
        StatusText();
    }

    Gdiplus::PointF ImagePoint(LPARAM position) const {
        return {std::clamp((GET_X_LPARAM(position) - origin_.X) / scale_, 0.0f, static_cast<float>(history_.current.width)),
                std::clamp((GET_Y_LPARAM(position) - origin_.Y) / scale_, 0.0f, static_cast<float>(history_.current.height))};
    }

    void FinishStroke() {
        if (!drawing_) return;
        drawing_ = false;
        ReleaseCapture();
        if (stroke_.tool == Tool::Crop &&
            (std::abs(stroke_.points.front().X - stroke_.points.back().X) < 2 ||
             std::abs(stroke_.points.front().Y - stroke_.points.back().Y) < 2)) {
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        history_.Commit(ApplyStroke(history_.current, stroke_));
        dirty_ = true;
        Layout();
        StatusText();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void SaveImage() {
        wchar_t filename[32768]{};
        const auto suggested = path_.parent_path() / (path_.stem().wstring() + L" editada");
        wcsncpy_s(filename, suggested.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"PNG sin pérdida\0*.png\0JPEG\0*.jpg;*.jpeg\0\0";
        dialog.nFilterIndex = settings_.format == OutputFormat::Png ? 1 : 2;
        dialog.lpstrFile = filename;
        dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) return;
        CaptureSettings output = settings_;
        std::filesystem::path target(filename);
        if (!target.has_extension()) {
            target += dialog.nFilterIndex == 1 ? L".png" : L".jpg";
            if (std::filesystem::exists(target) &&
                MessageBoxW(window_, L"El archivo ya existe. ¿Quieres reemplazarlo?", L"Guardar imagen",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        }
        std::wstring extension = target.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (extension == L".png") output.format = OutputFormat::Png;
        else if (extension == L".jpg" || extension == L".jpeg") output.format = OutputFormat::Jpeg;
        else {
            MessageBoxW(window_, L"Usa la extensión .png, .jpg o .jpeg.", L"Formato de imagen", MB_OK | MB_ICONINFORMATION);
            return;
        }
        // Write beside the destination, then replace only after successful encoding.
        const auto temporary = target.wstring() + L".nativehdrshot-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        try {
            const auto& image = history_.current;
            SaveSdrFile(temporary, output, image.width, image.height, image.stride, image.pixels);
            if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                check_hresult(HRESULT_FROM_WIN32(GetLastError()));
        } catch (...) {
            DeleteFileW(temporary.c_str());
            throw;
        }
        path_ = target;
        dirty_ = false;
        StatusText(L"Guardado: " + path_.filename().wstring());
    }

    void Command(UINT command) {
        if (command == Copy) {
            std::wstring error;
            const auto& image = history_.current;
            if (!CopySdrToClipboard(window_, image.width, image.height, image.stride, image.pixels.data(), error))
                MessageBoxW(window_, error.c_str(), L"Portapapeles", MB_OK | MB_ICONWARNING);
            else StatusText(error.empty() ? L"Imagen editada copiada al portapapeles" : error);
        } else if (command == Save) SaveImage();
        else if (command == Undo || command == Redo) {
            if (command == Undo ? history_.Undo() : history_.Redo()) {
                dirty_ = true;
                Layout();
                StatusText();
                InvalidateRect(window_, nullptr, FALSE);
            }
        } else if (command == Color) {
            static COLORREF custom[16]{};
            CHOOSECOLORW dialog{sizeof(dialog)};
            dialog.hwndOwner = window_;
            dialog.rgbResult = color_;
            dialog.lpCustColors = custom;
            dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&dialog)) color_ = dialog.rgbResult;
        }
        SetFocus(window_);
    }

    void PaintContent(HDC dc) {
        RECT client{};
        GetClientRect(window_, &client);
        // Buffered painting keeps freehand strokes from flickering.
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP surface = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
        HGDIOBJ previous = SelectObject(buffer, surface);
        {
            Gdiplus::Graphics graphics(buffer);
            graphics.Clear(Gdiplus::Color(255, 38, 42, 48));
            Gdiplus::SolidBrush toolbar(Gdiplus::Color(255, 245, 246, 248));
            graphics.FillRectangle(&toolbar, 0, 0, client.right, toolbarHeight_);
            graphics.TranslateTransform(origin_.X, origin_.Y);
            graphics.ScaleTransform(scale_, scale_);
            auto& image = history_.current;
            Gdiplus::Bitmap bitmap(image.width, image.height, image.stride, PixelFormat32bppARGB, image.pixels.data());
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(&bitmap, Gdiplus::Rect(0, 0, image.width, image.height));
            graphics.SetClip(Gdiplus::Rect(0, 0, image.width, image.height));
            if (drawing_) DrawStroke(graphics, stroke_);
        }
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
        DeleteObject(surface);
        DeleteDC(buffer);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: CreateControls(); return 0;
        case WM_SIZE: Layout(); StatusText(); InvalidateRect(window_, nullptr, FALSE); return 0;
        case WM_GETMINMAXINFO: {
            auto* size = reinterpret_cast<MINMAXINFO*>(lParam);
            size->ptMinTrackSize = {Px(660), Px(400)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const auto* bounds = reinterpret_cast<RECT*>(lParam);
            const LRESULT tool = SendMessageW(GetDlgItem(window_, Tools), CB_GETCURSEL, 0, 0);
            const LRESULT thickness = SendMessageW(GetDlgItem(window_, Thickness), CB_GETCURSEL, 0, 0);
            wchar_t pendingText[501]{};
            GetWindowTextW(GetDlgItem(window_, Text), pendingText, static_cast<int>(std::size(pendingText)));
            // Recreate controls so fonts and hit targets match the new monitor DPI.
            for (UINT id = Copy; id <= Status; ++id) DestroyWindow(GetDlgItem(window_, id));
            DeleteObject(font_);
            CreateControls();
            SendMessageW(GetDlgItem(window_, Tools), CB_SETCURSEL, tool, 0);
            SendMessageW(GetDlgItem(window_, Thickness), CB_SETCURSEL, thickness, 0);
            SetWindowTextW(GetDlgItem(window_, Text), pendingText);
            SetWindowPos(window_, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) >= Copy && LOWORD(wParam) <= Color)
                Command(LOWORD(wParam));
            return 0;
        case WM_KEYDOWN:
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (wParam == 'C') Command(Copy);
                if (wParam == 'S') Command(Save);
                if (wParam == 'Z') Command(Undo);
                if (wParam == 'Y') Command(Redo);
            } else if (wParam == VK_ESCAPE && drawing_) {
                drawing_ = false;
                ReleaseCapture();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)), y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (x < origin_.X || y < origin_.Y || x >= origin_.X + history_.current.width * scale_ ||
                y >= origin_.Y + history_.current.height * scale_) return 0;
            SetFocus(window_);
            stroke_ = {};
            stroke_.tool = static_cast<Tool>(SendMessageW(GetDlgItem(window_, Tools), CB_GETCURSEL, 0, 0));
            stroke_.color = Gdiplus::Color(255, GetRValue(color_), GetGValue(color_), GetBValue(color_));
            const int thickness = static_cast<int>(SendMessageW(GetDlgItem(window_, Thickness), CB_GETCURSEL, 0, 0));
            stroke_.size = static_cast<float>(2 << std::clamp(thickness, 0, 4));
            stroke_.points.push_back(ImagePoint(lParam));
            drawing_ = true;
            if (stroke_.tool == Tool::Text) {
                wchar_t text[501]{};
                GetWindowTextW(GetDlgItem(window_, Text), text, static_cast<int>(std::size(text)));
                stroke_.text = text;
                if (stroke_.text.empty()) {
                    drawing_ = false;
                    StatusText(L"Escribe el texto en el campo superior y después pulsa sobre la imagen");
                    return 0;
                }
                FinishStroke();
            } else SetCapture(window_);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (drawing_) {
                if (stroke_.tool != Tool::Pen && stroke_.points.size() > 1) stroke_.points.pop_back();
                stroke_.points.push_back(ImagePoint(lParam));
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (drawing_) {
                stroke_.points.push_back(ImagePoint(lParam));
                FinishStroke();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (drawing_) { drawing_ = false; InvalidateRect(window_, nullptr, FALSE); }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT:
            PaintContent(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window_, &paint);
            PaintContent(dc);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_CLOSE:
            if (dirty_) {
                const int choice = MessageBoxW(window_, L"Hay cambios sin guardar. ¿Quieres guardarlos antes de cerrar?",
                                                L"NativeHDRShot", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (choice == IDCANCEL) return 0;
                if (choice == IDYES) { SaveImage(); if (dirty_) return 0; }
            }
            DestroyWindow(window_);
            return 0;
        case WM_NCDESTROY:
            DeleteObject(font_);
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            if (selfOwned_) delete this;
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            editor = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            editor->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
        }
        if (!editor) return DefWindowProcW(window, message, wParam, lParam);
        try { return editor->Handle(message, wParam, lParam); }
        catch (const winrt::hresult_error& error) {
            MessageBoxW(window, error.message().c_str(), L"Editor", MB_OK | MB_ICONERROR);
        } catch (...) { MessageBoxW(window, L"No se pudo completar la edición.", L"Editor", MB_OK | MB_ICONERROR); }
        return 0;
    }
};
} // namespace capture_editor
