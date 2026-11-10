#pragma once

#include "DockMenuWindow.h"
#include "GlassBackdrop.h"
#include "IconCache.h"
#include "NativeTaskbarController.h"
#include "PinnedApps.h"
#include "PreviewWindow.h"
#include "SettingsWindow.h"
#include "WindowTracker.h"

#include <CommCtrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <map>

namespace MyDock {

class DockWindow {
public:
    DockWindow(
        HINSTANCE instance,
        SettingsStore& settingsStore,
        PinnedApps& pinnedApps,
        WindowTracker& windowTracker,
        IconCache& iconCache);

    ~DockWindow();

    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;

    bool Create();
    void Show();

    // Driven by the app message loop once per display frame while the dock is animating.
    bool WantsAnimationFrames() const;
    void AnimationFrame();
    HWND Handle() const {
        return hwnd_;
    }

private:
    struct IconLayout {
        D2D1_RECT_F bounds = {};
        float scale = 1.0f;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateDeviceResources();
    void DiscardDeviceResources();
    void RefreshApplications();
    void MergeApplications(const std::vector<ApplicationGroup>& running);
    void UpdateLayout(bool reposition);
    void PositionWindow(bool animate);
    void RequestRender();
    void Render();
    bool EnsureSurfaceBitmap(int width, int height);
    void ReleaseSurfaceBitmap();
    void PlaceBackdrop();
    void PaintDockSurface();
    void PaintSystemSection();
    void PaintApplicationSection();
    void PaintIconsWithD2D();
    void ApplySettings();
    void RestoreNativeTaskbarEmergency();
    ID2D1Bitmap* BitmapForIcon(HICON icon);

    int HitTestSystemButton(POINT point) const;
    int HitTestApplication(POINT point) const;
    bool StepAnimation(float deltaSeconds);
    void StartAnimation();
    void UpdateFocusFromCursor(POINT slabPoint);
    void HandleLeftClick(POINT point);
    void HandleRightClick(POINT point);
    void ShowApplicationMenu(int appIndex);
    void ExecuteMenuCommand(UINT command);
    void UpdateHover(POINT point);
    void HideTooltip();
    void MaybeShowTooltip();
    std::wstring CurrentTimeText() const;
    std::wstring CurrentDateText() const;
    RECT WindowRect() const;
    // Screen rect of the visible dock slab (the window is wider, to leave room for magnification).
    RECT SlabScreenRect() const;
    // Converts a client-area mouse position to slab coordinates used by layout and hit testing.
    POINT SlabPoint(LPARAM lParam) const;
    void ApplyTransparencySetting();
    RECT MonitorWorkArea() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    SettingsStore& settingsStore_;
    PinnedApps& pinnedApps_;
    WindowTracker& windowTracker_;
    IconCache& iconCache_;
    PreviewWindow previewWindow_;
    DockMenuWindow contextMenu_;
    SettingsWindow settingsWindow_;
    NativeTaskbarController taskbarController_;
    GlassBackdrop backdrop_;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassTintBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassShadowBrush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> glassSheenBrush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> glassRimBrush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> glassInnerRimBrush_;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> glassGlintBrush_;
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> glassEdgeStops_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hitTestBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> timeFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> dateFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> iconFormat_;
    std::map<HICON, Microsoft::WRL::ComPtr<ID2D1Bitmap>> iconBitmaps_;

    std::vector<ApplicationGroup> applications_;
    std::vector<IconLayout> iconLayouts_;
    std::vector<float> iconScales_;
    // Magnification focus as a fractional icon index (kNoFocus = none), from the cursor position.
    float hoverFocus_ = -1000.0f;
    float targetFocus_ = -1000.0f;
    float restingSlabWidth_ = 0.0f;
    bool animating_ = false;
    std::chrono::steady_clock::time_point lastAnimationFrame_ = std::chrono::steady_clock::now();
    RECT clockDateRect_ = {};
    RECT quickSettingsRect_ = {};
    RECT settingsRect_ = {};
    RECT startSearchRect_ = {};
    RECT dividerRect_ = {};
    SIZE dockSize_ = {560, 106};
    int hoverIndex_ = -1;
    int tooltipIndex_ = -1;
    HWND foregroundBeforeClick_ = nullptr;
    std::wstring tooltipText_;
    bool mouseInside_ = false;
    bool hidden_ = false;
    ApplicationGroup menuApp_;
    bool contextMenuAvailable_ = false;
    bool swallowLeftClick_ = false;
    bool previewAvailable_ = false;
    bool settingsAvailable_ = false;
    int currentY_ = 0;

    // Per-pixel-alpha surface presented with UpdateLayeredWindow. Grown, never shrunk, because the
    // dock width changes on every magnification frame.
    HDC surfaceDc_ = nullptr;
    HBITMAP surfaceBitmap_ = nullptr;
    HGDIOBJ previousSurfaceBitmap_ = nullptr;
    SIZE surfaceCapacity_ = {};
    POINT dockPosition_ = {};
    // The window keeps a fixed width while icons magnify (only the slab inside it grows), so hover
    // animation never moves or resizes the window. slabLeft_ is the slab offset inside the window.
    int windowWidth_ = 0;
    int slabLeft_ = 0;
    int presentedWidth_ = 0;
    bool transparencyEffects_ = true;
    bool renderPending_ = false;
    std::chrono::steady_clock::time_point lastInsideTime_ = std::chrono::steady_clock::now();
};

} // namespace MyDock
