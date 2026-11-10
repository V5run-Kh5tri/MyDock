#include "PreviewWindow.h"

#include "ShellActions.h"

#include <Windowsx.h>

#include <algorithm>

namespace MyDock {
namespace {

constexpr wchar_t kPreviewClassName[] = L"MyDockPreviewWindow";
constexpr int kPanelWidth = 360;
constexpr int kRowHeight = 96;
constexpr int kPadding = 12;

HFONT CreateTitleFont() {
    return CreateFontW(
        -15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

} // namespace

PreviewWindow::PreviewWindow(HINSTANCE instance)
    : instance_(instance) {}

PreviewWindow::~PreviewWindow() {
    ClearThumbnails();
}

bool PreviewWindow::Create(HWND owner) {
    owner_ = owner;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = PreviewWindow::WindowProc;
    wc.lpszClassName = kPreviewClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kPreviewClassName,
        L"MyDock window preview",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kPanelWidth,
        160,
        owner_,
        nullptr,
        instance_,
        this);

    return hwnd_ != nullptr;
}

void PreviewWindow::ShowFor(const ApplicationGroup& app, const RECT& anchor) {
    if (!hwnd_) {
        return;
    }

    app_ = app;
    const int rowCount = std::max(1, static_cast<int>(app_.windows.size()));
    const int height = kPadding * 2 + rowCount * kRowHeight;
    const int x = anchor.left;
    const int y = std::max(0, static_cast<int>(anchor.top) - height - 10);

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kPanelWidth, height, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    RegisterThumbnails();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void PreviewWindow::Hide() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }

    ClearThumbnails();
}

bool PreviewWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

LRESULT CALLBACK PreviewWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<PreviewWindow*>(create->lpCreateParams);
        if (!self) {
            return FALSE;
        }

        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT PreviewWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_LBUTTONUP: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int row = HitTestRow(point);
        if (row >= 0 && row < static_cast<int>(app_.windows.size())) {
            ShellActions::ActivateWindow(app_.windows[static_cast<size_t>(row)].hwnd);
            Hide();
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

void PreviewWindow::Paint() {
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(hwnd_, &paint);

    RECT client = {};
    GetClientRect(hwnd_, &client);

    HBRUSH background = CreateSolidBrush(RGB(32, 34, 38));
    HBRUSH rowBrush = CreateSolidBrush(RGB(47, 50, 56));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(105, 108, 118));
    HFONT titleFont = CreateTitleFont();

    FillRect(dc, &client, background);

    SelectObject(dc, titleFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(246, 247, 249));

    rowRects_.clear();
    int top = kPadding;

    if (app_.windows.empty()) {
        RECT textRect = {kPadding, kPadding, client.right - kPadding, client.bottom - kPadding};
        DrawTextW(dc, L"No open windows", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    for (const auto& window : app_.windows) {
        RECT rowRect = {kPadding, top, client.right - kPadding, top + kRowHeight - 8};
        rowRects_.push_back(rowRect);

        SelectObject(dc, rowBrush);
        SelectObject(dc, borderPen);
        RoundRect(dc, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, 12, 12);

        RECT titleRect = {rowRect.left + 108, rowRect.top + 12, rowRect.right - 10, rowRect.bottom - 10};
        const std::wstring title = window.minimized ? window.title + L" (minimized)" : window.title;
        DrawTextW(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        top += kRowHeight;
    }

    DeleteObject(titleFont);
    DeleteObject(borderPen);
    DeleteObject(rowBrush);
    DeleteObject(background);

    EndPaint(hwnd_, &paint);
}

void PreviewWindow::RegisterThumbnails() {
    ClearThumbnails();

    RECT client = {};
    GetClientRect(hwnd_, &client);

    int top = kPadding;
    for (const auto& window : app_.windows) {
        ThumbnailBinding binding;
        if (SUCCEEDED(DwmRegisterThumbnail(hwnd_, window.hwnd, &binding.handle))) {
            binding.destination = {kPadding + 10, top + 10, kPadding + 96, top + 70};
            DWM_THUMBNAIL_PROPERTIES props = {};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
            props.rcDestination = binding.destination;
            props.fVisible = TRUE;
            props.opacity = 230;
            DwmUpdateThumbnailProperties(binding.handle, &props);
            thumbnails_.push_back(binding);
        }

        top += kRowHeight;
    }
}

void PreviewWindow::ClearThumbnails() {
    for (auto& thumbnail : thumbnails_) {
        if (thumbnail.handle) {
            DwmUnregisterThumbnail(thumbnail.handle);
            thumbnail.handle = nullptr;
        }
    }

    thumbnails_.clear();
}

int PreviewWindow::HitTestRow(POINT point) const {
    for (size_t index = 0; index < rowRects_.size(); ++index) {
        if (PtInRect(&rowRects_[index], point)) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

} // namespace MyDock
