#include "SettingsWindow.h"

#include <Windowsx.h>

#include <algorithm>
#include <utility>

namespace MyDock {
namespace {

constexpr wchar_t kSettingsClassName[] = L"MyDockSettingsWindow";
constexpr int kWidth = 280;
constexpr int kHeight = 124;

HFONT CreateSettingsFont(int weight = FW_NORMAL) {
    return CreateFontW(
        -15, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

} // namespace

SettingsWindow::SettingsWindow(
    HINSTANCE instance,
    SettingsStore& settingsStore,
    std::function<void()> onSettingsChanged)
    : instance_(instance),
      settingsStore_(settingsStore),
      onSettingsChanged_(std::move(onSettingsChanged)) {}

bool SettingsWindow::Create(HWND owner) {
    owner_ = owner;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = SettingsWindow::WindowProc;
    wc.lpszClassName = kSettingsClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kSettingsClassName,
        L"MyDock settings",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWidth,
        kHeight,
        owner_,
        nullptr,
        instance_,
        this);

    return hwnd_ != nullptr;
}

void SettingsWindow::ShowNear(const RECT& anchor) {
    if (!hwnd_) {
        return;
    }

    const int x = anchor.right - kWidth;
    const int y = std::max(0, anchor.top - kHeight - 10);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kWidth, kHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

LRESULT CALLBACK SettingsWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT SettingsWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_LBUTTONUP: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RECT autoHide = AutoHideCheckRect();
        if (PtInRect(&autoHide, point)) {
            settingsStore_.Current().autoHide = !settingsStore_.Current().autoHide;
            settingsStore_.Save();
            if (onSettingsChanged_) {
                onSettingsChanged_();
            }
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return 0;
    }
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

void SettingsWindow::Paint() {
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(hwnd_, &paint);

    RECT client = {};
    GetClientRect(hwnd_, &client);

    HBRUSH background = CreateSolidBrush(RGB(32, 34, 38));
    HBRUSH checkBrush = CreateSolidBrush(settingsStore_.Current().autoHide ? RGB(65, 156, 255) : RGB(72, 76, 84));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(105, 108, 118));
    HFONT titleFont = CreateSettingsFont(FW_SEMIBOLD);
    HFONT bodyFont = CreateSettingsFont();

    FillRect(dc, &client, background);
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, titleFont);
    SetTextColor(dc, RGB(246, 247, 249));
    RECT title = {18, 14, client.right - 18, 42};
    DrawTextW(dc, L"MyDock settings", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT check = AutoHideCheckRect();
    SelectObject(dc, checkBrush);
    SelectObject(dc, borderPen);
    RoundRect(dc, check.left, check.top, check.right, check.bottom, 8, 8);

    if (settingsStore_.Current().autoHide) {
        HPEN tickPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        SelectObject(dc, tickPen);
        MoveToEx(dc, check.left + 6, check.top + 12, nullptr);
        LineTo(dc, check.left + 12, check.top + 18);
        LineTo(dc, check.right - 6, check.top + 6);
        DeleteObject(tickPen);
    }

    SelectObject(dc, bodyFont);
    SetTextColor(dc, RGB(230, 232, 236));
    RECT label = {check.right + 10, check.top - 2, client.right - 18, check.bottom + 4};
    DrawTextW(dc, L"Auto-hide dock", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DeleteObject(bodyFont);
    DeleteObject(titleFont);
    DeleteObject(borderPen);
    DeleteObject(checkBrush);
    DeleteObject(background);

    EndPaint(hwnd_, &paint);
}

RECT SettingsWindow::AutoHideCheckRect() const {
    return {20, 64, 44, 88};
}

} // namespace MyDock
