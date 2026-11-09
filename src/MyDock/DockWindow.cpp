#include "DockWindow.h"

#include "ShellActions.h"

#include <Dwmapi.h>
#include <Windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace MyDock {
namespace {

constexpr wchar_t kDockClassName[] = L"MyDockDockWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kClockTimer = 2;
constexpr UINT_PTR kAutoHideTimer = 3;
constexpr UINT_PTR kTooltipTimer = 4;
constexpr UINT_PTR kAnimationTimer = 5;

constexpr int kWindowHeight = 106;
constexpr int kSurfaceTop = 28;
constexpr int kSurfaceHeight = 70;
constexpr int kSurfaceBottom = kSurfaceTop + kSurfaceHeight;
constexpr int kSurfaceRadius = 22;
constexpr int kOuterPadding = 12;
constexpr int kSystemSectionWidth = 188;
constexpr int kStartSearchSectionWidth = 46;
constexpr int kFixedSectionWidth = kSystemSectionWidth + kStartSearchSectionWidth;
constexpr int kBaseIconSize = 50;
constexpr int kMaxIconSize = 80;
constexpr int kIconGap = 10;
constexpr int kVisibleBottomMargin = 12;
constexpr int kHiddenVisibleSliver = 6;
constexpr int kTooltipDelayMs = 725;
constexpr float kAnimationEase = 0.24f;
constexpr float kFocusEase = 0.34f;
constexpr float kIconRestingBottom = 80.0f;
constexpr COLORREF kTransparentKeyColor = 0x00030201;

float ScaleForDistance(float distance) {
    const float normalized = std::min(std::abs(distance), 2.75f) / 2.75f;
    const float influence = std::max(0.0f, 1.0f - normalized * normalized);
    return 1.0f + 0.58f * influence * influence;
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

void LogDock(const wchar_t* message) {
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return;
    }

    std::wofstream log(std::filesystem::path(tempPath) / L"MyDock.log", std::ios::app);
    if (log.is_open()) {
        log << message << L"\n";
    }
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
          ApplySettings();
          hidden_ = false;
          PositionWindow(false);
          if (hwnd_) {
              InvalidateRect(hwnd_, nullptr, FALSE);
          }
      }) {}

bool DockWindow::Create() {
    LogDock(L"DockWindow::Create begin");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = DockWindow::WindowProc;
    wc.lpszClassName = kDockClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    const ATOM registered = RegisterClassExW(&wc);
    if (registered == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogDock(L"RegisterClassExW failed");
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
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
        LogDock(L"CreateWindowExW dock failed");
        return false;
    }

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &darkMode, sizeof(darkMode));
    SetLayeredWindowAttributes(hwnd_, kTransparentKeyColor, 0, LWA_COLORKEY);

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

    previewAvailable_ = previewWindow_.Create(hwnd_);
    if (!previewAvailable_) {
        LogDock(L"PreviewWindow::Create failed; continuing without previews");
    }

    settingsAvailable_ = settingsWindow_.Create(hwnd_);
    if (!settingsAvailable_) {
        LogDock(L"SettingsWindow::Create failed; continuing without settings popup");
    }

    taskbarController_.Initialize(hwnd_);
    ApplySettings();

    RefreshApplications();
    UpdateLayout(false);

    SetTimer(hwnd_, kRefreshTimer, 1000, nullptr);
    SetTimer(hwnd_, kClockTimer, 1000, nullptr);
    SetTimer(hwnd_, kAutoHideTimer, 30, nullptr);
    SetTimer(hwnd_, kAnimationTimer, 16, nullptr);
    LogDock(L"DockWindow::Create complete");
    return true;
}

void DockWindow::Show() {
    PositionWindow(false);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

LRESULT CALLBACK DockWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DockWindow*>(create->lpCreateParams);
        if (!self) {
            return FALSE;
        }

        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT DockWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarController_.IsTaskbarCreatedMessage(message)) {
        taskbarController_.OnTaskbarCreated();
        return 0;
    }

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
    case WM_LBUTTONDOWN:
        foregroundBeforeClick_ = GetAncestor(GetForegroundWindow(), GA_ROOT);
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
            taskbarController_.ReapplyIfEnabled();
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
        if (wParam == kAnimationTimer) {
            StepAnimation();
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (taskbarController_.IsRestoreHotkey(wParam)) {
            RestoreNativeTaskbarEmergency();
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
        KillTimer(hwnd_, kAnimationTimer);
        taskbarController_.Shutdown();
        DiscardDeviceResources();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void DockWindow::ApplySettings() {
    taskbarController_.ApplyEnabled(settingsStore_.Current().replaceNativeTaskbar);
}

void DockWindow::RestoreNativeTaskbarEmergency() {
    taskbarController_.EmergencyRestore();
    settingsStore_.Current().replaceNativeTaskbar = false;
    settingsStore_.Save();

    hidden_ = false;
    PositionWindow(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
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

    if (!wicFactory_) {
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory_.GetAddressOf())))) {
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

        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x18202A, 0.54f), surfaceBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x8FC8FF, 0.10f), surfaceAccentBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.18f), surfaceShadowBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.42f), borderBrush_.GetAddressOf());
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
    surfaceShadowBrush_.Reset();
    surfaceAccentBrush_.Reset();
    surfaceBrush_.Reset();
    iconBitmaps_.clear();
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
    if (hoverIndex_ >= static_cast<int>(applications_.size())) {
        hoverIndex_ = -1;
        hoverFocus_ = -1.0f;
    }
}

void DockWindow::UpdateLayout(bool reposition) {
    iconLayouts_.clear();
    iconLayouts_.reserve(applications_.size());
    if (iconScales_.size() != applications_.size()) {
        iconScales_.assign(applications_.size(), 1.0f);
    }

    float appWidth = static_cast<float>(kOuterPadding);
    std::vector<float> sizes;
    sizes.reserve(applications_.size());

    for (size_t index = 0; index < applications_.size(); ++index) {
        const float scale = index < iconScales_.size() ? iconScales_[index] : 1.0f;
        const float size = std::min(static_cast<float>(kMaxIconSize), kBaseIconSize * scale);
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
    dockSize_ = {std::max(360, targetWidth), kWindowHeight};

    float x = static_cast<float>(kFixedSectionWidth + kOuterPadding);
    for (size_t index = 0; index < applications_.size(); ++index) {
        const float size = sizes[index];
        IconLayout layout;
        layout.scale = size / kBaseIconSize;
        layout.bounds = RectF(x, kIconRestingBottom - size, x + size, kIconRestingBottom);
        iconLayouts_.push_back(layout);
        x += size + kIconGap + (size - kBaseIconSize) * 0.22f;
    }

    clockDateRect_ = {12, kSurfaceTop + 8, 94, kSurfaceBottom - 8};
    quickSettingsRect_ = {102, kSurfaceTop + 20, 132, kSurfaceTop + 50};
    settingsRect_ = {142, kSurfaceTop + 20, 172, kSurfaceTop + 50};
    startSearchRect_ = {kFixedSectionWidth - 38, kSurfaceTop + 20, kFixedSectionWidth - 8, kSurfaceTop + 50};
    dividerRect_ = {kFixedSectionWidth - 1, kSurfaceTop + 12, kFixedSectionWidth, kSurfaceBottom - 12};

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
    HDC paintDc = BeginPaint(hwnd_, &paint);

    if (!CreateDeviceResources()) {
        PaintFallback(paintDc);
        EndPaint(hwnd_, &paint);
        return;
    }
    EndPaint(hwnd_, &paint);

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(1.0f / 255.0f, 2.0f / 255.0f, 3.0f / 255.0f, 1.0f));
    PaintDockSurface();
    PaintSystemSection();
    PaintApplicationSection();
    PaintIconsWithD2D();
    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        return;
    }
}

void DockWindow::PaintFallback(HDC dc) {
    if (!dc) {
        return;
    }

    RECT client = {};
    GetClientRect(hwnd_, &client);
    HBRUSH background = CreateSolidBrush(RGB(32, 36, 42));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(130, 136, 148));
    HFONT font = CreateFontW(
        -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    SelectObject(dc, background);
    SelectObject(dc, border);
    RoundRect(dc, client.left, client.top, client.right, client.bottom, 28, 28);

    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(245, 247, 251));
    DrawTextW(dc, L"MyDock", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DeleteObject(font);
    DeleteObject(border);
    DeleteObject(background);
}

void DockWindow::PaintDockSurface() {
    const auto rect = RectF(0.5f, static_cast<float>(kSurfaceTop), static_cast<float>(dockSize_.cx) - 0.5f, static_cast<float>(kSurfaceBottom));
    const auto rounded = D2D1::RoundedRect(rect, static_cast<float>(kSurfaceRadius), static_cast<float>(kSurfaceRadius));
    const auto shadow = D2D1::RoundedRect(RectF(2.0f, static_cast<float>(kSurfaceTop + 6), static_cast<float>(dockSize_.cx) - 2.0f, static_cast<float>(kSurfaceBottom + 5)), static_cast<float>(kSurfaceRadius), static_cast<float>(kSurfaceRadius));
    renderTarget_->FillRoundedRectangle(shadow, surfaceShadowBrush_.Get());
    renderTarget_->FillRoundedRectangle(rounded, surfaceBrush_.Get());
    renderTarget_->FillRoundedRectangle(rounded, surfaceAccentBrush_.Get());
    renderTarget_->DrawRoundedRectangle(rounded, borderBrush_.Get(), 1.0f);

    const auto highlight = D2D1::RoundedRect(RectF(1.0f, static_cast<float>(kSurfaceTop + 1), static_cast<float>(dockSize_.cx) - 1.0f, static_cast<float>(kSurfaceTop + 24)), 22.0f, 22.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlightBrush;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.14f), highlightBrush.GetAddressOf());
    renderTarget_->FillRoundedRectangle(highlight, highlightBrush.Get());
}

void DockWindow::PaintSystemSection() {
    const std::wstring time = CurrentTimeText();
    const std::wstring date = CurrentDateText();

    renderTarget_->DrawTextW(
        time.c_str(),
        static_cast<UINT32>(time.size()),
        timeFormat_.Get(),
        RectF(14.0f, static_cast<float>(kSurfaceTop + 13), 90.0f, static_cast<float>(kSurfaceTop + 34)),
        textBrush_.Get());

    renderTarget_->DrawTextW(
        date.c_str(),
        static_cast<UINT32>(date.size()),
        dateFormat_.Get(),
        RectF(14.0f, static_cast<float>(kSurfaceTop + 36), 90.0f, static_cast<float>(kSurfaceTop + 57)),
        mutedBrush_.Get());

    renderTarget_->DrawLine(
        D2D1::Point2F(static_cast<float>(dividerRect_.left), static_cast<float>(dividerRect_.top)),
        D2D1::Point2F(static_cast<float>(dividerRect_.left), static_cast<float>(dividerRect_.bottom)),
        borderBrush_.Get(),
        1.0f);

    const auto searchCenter = D2D1::Point2F(
        (startSearchRect_.left + startSearchRect_.right) / 2.0f - 2.0f,
        (startSearchRect_.top + startSearchRect_.bottom) / 2.0f - 2.0f);
    renderTarget_->DrawEllipse(D2D1::Ellipse(searchCenter, 7.5f, 7.5f), mutedBrush_.Get(), 2.0f);
    renderTarget_->DrawLine(
        D2D1::Point2F(searchCenter.x + 5.5f, searchCenter.y + 5.5f),
        D2D1::Point2F(searchCenter.x + 11.5f, searchCenter.y + 11.5f),
        mutedBrush_.Get(),
        2.0f);

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
        const float y = static_cast<float>(kSurfaceBottom - 12);

        for (int line = 0; line < count; ++line) {
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(RectF(x, y, x + 8.0f, y + 2.5f), 1.5f, 1.5f),
                textBrush_.Get());
            x += 12.0f;
        }
    }
}

void DockWindow::PaintIconsWithD2D() {
    if (!renderTarget_) {
        return;
    }

    for (size_t index = 0; index < applications_.size() && index < iconLayouts_.size(); ++index) {
        HICON icon = iconCache_.IconFor(applications_[index]);
        ID2D1Bitmap* bitmap = BitmapForIcon(icon);
        if (bitmap) {
            renderTarget_->DrawBitmap(
                bitmap,
                iconLayouts_[index].bounds,
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
}

ID2D1Bitmap* DockWindow::BitmapForIcon(HICON icon) {
    if (!icon || !renderTarget_ || !wicFactory_) {
        return nullptr;
    }

    const auto existing = iconBitmaps_.find(icon);
    if (existing != iconBitmaps_.end()) {
        return existing->second.Get();
    }

    Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory_->CreateBitmapFromHICON(icon, wicBitmap.GetAddressOf()))) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf()))) {
        return nullptr;
    }

    if (FAILED(converter->Initialize(
            wicBitmap.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut))) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
    if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, d2dBitmap.GetAddressOf()))) {
        return nullptr;
    }

    ID2D1Bitmap* result = d2dBitmap.Get();
    iconBitmaps_[icon] = std::move(d2dBitmap);
    return result;
}

int DockWindow::HitTestSystemButton(POINT point) const {
    if (PtInRect(&clockDateRect_, point)) {
        return 3;
    }
    if (PtInRect(&quickSettingsRect_, point)) {
        return 1;
    }
    if (PtInRect(&settingsRect_, point)) {
        return 2;
    }
    if (PtInRect(&startSearchRect_, point)) {
        return 4;
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

void DockWindow::StepAnimation() {
    if (iconScales_.size() != applications_.size()) {
        iconScales_.assign(applications_.size(), 1.0f);
    }

    bool changed = false;
    const float targetFocus = hoverIndex_ >= 0 ? static_cast<float>(hoverIndex_) : -1.0f;
    if (targetFocus >= 0.0f && hoverFocus_ < 0.0f) {
        hoverFocus_ = targetFocus;
        changed = true;
    } else if (targetFocus >= 0.0f) {
        const float focusDelta = targetFocus - hoverFocus_;
        if (std::abs(focusDelta) < 0.015f) {
            if (hoverFocus_ != targetFocus) {
                hoverFocus_ = targetFocus;
                changed = true;
            }
        } else {
            hoverFocus_ += focusDelta * kFocusEase;
            changed = true;
        }
    }

    for (size_t index = 0; index < iconScales_.size(); ++index) {
        const float target = hoverIndex_ >= 0 && hoverFocus_ >= 0.0f ? ScaleForDistance(static_cast<float>(index) - hoverFocus_) : 1.0f;
        float& current = iconScales_[index];
        const float delta = target - current;
        if (std::abs(delta) < 0.01f) {
            if (current != target) {
                current = target;
                changed = true;
            }
            continue;
        }

        current += delta * kAnimationEase;
        changed = true;
    }

    if (changed) {
        UpdateLayout(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DockWindow::HandleLeftClick(POINT point) {
    const int systemButton = HitTestSystemButton(point);
    if (systemButton == 3) {
        ShellActions::OpenNotificationCenter();
        return;
    }
    if (systemButton == 1) {
        ShellActions::OpenQuickSettings();
        return;
    }
    if (systemButton == 2) {
        RECT anchor = WindowRect();
        if (settingsAvailable_) {
            settingsWindow_.ShowNear(anchor);
        } else {
            ShellActions::OpenSettingsHome();
        }
        return;
    }
    if (systemButton == 4) {
        ShellActions::OpenStartMenu();
        return;
    }

    const int appIndex = HitTestApplication(point);
    if (appIndex < 0 || appIndex >= static_cast<int>(applications_.size())) {
        return;
    }

    const auto& app = applications_[static_cast<size_t>(appIndex)];
    if (!app.windows.empty()) {
        HWND target = app.windows.front().hwnd;
        if (app.windows.size() > 1) {
            for (const auto& window : app.windows) {
                if (ShellActions::IsWindowActive(window.hwnd)) {
                    target = window.hwnd;
                    break;
                }
            }
        }

        if (app.windows.size() == 1 && !IsIconic(target)) {
            ShowWindow(target, SW_MINIMIZE);
        } else {
            ShellActions::ToggleWindowState(target, foregroundBeforeClick_);
        }
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
    if (previewAvailable_) {
        previewWindow_.ShowFor(app, anchor);
    }
}

void DockWindow::UpdateHover(POINT point) {
    const int previous = hoverIndex_;
    hoverIndex_ = HitTestApplication(point);

    if (previous != hoverIndex_) {
        HideTooltip();
        KillTimer(hwnd_, kTooltipTimer);
        if (hoverIndex_ >= 0) {
            SetTimer(hwnd_, kTooltipTimer, kTooltipDelayMs, nullptr);
        }
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
        if (settingsStore_.Current().replaceNativeTaskbar) {
            return info.rcMonitor;
        }
        return info.rcWork;
    }

    RECT desktop = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    return desktop;
}

} // namespace MyDock
