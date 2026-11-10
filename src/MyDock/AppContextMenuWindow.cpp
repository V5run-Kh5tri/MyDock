#include "AppContextMenuWindow.h"

#include <Windowsx.h>

#include <algorithm>
#include <utility>

namespace MyDock {
namespace {

constexpr wchar_t kContextMenuClassName[] = L"MyDockAppContextMenuWindow";
constexpr int kMenuWidth = 236;
constexpr int kPadding = 8;
constexpr int kRowHeight = 36;
constexpr int kSeparatorHeight = 9;
constexpr int kCornerRadius = 18;

HFONT CreateMenuFont() {
    return CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

} // namespace

AppContextMenuWindow::AppContextMenuWindow(HINSTANCE instance)
    : instance_(instance) {}

bool AppContextMenuWindow::Create(HWND owner) {
    owner_ = owner;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = AppContextMenuWindow::WindowProc;
    wc.lpszClassName = kContextMenuClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kContextMenuClassName,
        L"MyDock app menu",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kMenuWidth,
        120,
        owner_,
        nullptr,
        instance_,
        this);

    return hwnd_ != nullptr;
}

void AppContextMenuWindow::Show(const std::vector<AppContextMenuItem>& items, const RECT& anchor, std::function<void(UINT)> onCommand) {
    if (!hwnd_ || items.empty()) {
        return;
    }

    items_ = items;
    onCommand_ = std::move(onCommand);
    hoverIndex_ = -1;

    const SIZE size = Measure();
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    RECT work = {};
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }

    int x = anchor.left + ((anchor.right - anchor.left) - size.cx) / 2;
    x = std::max(work.left + 8, std::min(x, work.right - size.cx - 8));

    int y = anchor.top - size.cy - 12;
    if (y < work.top + 8) {
        y = anchor.bottom + 12;
    }
    y = std::max(work.top + 8, std::min(y, work.bottom - size.cy - 8));

    HRGN region = CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, kCornerRadius * 2, kCornerRadius * 2);
    SetWindowRgn(hwnd_, region, TRUE);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void AppContextMenuWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }

    hoverIndex_ = -1;
}

bool AppContextMenuWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

LRESULT CALLBACK AppContextMenuWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<AppContextMenuWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<AppContextMenuWindow*>(create->lpCreateParams);
        if (!self) {
            return FALSE;
        }

        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AppContextMenuWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_MOUSEMOVE: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int nextHover = HitTest(point);
        if (nextHover != hoverIndex_) {
            hoverIndex_ = nextHover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }

        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hoverIndex_ != -1) {
            hoverIndex_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int itemIndex = HitTest(point);
        if (itemIndex >= 0 && itemIndex < static_cast<int>(items_.size())) {
            const auto item = items_[static_cast<size_t>(itemIndex)];
            if (!item.separator && item.enabled && item.command != 0) {
                Hide();
                if (onCommand_) {
                    onCommand_(item.command);
                }
            }
        }
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_ACTIVATE:
        if (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) {
            break;
        }
        Hide();
        return 0;
    case WM_KILLFOCUS:
        Hide();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            Hide();
            return 0;
        }
        break;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void AppContextMenuWindow::Paint() {
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(hwnd_, &paint);

    RECT client = {};
    GetClientRect(hwnd_, &client);

    HBRUSH background = CreateSolidBrush(RGB(30, 36, 44));
    HBRUSH highlight = CreateSolidBrush(RGB(65, 82, 101));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(136, 150, 166));
    HPEN separatorPen = CreatePen(PS_SOLID, 1, RGB(83, 95, 108));
    HFONT font = CreateMenuFont();

    SelectObject(dc, background);
    SelectObject(dc, borderPen);
    RoundRect(dc, client.left, client.top, client.right, client.bottom, kCornerRadius, kCornerRadius);

    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    int y = kPadding;
    for (size_t index = 0; index < items_.size(); ++index) {
        const auto& item = items_[index];
        if (item.separator) {
            SelectObject(dc, separatorPen);
            MoveToEx(dc, kPadding + 8, y + kSeparatorHeight / 2, nullptr);
            LineTo(dc, client.right - kPadding - 8, y + kSeparatorHeight / 2);
            y += kSeparatorHeight;
            continue;
        }

        RECT row = {kPadding, y, client.right - kPadding, y + kRowHeight};
        if (static_cast<int>(index) == hoverIndex_ && item.enabled) {
            SelectObject(dc, highlight);
            SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, row.left, row.top, row.right, row.bottom, 12, 12);
        }

        SetTextColor(dc, item.enabled ? RGB(246, 248, 251) : RGB(139, 147, 157));
        RECT label = {row.left + 13, row.top, row.right - 13, row.bottom};
        DrawTextW(dc, item.label.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += kRowHeight;
    }

    DeleteObject(font);
    DeleteObject(separatorPen);
    DeleteObject(borderPen);
    DeleteObject(highlight);
    DeleteObject(background);

    EndPaint(hwnd_, &paint);
}

int AppContextMenuWindow::HitTest(POINT point) const {
    int y = kPadding;
    for (size_t index = 0; index < items_.size(); ++index) {
        const auto& item = items_[index];
        const int height = item.separator ? kSeparatorHeight : kRowHeight;
        RECT row = {kPadding, y, kMenuWidth - kPadding, y + height};
        if (!item.separator && PtInRect(&row, point)) {
            return static_cast<int>(index);
        }
        y += height;
    }

    return -1;
}

SIZE AppContextMenuWindow::Measure() const {
    int height = kPadding * 2;
    for (const auto& item : items_) {
        height += item.separator ? kSeparatorHeight : kRowHeight;
    }

    return {kMenuWidth, height};
}

} // namespace MyDock
