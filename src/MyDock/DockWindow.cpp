#include "DockWindow.h"

#include "ShellActions.h"

#include <Dwmapi.h>
#include <Windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <vector>

namespace MyDock {
namespace {

constexpr wchar_t kDockClassName[] = L"MyDockDockWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kClockTimer = 2;
constexpr UINT_PTR kAutoHideTimer = 3;
constexpr UINT_PTR kTooltipTimer = 4;
constexpr UINT kMenuOpenOrNewWindow = 1001;
constexpr UINT kMenuShowWindows = 1002;
constexpr UINT kMenuCloseWindow = 1003;
constexpr UINT kMenuCloseAllWindows = 1004;
constexpr UINT kMenuPinToggle = 1005;
constexpr UINT kMenuCommandMessage = WM_APP + 1;

constexpr int kWindowHeight = 106;
constexpr int kSurfaceTop = 28;
constexpr int kSurfaceHeight = 70;
constexpr int kSurfaceBottom = kSurfaceTop + kSurfaceHeight;
constexpr int kSurfaceRadius = 22;
constexpr int kOuterPadding = 12;
constexpr int kSystemSectionWidth = 188;
constexpr int kStartSearchSectionWidth = 46;
constexpr int kFixedSectionWidth = kSystemSectionWidth + kStartSearchSectionWidth;
// Resting icon size and placement: vertically centred in the 70 px slab with a 9 px top margin,
// leaving room for the running-window indicators underneath.
constexpr int kBaseIconSize = 46;
constexpr int kIconTopMargin = 9;
constexpr int kMaxIconSize = 80;
constexpr int kIconGap = 10;
constexpr int kVisibleBottomMargin = 12;
constexpr int kHiddenVisibleSliver = 6;
// When auto-hidden, the dock is revealed by touching the bottom edge anywhere within this zone:
// centred, at least as wide as the dock and covering this share of the monitor width.
constexpr int kHiddenActivationHeight = 8;
constexpr float kHiddenActivationWidthRatio = 0.7f;
constexpr int kTooltipDelayMs = 725;
// Time constant of the magnification easing. Lower is snappier, higher is floatier.
constexpr float kMagnifySmoothingSeconds = 0.07f;
// "No icon under the cursor" focus value; far enough away that every icon rests at scale 1.
constexpr float kNoFocus = -1000.0f;
constexpr float kIconRestingBottom = static_cast<float>(kSurfaceTop + kIconTopMargin + kBaseIconSize);
constexpr int kIndicatorTop = kSurfaceBottom - 9;
// Extra window width on each side of the resting slab, so magnification can widen the slab
// without resizing or moving the window.
constexpr int kWindowSideMargin = 80;
// The blur backdrop is inset under the glass rim so its aliased (region-clipped) edge is hidden
// beneath the dock's anti-aliased border.
constexpr int kBackdropInset = 2;

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

bool ContentBounds(IWICBitmapSource* source, WICRect& bounds) {
    if (!source) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }

    const UINT stride = width * 4;
    const UINT bufferSize = stride * height;
    std::vector<BYTE> pixels(bufferSize);
    WICRect fullRect = {0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    if (FAILED(source->CopyPixels(&fullRect, stride, bufferSize, pixels.data()))) {
        return false;
    }

    UINT left = width;
    UINT top = height;
    UINT right = 0;
    UINT bottom = 0;
    bool found = false;

    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const BYTE alpha = pixels[y * stride + x * 4 + 3];
            if (alpha <= 12) {
                continue;
            }

            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    const UINT contentWidth = right - left + 1;
    const UINT contentHeight = bottom - top + 1;
    if (contentWidth > width * 82 / 100 && contentHeight > height * 82 / 100) {
        return false;
    }

    const UINT contentSide = std::max(contentWidth, contentHeight);
    const UINT padding = std::max<UINT>(2, contentSide / 12);
    const UINT cropSide = std::min(std::max(contentSide + padding * 2, 1U), std::min(width, height));
    const int centerX = static_cast<int>((left + right) / 2);
    const int centerY = static_cast<int>((top + bottom) / 2);

    int cropLeft = centerX - static_cast<int>(cropSide / 2);
    int cropTop = centerY - static_cast<int>(cropSide / 2);
    cropLeft = std::max(0, std::min(cropLeft, static_cast<int>(width - cropSide)));
    cropTop = std::max(0, std::min(cropTop, static_cast<int>(height - cropSide)));

    bounds = {cropLeft, cropTop, static_cast<INT>(cropSide), static_cast<INT>(cropSide)};
    return true;
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

DWORD WindowsBuildNumber() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
        : nullptr;
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    return rtlGetVersion && rtlGetVersion(&info) == 0 ? info.dwBuildNumber : 0;
}

// Settings > Personalization > Colors > Transparency effects.
bool TransparencyEffectsEnabled() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"EnableTransparency",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return value != 0;
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
      contextMenu_(instance),
      settingsWindow_(instance, settingsStore, [this]() {
          ApplySettings();
          hidden_ = false;
          PositionWindow(false);
          RequestRender();
      }),
      backdrop_(instance) {}

DockWindow::~DockWindow() {
    backdrop_.Destroy();
    ReleaseSurfaceBitmap();
}

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
    // The dock is a per-pixel-alpha layered window (UpdateLayeredWindow, see Render()), so
    // translucent glass layers blend correctly with what is behind them. Fully transparent pixels
    // (the headroom above the surface) stay click-through, as with the old colour key.

    if (!backdrop_.Create()) {
        LogDock(L"GlassBackdrop::Create failed; continuing without blur");
    }
    ApplyTransparencySetting();

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

    contextMenuAvailable_ = contextMenu_.Create(hwnd_, kMenuCommandMessage);
    if (!contextMenuAvailable_) {
        LogDock(L"DockMenuWindow::Create failed; continuing without app context menu");
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
    LogDock(L"DockWindow::Create complete");
    return true;
}

void DockWindow::Show() {
    PositionWindow(false);
    Render(); // A layered window shows nothing until its first UpdateLayeredWindow.
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    PlaceBackdrop();
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
    case kMenuCommandMessage:
        ExecuteMenuCommand(static_cast<UINT>(wParam));
        return 0;
    case WM_PAINT: {
        // Layered (UpdateLayeredWindow) windows are not painted through the DC; just validate.
        PAINTSTRUCT paint = {};
        BeginPaint(hwnd_, &paint);
        EndPaint(hwnd_, &paint);
        if (renderPending_) {
            Render();
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && std::wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            ApplyTransparencySetting();
        }
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        UpdateLayout(true);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point = SlabPoint(lParam);
        if (!mouseInside_) {
            RequestRender(); // Adds the hover hit area above the slab.
        }
        mouseInside_ = true;
        lastInsideTime_ = std::chrono::steady_clock::now();
        UpdateHover(point);
        UpdateFocusFromCursor(point);

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
        targetFocus_ = kNoFocus; // Icons ease back to resting size.
        StartAnimation();
        RequestRender();
        return 0;
    case WM_LBUTTONDOWN:
        foregroundBeforeClick_ = GetAncestor(GetForegroundWindow(), GA_ROOT);
        // A click that only dismissed the context menu should not also activate an app.
        swallowLeftClick_ = contextMenu_.WasJustDismissed();
        return 0;
    case WM_LBUTTONUP: {
        if (swallowLeftClick_) {
            swallowLeftClick_ = false;
            return 0;
        }
        const POINT point = SlabPoint(lParam);
        HandleLeftClick(point);
        return 0;
    }
    case WM_RBUTTONUP: {
        const POINT point = SlabPoint(lParam);
        HandleRightClick(point);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            RefreshApplications();
            UpdateLayout(true);
            taskbarController_.ReapplyIfEnabled();
            // Re-assert z-order once a second rather than on every animation frame. Owned popups
            // (context menu, previews) stay above the dock; the backdrop goes right below it.
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            PlaceBackdrop();
            RequestRender();
            return 0;
        }
        if (wParam == kClockTimer) {
            RequestRender();
            return 0;
        }
        if (wParam == kAutoHideTimer) {
            PositionWindow(true);
            // Backstop in case WM_PAINT is not generated for the layered window.
            if (renderPending_) {
                Render();
            }
            return 0;
        }
        if (wParam == kTooltipTimer) {
            MaybeShowTooltip();
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
        taskbarController_.Shutdown();
        backdrop_.Destroy();
        DiscardDeviceResources();
        ReleaseSurfaceBitmap();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void DockWindow::ApplySettings() {
    taskbarController_.ApplyEnabled(settingsStore_.Current().replaceNativeTaskbar);
}

void DockWindow::ApplyTransparencySetting() {
    // Follow the Windows "Transparency effects" switch like the shell does: blurred glass when on,
    // a more solid tint when off.
    transparencyEffects_ = TransparencyEffectsEnabled();
    const bool accentApplied = backdrop_.SetBlur(transparencyEffects_);

    wchar_t message[160] = {};
    swprintf_s(
        message,
        L"Glass: Windows build %lu, transparency effects %s, blur %s",
        WindowsBuildNumber(),
        transparencyEffects_ ? L"on" : L"off",
        backdrop_.BlurEnabled() ? L"enabled" : (accentApplied ? L"disabled" : L"unavailable"));
    LogDock(message);

    // The tint opacity depends on the setting; rebuild brushes on the next render.
    DiscardDeviceResources();
    RequestRender();
}

void DockWindow::RestoreNativeTaskbarEmergency() {
    taskbarController_.EmergencyRestore();
    settingsStore_.Current().replaceNativeTaskbar = false;
    settingsStore_.Save();

    hidden_ = false;
    PositionWindow(false);
    RequestRender();
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
        // Renders into a premultiplied 32bpp DIB that Render() presents with UpdateLayeredWindow.
        // Software rendering: every frame ends up in system memory anyway, and for a surface this
        // small the CPU rasteriser avoids a GPU sync and read-back on each animation frame.
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, renderTarget_.GetAddressOf()))) {
            return false;
        }

        // ClearType needs an opaque background; on a transparent surface it produces colour fringes.
        renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        const auto white = [](float alpha) {
            return D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha);
        };
        const auto createStops = [this](std::initializer_list<D2D1_GRADIENT_STOP> stops, ID2D1GradientStopCollection** collection) {
            return SUCCEEDED(renderTarget_->CreateGradientStopCollection(
                stops.begin(), static_cast<UINT32>(stops.size()), collection));
        };
        const auto createVerticalBrush = [this, &createStops](
                                             std::initializer_list<D2D1_GRADIENT_STOP> stops,
                                             float top,
                                             float bottom,
                                             ID2D1LinearGradientBrush** brush) {
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            if (createStops(stops, collection.GetAddressOf())) {
                renderTarget_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, top), D2D1::Point2F(0.0f, bottom)),
                    collection.Get(),
                    brush);
            }
        };

        constexpr float surfaceTop = static_cast<float>(kSurfaceTop);
        constexpr float surfaceBottom = static_cast<float>(kSurfaceBottom);

        // Liquid glass layers, painted over the live blur from GlassBackdrop:
        // a light cool tint for legibility on bright wallpapers,
        // (more solid when Windows transparency effects are off, matching the shell),
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0x121821, transparencyEffects_ ? 0.30f : 0.86f), glassTintBrush_.GetAddressOf());
        // a soft contact shadow under the slab,
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.07f), glassShadowBrush_.GetAddressOf());
        // a sheen on the upper half, as if lit from above,
        createVerticalBrush(
            {{0.0f, white(0.22f)}, {0.6f, white(0.05f)}, {1.0f, white(0.0f)}},
            surfaceTop,
            surfaceTop + kSurfaceHeight * 0.55f,
            glassSheenBrush_.GetAddressOf());
        // a specular rim: bright at the top edge, faint on the sides, catching light again at the
        // bottom where a thick glass edge would refract it,
        createVerticalBrush(
            {{0.0f, white(0.75f)}, {0.22f, white(0.26f)}, {0.6f, white(0.08f)}, {1.0f, white(0.32f)}},
            surfaceTop,
            surfaceBottom,
            glassRimBrush_.GetAddressOf());
        // an inner bevel that suggests thickness,
        createVerticalBrush(
            {{0.0f, white(0.22f)}, {0.35f, white(0.0f)}, {0.8f, white(0.0f)}, {1.0f, white(0.12f)}},
            surfaceTop,
            surfaceBottom,
            glassInnerRimBrush_.GetAddressOf());
        // luminous left/right edges (the brush is built per frame because the dock width changes),
        createStops(
            {{0.0f, white(0.14f)}, {0.035f, white(0.0f)}, {0.965f, white(0.0f)}, {1.0f, white(0.14f)}},
            glassEdgeStops_.GetAddressOf());
        // and a specular glint near the top-left corner.
        {
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            if (createStops({{0.0f, white(0.45f)}, {1.0f, white(0.0f)}}, collection.GetAddressOf())) {
                renderTarget_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(D2D1::Point2F(92.0f, surfaceTop + 3.0f), D2D1::Point2F(0.0f, 0.0f), 112.0f, 16.0f),
                    collection.Get(),
                    glassGlintBrush_.GetAddressOf());
            }
        }

        // Alpha 1/255: invisible, but makes the headroom above the slab hit-testable while hovering
        // so the cursor passing between magnified icons does not flap the hover state.
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f / 255.0f), hitTestBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.28f), borderBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xF5F7FB, 0.98f), textBrush_.GetAddressOf());
        renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xD5DBE3, 0.92f), mutedBrush_.GetAddressOf());

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
    hitTestBrush_.Reset();
    glassEdgeStops_.Reset();
    glassGlintBrush_.Reset();
    glassInnerRimBrush_.Reset();
    glassRimBrush_.Reset();
    glassSheenBrush_.Reset();
    glassShadowBrush_.Reset();
    glassTintBrush_.Reset();
    iconBitmaps_.clear();
    renderTarget_.Reset();
}

void DockWindow::RefreshApplications() {
    if (pinnedApps_.Refresh()) {
        iconCache_.Clear();
        iconBitmaps_.clear();
    }

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
        app.iconIdentity = pinned.iconIdentity;
        if (app.iconIdentity.iconResourcePath.empty()) {
            app.iconIdentity.iconResourcePath = app.iconPath;
        }
        if (app.iconIdentity.executablePath.empty()) {
            app.iconIdentity.executablePath = app.launchCommand;
        }
        app.pinned = true;

        auto runningMatch = runningById.find(app.id);
        if (runningMatch == runningById.end()) {
            // Packaged apps run under an AUMID id; older manual pins may still use the exe name.
            runningMatch = std::find_if(runningById.begin(), runningById.end(), [&app](const auto& entry) {
                const auto& executable = entry.second.iconIdentity.executablePath;
                return !entry.second.iconIdentity.appUserModelId.empty() &&
                       !executable.empty() &&
                       IdentityFromExecutable(executable) == app.id;
            });
        }
        if (runningMatch != runningById.end()) {
            represented[runningMatch->first] = true;
            app.windows = runningMatch->second.windows;
            if (app.iconPath.empty()) {
                app.iconPath = runningMatch->second.iconPath;
            }
            if (app.iconIdentity.iconResourcePath.empty()) {
                app.iconIdentity.iconResourcePath = runningMatch->second.iconIdentity.iconResourcePath;
            }
            if (app.iconIdentity.executablePath.empty()) {
                app.iconIdentity.executablePath = runningMatch->second.iconIdentity.executablePath;
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
        hoverFocus_ = kNoFocus;
        targetFocus_ = kNoFocus;
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

    // Window width depends only on the resting (unmagnified) slab, so hovering never resizes it.
    const int iconCount = static_cast<int>(applications_.size());
    const float restingAppWidth = kOuterPadding +
                                  static_cast<float>(std::max(1, iconCount) * kBaseIconSize) +
                                  static_cast<float>(std::max(0, iconCount - 1) * kIconGap);
    const int restingWidth = std::max(360, static_cast<int>(std::ceil(kFixedSectionWidth + restingAppWidth + kOuterPadding)));
    restingSlabWidth_ = static_cast<float>(restingWidth);
    windowWidth_ = std::max(restingWidth + 2 * kWindowSideMargin, static_cast<int>(dockSize_.cx));
    slabLeft_ = (windowWidth_ - static_cast<int>(dockSize_.cx)) / 2;

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
    startSearchRect_ = {kSystemSectionWidth, kSurfaceTop + 20, kSystemSectionWidth + 30, kSurfaceTop + 50};
    dividerRect_ = {kFixedSectionWidth - 1, kSurfaceTop + 12, kFixedSectionWidth, kSurfaceBottom - 12};

    if (reposition) {
        PositionWindow(false);
    }
    RequestRender();
}

void DockWindow::PositionWindow(bool animate) {
    if (!hwnd_) {
        return;
    }

    const RECT work = MonitorWorkArea();
    POINT cursor = {};
    GetCursorPos(&cursor);

    RECT windowRect = SlabScreenRect();
    const bool cursorInDock = PtInRect(&windowRect, cursor) != FALSE;
    const auto now = std::chrono::steady_clock::now();
    const bool wasHidden = hidden_;

    // Reveal zone along the bottom edge while hidden: much wider than the dock itself, so the
    // cursor does not have to find the thin sliver exactly.
    const int workWidth = static_cast<int>(work.right - work.left);
    const int activationWidth = std::min(
        workWidth,
        std::max(static_cast<int>(dockSize_.cx), static_cast<int>(workWidth * kHiddenActivationWidthRatio)));
    const int activationLeft = static_cast<int>(work.left) + (workWidth - activationWidth) / 2;
    const RECT activationZone = {
        activationLeft,
        work.bottom - kHiddenActivationHeight,
        activationLeft + activationWidth,
        work.bottom,
    };
    const bool cursorInActivationZone = PtInRect(&activationZone, cursor) != FALSE;

    if (!settingsStore_.Current().autoHide) {
        hidden_ = false;
    } else if (cursorInDock || mouseInside_ || contextMenu_.IsVisible()) {
        // Keep the dock up while its context menu is open, even though the cursor is over the menu.
        hidden_ = false;
        lastInsideTime_ = now;
    } else if (hidden_ && cursorInActivationZone) {
        hidden_ = false;
        lastInsideTime_ = now;
    } else if (now - lastInsideTime_ > std::chrono::milliseconds(850)) {
        hidden_ = true;
    }

    if (!wasHidden && hidden_) {
        contextMenu_.Hide();
    }

    const int targetX = work.left + ((work.right - work.left) - windowWidth_) / 2;
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

    if (dockPosition_.x == targetX && dockPosition_.y == currentY_) {
        return;
    }

    dockPosition_ = {targetX, currentY_};
    if (presentedWidth_ != windowWidth_) {
        // The window is about to change size: move it in the same UpdateLayeredWindow call as the
        // new content, otherwise the old image is briefly shown at the new position.
        RequestRender();
        return;
    }

    // Pure move (auto-hide slide): shift the existing layered content and the blur together.
    SetWindowPos(hwnd_, nullptr, dockPosition_.x, dockPosition_.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PlaceBackdrop();
}

void DockWindow::RequestRender() {
    if (!hwnd_) {
        return;
    }

    // Coalesced: rendered on the next WM_PAINT or animation tick, whichever comes first.
    renderPending_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DockWindow::Render() {
    renderPending_ = false;
    if (!hwnd_) {
        return;
    }

    const int width = windowWidth_ > 0 ? windowWidth_ : static_cast<int>(dockSize_.cx);
    const int height = static_cast<int>(dockSize_.cy);
    if (width <= 0 || height <= 0 || !CreateDeviceResources() || !EnsureSurfaceBitmap(width, height)) {
        return;
    }

    const RECT bounds = {0, 0, width, height};
    if (FAILED(renderTarget_->BindDC(surfaceDc_, &bounds))) {
        return;
    }

    const float slabLeft = static_cast<float>(slabLeft_);
    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    renderTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    if (mouseInside_ && hitTestBrush_) {
        renderTarget_->FillRectangle(
            RectF(slabLeft, 0.0f, slabLeft + static_cast<float>(dockSize_.cx), static_cast<float>(kSurfaceTop)),
            hitTestBrush_.Get());
    }

    // Everything below is laid out in slab coordinates.
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Translation(slabLeft, 0.0f));
    PaintDockSurface();
    PaintSystemSection();
    PaintApplicationSection();
    PaintIconsWithD2D();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        renderPending_ = true;
        return;
    }
    if (FAILED(hr)) {
        return;
    }

    // Position, size and content in one call, then the blur right behind it.
    POINT source = {0, 0};
    POINT destination = dockPosition_;
    SIZE size = {width, height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    if (UpdateLayeredWindow(hwnd_, nullptr, &destination, &size, surfaceDc_, &source, 0, &blend, ULW_ALPHA)) {
        presentedWidth_ = width;
    }
    PlaceBackdrop();
}

bool DockWindow::EnsureSurfaceBitmap(int width, int height) {
    if (surfaceDc_ && surfaceBitmap_ && width <= surfaceCapacity_.cx && height <= surfaceCapacity_.cy) {
        return true;
    }

    // Grow with headroom so magnification (which widens the dock every frame) rarely reallocates.
    const int capacityWidth = std::max(width + 256, static_cast<int>(surfaceCapacity_.cx));
    const int capacityHeight = std::max(height, static_cast<int>(surfaceCapacity_.cy));
    ReleaseSurfaceBitmap();

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = capacityWidth;
    info.bmiHeader.biHeight = -capacityHeight; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    surfaceDc_ = CreateCompatibleDC(nullptr);
    if (!surfaceDc_) {
        return false;
    }

    void* bits = nullptr;
    surfaceBitmap_ = CreateDIBSection(surfaceDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!surfaceBitmap_) {
        ReleaseSurfaceBitmap();
        return false;
    }

    previousSurfaceBitmap_ = SelectObject(surfaceDc_, surfaceBitmap_);
    surfaceCapacity_ = {capacityWidth, capacityHeight};
    return true;
}

void DockWindow::ReleaseSurfaceBitmap() {
    if (surfaceDc_) {
        if (previousSurfaceBitmap_) {
            SelectObject(surfaceDc_, previousSurfaceBitmap_);
        }
        DeleteDC(surfaceDc_);
    }
    if (surfaceBitmap_) {
        DeleteObject(surfaceBitmap_);
    }

    surfaceDc_ = nullptr;
    surfaceBitmap_ = nullptr;
    previousSurfaceBitmap_ = nullptr;
    surfaceCapacity_ = {};
}

void DockWindow::PlaceBackdrop() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) {
        return;
    }

    const RECT glass = {
        dockPosition_.x + slabLeft_ + kBackdropInset,
        dockPosition_.y + kSurfaceTop + kBackdropInset,
        dockPosition_.x + slabLeft_ + dockSize_.cx - kBackdropInset,
        dockPosition_.y + kSurfaceBottom - kBackdropInset,
    };
    backdrop_.Place(hwnd_, glass, kSurfaceRadius - kBackdropInset);
}

void DockWindow::PaintDockSurface() {
    const float width = static_cast<float>(dockSize_.cx);
    const float top = static_cast<float>(kSurfaceTop);
    const float bottom = static_cast<float>(kSurfaceBottom);
    const float radius = static_cast<float>(kSurfaceRadius);
    const auto slab = D2D1::RoundedRect(RectF(0.0f, top, width, bottom), radius, radius);

    // Soft contact shadow, kept strictly below the slab so it never darkens the glass itself.
    if (glassShadowBrush_) {
        renderTarget_->PushAxisAlignedClip(RectF(0.0f, bottom - 1.0f, width, static_cast<float>(kWindowHeight)), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int layer = 0; layer < 4; ++layer) {
            const float spread = layer * 1.5f;
            renderTarget_->FillRoundedRectangle(
                D2D1::RoundedRect(RectF(6.0f - spread, top + 8.0f, width - 6.0f + spread, bottom + 1.0f + spread), radius, radius),
                glassShadowBrush_.Get());
        }
        renderTarget_->PopAxisAlignedClip();
    }

    if (glassTintBrush_) {
        renderTarget_->FillRoundedRectangle(slab, glassTintBrush_.Get());
    }

    if (glassEdgeStops_) {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> edgeBrush;
        if (SUCCEEDED(renderTarget_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, top), D2D1::Point2F(width, top)),
                glassEdgeStops_.Get(),
                edgeBrush.GetAddressOf()))) {
            renderTarget_->FillRoundedRectangle(slab, edgeBrush.Get());
        }
    }

    if (glassSheenBrush_) {
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(RectF(1.0f, top + 1.0f, width - 1.0f, top + kSurfaceHeight * 0.55f), radius - 1.0f, radius - 1.0f),
            glassSheenBrush_.Get());
    }

    if (glassGlintBrush_) {
        renderTarget_->FillRoundedRectangle(slab, glassGlintBrush_.Get());
    }

    if (glassInnerRimBrush_) {
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(RectF(2.5f, top + 2.5f, width - 2.5f, bottom - 2.5f), radius - 2.5f, radius - 2.5f),
            glassInnerRimBrush_.Get(),
            2.0f);
    }

    if (glassRimBrush_) {
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(RectF(0.75f, top + 0.75f, width - 0.75f, bottom - 0.75f), radius - 0.75f, radius - 0.75f),
            glassRimBrush_.Get(),
            1.25f);
    }
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
        const float y = static_cast<float>(kIndicatorTop);

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

    Microsoft::WRL::ComPtr<IWICBitmapSource> bitmapSource;
    converter.As(&bitmapSource);

    WICRect cropRect = {};
    Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
    if (ContentBounds(converter.Get(), cropRect) &&
        SUCCEEDED(wicFactory_->CreateBitmapClipper(clipper.GetAddressOf())) &&
        SUCCEEDED(clipper->Initialize(converter.Get(), &cropRect))) {
        clipper.As(&bitmapSource);
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap;
    if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(bitmapSource.Get(), nullptr, d2dBitmap.GetAddressOf()))) {
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

bool DockWindow::StepAnimation(float deltaSeconds) {
    if (iconScales_.size() != applications_.size()) {
        iconScales_.assign(applications_.size(), 1.0f);
    }

    // Frame-rate independent exponential smoothing: the same feel at 60, 120 or 144 Hz and when a
    // frame arrives late.
    const float blend = 1.0f - std::exp(-std::max(0.0f, deltaSeconds) / kMagnifySmoothingSeconds);
    hoverFocus_ = targetFocus_;

    bool changed = false;
    for (size_t index = 0; index < iconScales_.size(); ++index) {
        const float target = hoverFocus_ >= kNoFocus + 1.0f ? ScaleForDistance(static_cast<float>(index) - hoverFocus_) : 1.0f;
        float& current = iconScales_[index];
        const float delta = target - current;
        if (std::abs(delta) < 0.002f) {
            if (current != target) {
                current = target;
                changed = true;
            }
            continue;
        }

        current += delta * blend;
        changed = true;
    }

    if (changed) {
        UpdateLayout(true);
    }
    return changed;
}

bool DockWindow::WantsAnimationFrames() const {
    return animating_;
}

void DockWindow::AnimationFrame() {
    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = std::chrono::duration<float>(now - lastAnimationFrame_).count();
    lastAnimationFrame_ = now;
    // After an idle period the first frame should not jump; clamp long gaps to one 60 Hz frame.
    if (deltaSeconds <= 0.0f || deltaSeconds > 0.05f) {
        deltaSeconds = 1.0f / 60.0f;
    }

    animating_ = StepAnimation(deltaSeconds);
    if (renderPending_) {
        Render(); // Present this frame now rather than on a later WM_PAINT.
    }
}

void DockWindow::StartAnimation() {
    if (!animating_) {
        animating_ = true;
        lastAnimationFrame_ = std::chrono::steady_clock::now();
    }
}

void DockWindow::UpdateFocusFromCursor(POINT slabPoint) {
    // Magnification follows the cursor continuously (like the macOS dock) instead of snapping to
    // the hovered icon's index. The focus is measured against the resting (unmagnified) layout,
    // which does not move as icons grow, so there is no feedback between cursor and layout.
    const int count = static_cast<int>(applications_.size());
    float focus = kNoFocus;
    if (count > 0 && slabPoint.y >= 0 && slabPoint.y < kSurfaceBottom) {
        const float windowX = static_cast<float>(slabPoint.x + slabLeft_);
        const float restingSlabLeft = (windowWidth_ - restingSlabWidth_) / 2.0f;
        const float firstCenter = kFixedSectionWidth + kOuterPadding + kBaseIconSize / 2.0f;
        const float pitch = static_cast<float>(kBaseIconSize + kIconGap);
        const float candidate = (windowX - restingSlabLeft - firstCenter) / pitch;
        if (candidate >= -0.5f && candidate <= count - 0.5f) {
            focus = candidate;
        }
    }

    if (focus != targetFocus_) {
        targetFocus_ = focus;
        StartAnimation();
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
        RECT anchor = SlabScreenRect();
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

    ShowApplicationMenu(appIndex);
}

void DockWindow::ShowApplicationMenu(int appIndex) {
    if (!contextMenuAvailable_ ||
        appIndex < 0 ||
        appIndex >= static_cast<int>(applications_.size()) ||
        appIndex >= static_cast<int>(iconLayouts_.size())) {
        return;
    }

    // Snapshot the app: applications_ is rebuilt every second, so neither an index nor a
    // reference would still be valid when the command message arrives.
    menuApp_ = applications_[static_cast<size_t>(appIndex)];

    const bool canLaunch = !menuApp_.launchCommand.empty();
    const bool hasWindows = !menuApp_.windows.empty();

    std::vector<DockMenuItem> items;
    auto addItem = [&items](UINT command, const wchar_t* label, wchar_t glyph) {
        DockMenuItem item;
        item.command = command;
        item.label = label;
        item.glyph = glyph;
        items.push_back(std::move(item));
    };
    auto addSeparator = [&items]() {
        if (!items.empty() && !items.back().separator) {
            DockMenuItem separator;
            separator.separator = true;
            items.push_back(std::move(separator));
        }
    };

    if (canLaunch) {
        addItem(kMenuOpenOrNewWindow, hasWindows ? L"New window" : L"Open", hasWindows ? L'\xE710' : L'\xE8A7');
    }
    if (hasWindows && previewAvailable_) {
        addItem(kMenuShowWindows, menuApp_.windows.size() > 1 ? L"Show windows" : L"Show window", L'\xE7C4');
    }
    if (hasWindows) {
        addSeparator();
        addItem(kMenuCloseWindow, L"Close window", L'\xE711');
        if (menuApp_.windows.size() > 1) {
            addItem(kMenuCloseAllWindows, L"Close all windows", L'\xE711');
        }
    }
    addSeparator();
    addItem(kMenuPinToggle, menuApp_.pinned ? L"Unpin from MyDock" : L"Pin to MyDock", menuApp_.pinned ? L'\xE77A' : L'\xE718');

    HideTooltip();
    KillTimer(hwnd_, kTooltipTimer);

    RECT anchor = ToRect(iconLayouts_[static_cast<size_t>(appIndex)].bounds);
    OffsetRect(&anchor, slabLeft_, 0);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);

    // Open above the whole dock window (which already includes headroom for magnified icons),
    // never over it.
    constexpr LONG kMenuGap = 6;
    const RECT dock = SlabScreenRect();
    contextMenu_.Show(menuApp_.displayName, std::move(items), anchor.left, anchor.right, dock.top - kMenuGap);
}

void DockWindow::ExecuteMenuCommand(UINT command) {
    const ApplicationGroup app = menuApp_;
    if (app.id.empty()) {
        return;
    }

    auto primaryWindow = [&app]() -> HWND {
        if (app.windows.empty()) {
            return nullptr;
        }

        for (const auto& window : app.windows) {
            if (ShellActions::IsWindowActive(window.hwnd)) {
                return window.hwnd;
            }
        }

        return app.windows.front().hwnd;
    };

    switch (command) {
    case kMenuOpenOrNewWindow:
        pinnedApps_.Launch(app);
        break;
    case kMenuShowWindows: {
        // Re-locate the app: the dock may have changed since the menu opened.
        RECT anchor = SlabScreenRect();
        for (size_t index = 0; index < applications_.size() && index < iconLayouts_.size(); ++index) {
            if (ToLower(applications_[index].id) == ToLower(app.id)) {
                anchor = ToRect(iconLayouts_[index].bounds);
                OffsetRect(&anchor, slabLeft_, 0);
                MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
                break;
            }
        }
        previewWindow_.ShowFor(app, anchor);
        break;
    }
    case kMenuCloseWindow:
        if (HWND hwnd = primaryWindow(); hwnd && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case kMenuCloseAllWindows:
        for (const auto& window : app.windows) {
            if (IsWindow(window.hwnd)) {
                PostMessageW(window.hwnd, WM_CLOSE, 0, 0);
            }
        }
        break;
    case kMenuPinToggle:
        if (app.pinned) {
            pinnedApps_.Unpin(app);
        } else {
            pinnedApps_.Pin(app);
        }
        RefreshApplications();
        iconBitmaps_.clear();
        UpdateLayout(true);
        RequestRender();
        break;
    default:
        break;
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
        RequestRender();
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
    position.x += slabLeft_;
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

RECT DockWindow::SlabScreenRect() const {
    RECT rect = WindowRect();
    rect.left += slabLeft_;
    rect.right = rect.left + dockSize_.cx;
    return rect;
}

POINT DockWindow::SlabPoint(LPARAM lParam) const {
    return {GET_X_LPARAM(lParam) - slabLeft_, GET_Y_LPARAM(lParam)};
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
