#include "DockWindow.h"

#include "ShellActions.h"

#include <Dwmapi.h>
#include <Windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <sstream>

namespace MyDock {
namespace {

constexpr wchar_t kDockClassName[] = L"MyDockDockWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kClockTimer = 2;
constexpr UINT_PTR kAutoHideTimer = 3;
constexpr UINT_PTR kTooltipTimer = 4;

constexpr int kDockHeight = 92;
constexpr int kSurfaceRadius = 23;
constexpr int kOuterPadding = 12;
constexpr int kFixedSectionWidth = 188;
constexpr int kBaseIconSize = 44;
constexpr int kMaxIconSize = 70;
constexpr int kIconGap = 10;
constexpr int kVisibleBottomMargin = 12;
constexpr int kHiddenVisibleSliver = 6;

float ScaleForDistance(int distance) {
    switch (std::abs(distance)) {
    case 0:
        return 1.58f;
    case 1:
        return 1.32f;
    case 2:
        return 1.12f;
    default:
        return 1.0f;
    }
}

D2D1_RECT_F RectF(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

RECT ToRect(const D2D1_RECT_F& rect) {
    return {
        static_cast<LONG>(std::floor(rect.left)),
        static_cast<LONG>(std::floor(rect.top)),
        static_cast<LONG>(std::ceil(rect.right)),
        static_cast<LONG>(std::ceil(rect.bottom)),
    };
}

std::wstring FormatTwoDigits(int value) {
    wchar_t buffer[8] = {};
    swprintf_s(buffer, L"%02d", value);
    return buffer;
}

} // namespace

DockWindow::DockWindow(
    HINSTANCE instance,
    SettingsStore& settingsStore,
    PinnedApps& pinnedApps,
    WindowTracker& windowTracker,
    IconCache& iconCache)
    : instance_(instance),
      settingsStore_(settingsStore),
      pinnedApps_(pinnedApps),
      windowTracker_(windowTracker),
      iconCache_(iconCache),
      previewWindow_(instance),
      settingsWindow_(instance, settingsStore, [this]() {
          hidden_ = false;
          PositionWindow(false);
          if (hwnd_) {
              InvalidateRect(hwnd_, nullptr, FALSE);
          }
      }) {}

bool DockWindow::Create() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = DockWindow::WindowProc;
    wc.lpszClassName = kDockClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kDockClassName,
        L"MyDock",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        dockSize_.cx,
        dockSize_.cy,
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        return false;
    }

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &darkMode, sizeof(darkMode));

    tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        hwnd_,
        nullptr,
        instance_,
        nullptr);

    if (tooltip_) {
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 260);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    }

    if (!previewWindow_.Create(hwnd_) || !settingsWindow_.Create(hwnd_)) {
        return false;
    }

    RefreshApplications();
    UpdateLayout(false);

    SetTimer(hwnd_, kRefreshTimer, 1000, nullptr);
    SetTimer(hwnd_, kClockTimer, 1000, nullptr);
    SetTimer(hwnd_, kAutoHideTimer, 30, nullptr);
    return true;
}

void DockWindow::Show() {
    PositionWindow(false);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

LRESULT CALLBACK DockWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DockWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT DockWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (renderTarget_) {
            renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        return 0;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        UpdateLayout(true);
        return 0;
    case WM_MOUSEMOVE: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        mouseInside_ = true;
        lastInsideTime_ = std::chrono::steady_clock::now();
        UpdateHover(point);

        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        mouseInside_ = false;
        hoverIndex_ = -1;
        HideTooltip();
        KillTimer(hwnd_, kTooltipTimer);
        UpdateLayout(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        HandleLeftClick(point);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        HandleRightClick(point);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            RefreshApplications();
            UpdateLayout(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wParam == kClockTimer) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wParam == kAutoHideTimer) {
            PositionWindow(true);
            return 0;
        }
        if (wParam == kTooltipTimer) {
            MaybeShowTooltip();
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd_, kRefreshTimer);
        KillTimer(hwnd_, kClockTimer);
        KillTimer(hwnd_, kAutoHideTimer);
        KillTimer(hwnd_, kTooltipTimer);
        DiscardDeviceResources();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool DockWindow::CreateDeviceResources() {
    if (!d2dFactory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf()))) {
            return false;
        }
    }

    if (!dwriteFactory_) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())))) {
            return false;
        }
    }

    if (!renderTarget_) {
        RECT client = {};
        GetClientRect(hwnd_, &client);

        if (FAILED(d2dFactory_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                D2D1::HwndRenderTargetProperties(
                    hwnd_,
                    D2D1::SizeU(client.right - client.left, client.bottom - client.top)),
                renderTarget_.GetAddressOf()))) {
            return false;
        }

        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x20242A, 0.86f), surfaceBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.28f), borderBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xF5F7FB, 0.98f), textBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xBCC3CE, 0.88f), mutedBrush_.GetAddressOf());

        dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            15.0f,
            L"",
            timeFormat_.GetAddressOf());
        dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"",
            dateFormat_.GetAddressOf());
        dwriteFactory_->CreateTextFormat(
            L"Segoe Fluent Icons",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            20.0f,
            L"",
            iconFormat_.GetAddressOf());

        if (timeFormat_) {
            timeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            timeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (dateFormat_) {
            dateFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dateFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    return true;
}

void DockWindow::DiscardDeviceResources() {
    iconFormat_.Reset();
    dateFormat_.Reset();
    timeFormat_.Reset();
    mutedBrush_.Reset();
    textBrush_.Reset();
    borderBrush_.Reset();
    surfaceBrush_.Reset();
    renderTarget_.Reset();
}

void DockWindow::RefreshApplications() {
    MergeApplications(windowTracker_.Snapshot());
}

void DockWindow::MergeApplications(const std::vector<ApplicationGroup>& running) {
    std::map<std::wstring, ApplicationGroup> runningById;
    for (const auto& app : running) {
        runningById[ToLower(app.id)] = app;
    }

    std::vector<ApplicationGroup> merged;
    std::map<std::wstring, bool> represented;

    for (const auto& pinned : pinnedApps_.Apps()) {
        ApplicationGroup app;
        app.id = ToLower(pinned.id.empty() ? IdentityFromExecutable(pinned.launchCommand) : pinned.id);
        app.displayName = pinned.displayName;
        app.launchCommand = pinned.launchCommand;
        app.iconPath = pinned.iconPath.empty() ? pinned.launchCommand : pinned.iconPath;
        app.pinned = true;

        const auto runningMatch = runningById.find(app.id);
        if (runningMatch != runningById.end()) {
            app.windows = runningMatch->second.windows;
            if (app.iconPath.empty()) {
                app.iconPath = runningMatch->second.iconPath;
            }
        }

        represented[app.id] = true;
        merged.push_back(std::move(app));
    }

    for (const auto& runningApp : running) {
        const auto id = ToLower(runningApp.id);
        if (represented.find(id) == represented.end()) {
            ApplicationGroup app = runningApp;
            app.pinned = false;
            merged.push_back(std::move(app));
        }
    }

    applications_ = std::move(merged);
}

void DockWindow::UpdateLayout(bool reposition) {
    iconLayouts_.clear();
    iconLayouts_.reserve(applications_.size());

    float appWidth = static_cast<float>(kOuterPadding);
    std::vector<float> sizes;
    sizes.reserve(applications_.size());

    for (size_t index = 0; index < applications_.size(); ++index) {
        const float scale = hoverIndex_ >= 0 ? ScaleForDistance(static_cast<int>(index) - hoverIndex_) : 1.0f;
        const float size = kBaseIconSize * scale;
        sizes.push_back(size);
        appWidth += size;
        if (index + 1 < applications_.size()) {
            appWidth += kIconGap + (size - kBaseIconSize) * 0.22f;
        }
    }

    if (applications_.empty()) {
        appWidth += kBaseIconSize;
    }

    const int targetWidth = static_cast<int>(std::ceil(kFixedSectionWidth + appWidth + kOuterPadding));
    dockSize_ = {std::max(360, targetWidth), kDockHeight};

    float x = static_cast<float>(kFixedSectionWidth + kOuterPadding);
    const float centerY = 46.0f;
    for (size_t index = 0; index < applications_.size(); ++index) {
        const float size = sizes[index];
        IconLayout layout;
        layout.scale = size / kBaseIconSize;
        layout.bounds = RectF(x, centerY - size / 2.0f, x + size, centerY + size / 2.0f);
        iconLayouts_.push_back(layout);
        x += size + kIconGap + (size - kBaseIconSize) * 0.22f;
    }

    quickSettingsRect_ = {102, 28, 132, 58};
    settingsRect_ = {142, 28, 172, 58};
    dividerRect_ = {kFixedSectionWidth - 1, 18, kFixedSectionWidth, kDockHeight - 18};

    HRGN region = CreateRoundRectRgn(0, 0, dockSize_.cx + 1, dockSize_.cy + 1, kSurfaceRadius * 2, kSurfaceRadius * 2);
    SetWindowRgn(hwnd_, region, TRUE);

    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        0,
        0,
        dockSize_.cx,
        dockSize_.cy,
        SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    if (renderTarget_) {
        renderTarget_->Resize(D2D1::SizeU(dockSize_.cx, dockSize_.cy));
    }

    if (reposition) {
        PositionWindow(false);
    }
}

void DockWindow::PositionWindow(bool animate) {
    if (!hwnd_) {
        return;
    }

    const RECT work = MonitorWorkArea();
    POINT cursor = {};
    GetCursorPos(&cursor);

    RECT windowRect = WindowRect();
    const bool cursorInDock = PtInRect(&windowRect, cursor) != FALSE;
    const auto now = std::chrono::steady_clock::now();

    if (!settingsStore_.Current().autoHide) {
        hidden_ = false;
    } else if (cursorInDock || mouseInside_) {
        hidden_ = false;
        lastInsideTime_ = now;
    } else if (now - lastInsideTime_ > std::chrono::milliseconds(850)) {
        hidden_ = true;
    }

    const int targetX = work.left + ((work.right - work.left) - dockSize_.cx) / 2;
    const int visibleY = work.bottom - dockSize_.cy - kVisibleBottomMargin;
    const int hiddenY = work.bottom - kHiddenVisibleSliver;
    const int targetY = hidden_ ? hiddenY : visibleY;

    if (currentY_ == 0 || !animate) {
        currentY_ = targetY;
    } else {
        const int delta = targetY - currentY_;
        if (std::abs(delta) <= 2) {
            currentY_ = targetY;
        } else {
            currentY_ += delta / 4;
        }
    }

    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        targetX,
        currentY_,
        dockSize_.cx,
        dockSize_.cy,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void DockWindow::Paint() {
    PAINTSTRUCT paint = {};
    BeginPaint(hwnd_, &paint);
    EndPaint(hwnd_, &paint);

    if (!CreateDeviceResources()) {
        return;
    }

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    PaintDockSurface();
    PaintSystemSection();
    PaintApplicationSection();
    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        return;
    }

    PaintIconsWithGdi();
}

void DockWindow::PaintDockSurface() {
    const auto rect = RectF(0.5f, 0.5f, static_cast<float>(dockSize_.cx) - 0.5f, static_cast<float>(dockSize_.cy) - 0.5f);
    const auto rounded = D2D1::RoundedRect(rect, static_cast<float>(kSurfaceRadius), static_cast<float>(kSurfaceRadius));
    renderTarget_->FillRoundedRectangle(rounded, surfaceBrush_.Get());
    renderTarget_->DrawRoundedRectangle(rounded, borderBrush_.Get(), 1.0f);

    const auto highlight = D2D1::RoundedRect(RectF(1.0f, 1.0f, static_cast<float>(dockSize_.cx) - 1.0f, 22.0f), 22.0f, 22.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlightBrush;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.08f), highlightBrush.GetAddressOf());
    renderTarget_->FillRoundedRectangle(highlight, highlightBrush.Get());
}

void DockWindow::PaintSystemSection() {
    const std::wstring time = CurrentTimeText();
    const std::wstring date = CurrentDateText();

    renderTarget_->DrawTextW(
        time.c_str(),
        static_cast<UINT32>(time.size()),
        timeFormat_.Get(),
        RectF(14.0f, 22.0f, 90.0f, 43.0f),
        textBrush_.Get());

    renderTarget_->DrawTextW(
        date.c_str(),
        static_cast<UINT32>(date.size()),
        dateFormat_.Get(),
        RectF(14.0f, 45.0f, 90.0f, 66.0f),
        mutedBrush_.Get());

    renderTarget_->DrawLine(
        D2D1::Point2F(static_cast<float>(dividerRect_.left), static_cast<float>(dividerRect_.top)),
        D2D1::Point2F(static_cast<float>(dividerRect_.left), static_cast<float>(dividerRect_.bottom)),
        borderBrush_.Get(),
        1.0f);

    const auto monitorOuter = RectF(quickSettingsRect_.left + 5.0f, quickSettingsRect_.top + 6.0f, quickSettingsRect_.right - 5.0f, quickSettingsRect_.bottom - 8.0f);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(monitorOuter, 3.0f, 3.0f), mutedBrush_.Get(), 1.8f);
    renderTarget_->DrawLine(
        D2D1::Point2F(quickSettingsRect_.left + 13.0f, quickSettingsRect_.bottom - 6.0f),
        D2D1::Point2F(quickSettingsRect_.right - 13.0f, quickSettingsRect_.bottom - 6.0f),
        mutedBrush_.Get(),
        1.8f);

    const auto gearCenter = D2D1::Point2F((settingsRect_.left + settingsRect_.right) / 2.0f, (settingsRect_.top + settingsRect_.bottom) / 2.0f);
    renderTarget_->DrawEllipse(D2D1::Ellipse(gearCenter, 9.0f, 9.0f), mutedBrush_.Get(), 1.8f);
    renderTarget_->DrawEllipse(D2D1::Ellipse(gearCenter, 3.0f, 3.0f), mutedBrush_.Get(), 1.8f);
    for (int spoke = 0; spoke < 8; ++spoke) {
        const float angle = static_cast<float>(spoke) * 3.14159265f / 4.0f;
        const float x1 = gearCenter.x + std::cos(angle) * 11.0f;
        const float y1 = gearCenter.y + std::sin(angle) * 11.0f;
        const float x2 = gearCenter.x + std::cos(angle) * 14.0f;
        const float y2 = gearCenter.y + std::sin(angle) * 14.0f;
        renderTarget_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), mutedBrush_.Get(), 1.4f);
    }
}

void DockWindow::PaintApplicationSection() {
    for (size_t index = 0; index < applications_.size() && index < iconLayouts_.size(); ++index) {
        const auto& app = applications_[index];
        const auto& layout = iconLayouts_[index];
        const int count = std::min(3, app.WindowCount());
        if (count <= 0) {
            continue;
        }

        const float totalWidth = count * 8.0f + (count - 1) * 4.0f;
        float x = (layout.bounds.left + layout.bounds.right - totalWidth) / 2.0f;
        const float y = 78.0f;

        for (int line = 0; line < count; ++line) {
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(RectF(x, y, x + 8.0f, y + 2.5f), 1.5f, 1.5f),
                textBrush_.Get());
            x += 12.0f;
        }
    }
}

void DockWindow::PaintIconsWithGdi() {
    HDC dc = GetDC(hwnd_);
    if (!dc) {
        return;
    }

    SetBkMode(dc, TRANSPARENT);

    for (size_t index = 0; index < applications_.size() && index < iconLayouts_.size(); ++index) {
        const auto iconRect = ToRect(iconLayouts_[index].bounds);
        const int width = iconRect.right - iconRect.left;
        const int height = iconRect.bottom - iconRect.top;
        HICON icon = iconCache_.IconFor(applications_[index]);
        DrawIconEx(dc, iconRect.left, iconRect.top, icon, width, height, 0, nullptr, DI_NORMAL);
    }

    ReleaseDC(hwnd_, dc);
}

int DockWindow::HitTestSystemButton(POINT point) const {
    if (PtInRect(&quickSettingsRect_, point)) {
        return 1;
    }
    if (PtInRect(&settingsRect_, point)) {
        return 2;
    }
    return 0;
}

int DockWindow::HitTestApplication(POINT point) const {
    for (size_t index = 0; index < iconLayouts_.size(); ++index) {
        const RECT rect = ToRect(iconLayouts_[index].bounds);
        if (PtInRect(&rect, point)) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

void DockWindow::HandleLeftClick(POINT point) {
    const int systemButton = HitTestSystemButton(point);
    if (systemButton == 1) {
        ShellActions::OpenQuickSettings();
        return;
    }
    if (systemButton == 2) {
        RECT anchor = WindowRect();
        settingsWindow_.ShowNear(anchor);
        return;
    }

    const int appIndex = HitTestApplication(point);
    if (appIndex < 0 || appIndex >= static_cast<int>(applications_.size())) {
        return;
    }

    const auto& app = applications_[static_cast<size_t>(appIndex)];
    if (!app.windows.empty()) {
        ShellActions::ActivateWindow(app.windows.front().hwnd);
    } else {
        pinnedApps_.Launch(app);
    }
}

void DockWindow::HandleRightClick(POINT point) {
    const int appIndex = HitTestApplication(point);
    if (appIndex < 0 || appIndex >= static_cast<int>(applications_.size())) {
        return;
    }

    const auto& app = applications_[static_cast<size_t>(appIndex)];
    if (app.windows.empty()) {
        return;
    }

    RECT anchor = ToRect(iconLayouts_[static_cast<size_t>(appIndex)].bounds);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    previewWindow_.ShowFor(app, anchor);
}

void DockWindow::UpdateHover(POINT point) {
    const int previous = hoverIndex_;
    hoverIndex_ = HitTestApplication(point);

    if (previous != hoverIndex_) {
        HideTooltip();
        KillTimer(hwnd_, kTooltipTimer);
        if (hoverIndex_ >= 0) {
            SetTimer(hwnd_, kTooltipTimer, 550, nullptr);
        }
        UpdateLayout(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DockWindow::HideTooltip() {
    if (!tooltip_ || tooltipIndex_ < 0) {
        tooltipIndex_ = -1;
        return;
    }

    TOOLINFOW tool = {};
    tool.cbSize = sizeof(tool);
    tool.hwnd = hwnd_;
    tool.uId = 1;
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(tooltip_, TTM_DELTOOL, 0, reinterpret_cast<LPARAM>(&tool));
    tooltipIndex_ = -1;
}

void DockWindow::MaybeShowTooltip() {
    KillTimer(hwnd_, kTooltipTimer);
    if (!tooltip_ || hoverIndex_ < 0 || hoverIndex_ >= static_cast<int>(applications_.size())) {
        return;
    }

    HideTooltip();

    tooltipText_ = applications_[static_cast<size_t>(hoverIndex_)].displayName;

    TOOLINFOW tool = {};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = hwnd_;
    tool.uId = 1;
    tool.lpszText = tooltipText_.data();
    SendMessageW(tooltip_, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&tool));

    const auto& layout = iconLayouts_[static_cast<size_t>(hoverIndex_)];
    POINT position = {
        static_cast<LONG>((layout.bounds.left + layout.bounds.right) / 2.0f),
        static_cast<LONG>(layout.bounds.top - 8.0f),
    };
    ClientToScreen(hwnd_, &position);

    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(position.x, position.y));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
    tooltipIndex_ = hoverIndex_;
}

std::wstring DockWindow::CurrentTimeText() const {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    int hour = time.wHour % 12;
    if (hour == 0) {
        hour = 12;
    }

    std::wostringstream stream;
    stream << hour << L":" << FormatTwoDigits(time.wMinute) << (time.wHour >= 12 ? L" PM" : L" AM");
    return stream.str();
}

std::wstring DockWindow::CurrentDateText() const {
    wchar_t buffer[64] = {};
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr, buffer, _countof(buffer));
    return buffer;
}

RECT DockWindow::WindowRect() const {
    RECT rect = {};
    if (hwnd_) {
        GetWindowRect(hwnd_, &rect);
    }
    return rect;
}

RECT DockWindow::MonitorWorkArea() const {
    POINT cursor = {};
    GetCursorPos(&cursor);

    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (!monitor && hwnd_) {
        monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }

    RECT desktop = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    return desktop;
}

} // namespace MyDock
