#include "DockMenuWindow.h"

#include <Dwmapi.h>
#include <Windowsx.h>

#include <algorithm>
#include <cwchar>
#include <initializer_list>
#include <utility>

namespace MyDock {
namespace {

constexpr wchar_t kMenuClassName[] = L"MyDockContextMenuWindow";
constexpr int kMenuWidth = 232;
constexpr int kPadding = 6;
constexpr int kHeaderHeight = 30;
constexpr int kRowHeight = 34;
constexpr int kSeparatorHeight = 9;
constexpr int kHoverRadius = 8;
constexpr int kGlyphColumn = 34;
constexpr int kTextInset = 12;

// DWM attributes by value so older Windows SDK headers still compile. Unsupported attributes
// (Windows 10) fail harmlessly and the menu falls back to square corners.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr int kDwmCornerRound = 2;

constexpr COLORREF kBackgroundColor = RGB(30, 35, 43);
constexpr COLORREF kHoverColor = RGB(58, 66, 78);
constexpr COLORREF kTextColor = RGB(245, 247, 251);
constexpr COLORREF kMutedColor = RGB(160, 169, 182);
constexpr COLORREF kSeparatorColor = RGB(64, 72, 84);
constexpr COLORREF kBorderColor = RGB(88, 97, 110);

HFONT CreateUiFont(int height, int weight, const wchar_t* face) {
    return CreateFontW(
        -height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, face);
}

// GDI silently substitutes missing fonts, which would render icon code points as boxes.
HFONT CreateGlyphFontIfInstalled(const wchar_t* face) {
    HFONT font = CreateUiFont(16, FW_NORMAL, face);
    if (!font) {
        return nullptr;
    }

    wchar_t actual[LF_FACESIZE] = {};
    HDC dc = GetDC(nullptr);
    if (dc) {
        HGDIOBJ previous = SelectObject(dc, font);
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, previous);
        ReleaseDC(nullptr, dc);
    }

    if (_wcsicmp(actual, face) != 0) {
        DeleteObject(font);
        return nullptr;
    }

    return font;
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ previousBrush = SelectObject(dc, brush);
    HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, previousPen);
    SelectObject(dc, previousBrush);
    DeleteObject(brush);
}

void DrawHorizontalLine(HDC dc, int left, int right, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ previous = SelectObject(dc, pen);
    MoveToEx(dc, left, y, nullptr);
    LineTo(dc, right, y);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

} // namespace

DockMenuWindow::DockMenuWindow(HINSTANCE instance)
    : instance_(instance) {}

DockMenuWindow::~DockMenuWindow() {
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    DestroyFonts();
}

bool DockMenuWindow::Create(HWND owner, UINT commandMessage) {
    owner_ = owner;
    commandMessage_ = commandMessage;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.hInstance = instance_;
    wc.lpfnWndProc = DockMenuWindow::WindowProc;
    wc.lpszClassName = kMenuClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Owned by the dock: an owned window always stays above its owner in z-order.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kMenuClassName,
        L"MyDock menu",
        WS_POPUP,
        0,
        0,
        kMenuWidth,
        kRowHeight,
        owner_,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        return false;
    }

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    const int corners = kDwmCornerRound;
    DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corners, sizeof(corners));
    const COLORREF border = kBorderColor;
    DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &border, sizeof(border));

    CreateFonts();
    return true;
}

void DockMenuWindow::Show(
    const std::wstring& title,
    std::vector<DockMenuItem> items,
    LONG anchorLeft,
    LONG anchorRight,
    LONG bottom) {
    if (!hwnd_ || items.empty()) {
        return;
    }

    title_ = title;
    items_ = std::move(items);
    hoverIndex_ = -1;

    const int width = kMenuWidth;
    const int height = ContentHeight();
    const LONG centerX = anchorLeft + (anchorRight - anchorLeft) / 2;

    RECT bounds = {};
    const POINT probe = {centerX, bottom};
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        bounds = info.rcMonitor;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &bounds, 0);
    }

    int x = static_cast<int>(centerX) - width / 2;
    x = std::max(static_cast<int>(bounds.left) + 8, std::min(x, static_cast<int>(bounds.right) - width - 8));
    int y = static_cast<int>(bottom) - height;
    y = std::max(static_cast<int>(bounds.top) + 8, y);

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DockMenuWindow::Hide() {
    hoverIndex_ = -1;
    trackingMouse_ = false;
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

bool DockMenuWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

bool DockMenuWindow::WasJustDismissed() const {
    constexpr ULONGLONG kDismissClickWindowMs = 300;
    return dismissedAt_ != 0 && GetTickCount64() - dismissedAt_ < kDismissClickWindowMs;
}

LRESULT CALLBACK DockMenuWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<DockMenuWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DockMenuWindow*>(create->lpCreateParams);
        if (!self) {
            return FALSE;
        }

        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) {
            self->hwnd_ = nullptr;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT DockMenuWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetHover(HitTest(point));
        if (!trackingMouse_) {
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd_;
            trackingMouse_ = TrackMouseEvent(&track) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        SetHover(-1);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        Invoke(HitTest(point));
        return 0;
    }
    case WM_ACTIVATE:
        // Clicking anywhere else (including the dock) deactivates the menu: dismiss like a native menu.
        if (LOWORD(wParam) == WA_INACTIVE && IsVisible()) {
            dismissedAt_ = GetTickCount64();
            Hide();
        }
        return 0;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            Hide();
            return 0;
        case VK_UP:
            MoveHover(-1);
            return 0;
        case VK_DOWN:
            MoveHover(1);
            return 0;
        case VK_RETURN:
        case VK_SPACE:
            Invoke(hoverIndex_);
            return 0;
        default:
            break;
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void DockMenuWindow::CreateFonts() {
    DestroyFonts();
    labelFont_ = CreateUiFont(14, FW_NORMAL, L"Segoe UI");
    titleFont_ = CreateUiFont(12, FW_SEMIBOLD, L"Segoe UI");
    glyphFont_ = CreateGlyphFontIfInstalled(L"Segoe Fluent Icons");
    if (!glyphFont_) {
        glyphFont_ = CreateGlyphFontIfInstalled(L"Segoe MDL2 Assets");
    }
}

void DockMenuWindow::DestroyFonts() {
    for (HFONT* font : {&labelFont_, &titleFont_, &glyphFont_}) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void DockMenuWindow::Paint() {
    PAINTSTRUCT paint = {};
    HDC windowDc = BeginPaint(hwnd_, &paint);

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    HDC dc = CreateCompatibleDC(windowDc);
    HBITMAP buffer = CreateCompatibleBitmap(windowDc, width, height);
    if (!dc || !buffer) {
        if (buffer) {
            DeleteObject(buffer);
        }
        if (dc) {
            DeleteDC(dc);
        }
        EndPaint(hwnd_, &paint);
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(dc, buffer);
    HGDIOBJ previousFont = SelectObject(dc, labelFont_ ? labelFont_ : GetStockObject(DEFAULT_GUI_FONT));

    HBRUSH background = CreateSolidBrush(kBackgroundColor);
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    const int rowLeft = kPadding;
    const int rowRight = width - kPadding;

    if (!title_.empty()) {
        if (titleFont_) {
            SelectObject(dc, titleFont_);
        }
        SetTextColor(dc, kMutedColor);
        RECT titleRect = {rowLeft + kTextInset, kPadding, rowRight - kTextInset, kPadding + kHeaderHeight};
        DrawTextW(dc, title_.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawHorizontalLine(dc, rowLeft + 8, rowRight - 8, kPadding + kHeaderHeight + kSeparatorHeight / 2, kSeparatorColor);
    }

    for (size_t index = 0; index < items_.size(); ++index) {
        const auto& item = items_[index];
        const int top = ItemTop(index);

        if (item.separator) {
            DrawHorizontalLine(dc, rowLeft + 8, rowRight - 8, top + kSeparatorHeight / 2, kSeparatorColor);
            continue;
        }

        const RECT row = {rowLeft, top, rowRight, top + kRowHeight};
        if (static_cast<int>(index) == hoverIndex_) {
            FillRoundedRect(dc, row, kHoverRadius, kHoverColor);
        }

        int textLeft = row.left + kTextInset;
        if (glyphFont_) {
            if (item.glyph != 0) {
                SelectObject(dc, glyphFont_);
                SetTextColor(dc, kMutedColor);
                RECT glyphRect = {row.left + 4, row.top, row.left + kGlyphColumn, row.bottom};
                const wchar_t glyph[2] = {item.glyph, L'\0'};
                DrawTextW(dc, glyph, 1, &glyphRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            textLeft = row.left + kGlyphColumn + 6;
        }

        if (labelFont_) {
            SelectObject(dc, labelFont_);
        }
        SetTextColor(dc, kTextColor);
        RECT labelRect = {textLeft, row.top, row.right - kTextInset, row.bottom};
        DrawTextW(dc, item.label.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    BitBlt(windowDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);

    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(buffer);
    DeleteDC(dc);
    EndPaint(hwnd_, &paint);
}

int DockMenuWindow::HeaderHeight() const {
    return title_.empty() ? 0 : kHeaderHeight + kSeparatorHeight;
}

int DockMenuWindow::ItemHeight(size_t index) const {
    return items_[index].separator ? kSeparatorHeight : kRowHeight;
}

int DockMenuWindow::ItemTop(size_t index) const {
    int top = kPadding + HeaderHeight();
    for (size_t previous = 0; previous < index && previous < items_.size(); ++previous) {
        top += ItemHeight(previous);
    }
    return top;
}

int DockMenuWindow::ContentHeight() const {
    return ItemTop(items_.size()) + kPadding;
}

int DockMenuWindow::HitTest(POINT point) const {
    if (point.x < kPadding || point.x >= kMenuWidth - kPadding) {
        return -1;
    }

    for (size_t index = 0; index < items_.size(); ++index) {
        if (items_[index].separator) {
            continue;
        }

        const int top = ItemTop(index);
        if (point.y >= top && point.y < top + kRowHeight) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

void DockMenuWindow::SetHover(int index) {
    if (index != hoverIndex_) {
        hoverIndex_ = index;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DockMenuWindow::MoveHover(int direction) {
    const int count = static_cast<int>(items_.size());
    if (count == 0) {
        return;
    }

    int index = hoverIndex_;
    for (int step = 0; step < count; ++step) {
        if (index < 0) {
            index = direction > 0 ? 0 : count - 1;
        } else {
            index = (index + direction + count) % count;
        }

        if (!items_[static_cast<size_t>(index)].separator) {
            SetHover(index);
            return;
        }
    }
}

void DockMenuWindow::Invoke(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }

    const auto& item = items_[static_cast<size_t>(index)];
    if (item.separator || item.command == 0) {
        return;
    }

    const UINT command = item.command;
    Hide();
    if (owner_ && commandMessage_ != 0) {
        PostMessageW(owner_, commandMessage_, command, 0);
    }
}

} // namespace MyDock
