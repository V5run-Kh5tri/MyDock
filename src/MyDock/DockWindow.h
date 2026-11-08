#pragma once

#include "IconCache.h"
#include "PinnedApps.h"
#include "PreviewWindow.h"
#include "SettingsWindow.h"
#include "WindowTracker.h"

#include <CommCtrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <chrono>

namespace MyDock {

class DockWindow {
public:
    DockWindow(
        HINSTANCE instance,
        SettingsStore& settingsStore,
        PinnedApps& pinnedApps,
        WindowTracker& windowTracker,
        IconCache& iconCache);

    bool Create();
    void Show();
    HWND Handle() const {
        return hwnd_;
    }

private:
    struct IconLayout {
        RECTF bounds = {};
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
    void Paint();
    void PaintDockSurface();
    void PaintSystemSection();
    void PaintApplicationSection();
    void PaintIconsWithGdi();

    int HitTestSystemButton(POINT point) const;
    int HitTestApplication(POINT point) const;
    void HandleLeftClick(POINT point);
    void HandleRightClick(POINT point);
    void UpdateHover(POINT point);
    void HideTooltip();
    void MaybeShowTooltip();
    std::wstring CurrentTimeText() const;
    std::wstring CurrentDateText() const;
    RECT WindowRect() const;
    RECT MonitorWorkArea() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    SettingsStore& settingsStore_;
    PinnedApps& pinnedApps_;
    WindowTracker& windowTracker_;
    IconCache& iconCache_;
    PreviewWindow previewWindow_;
    SettingsWindow settingsWindow_;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surfaceBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> timeFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> dateFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> iconFormat_;

    std::vector<ApplicationGroup> applications_;
    std::vector<IconLayout> iconLayouts_;
    RECT quickSettingsRect_ = {};
    RECT settingsRect_ = {};
    RECT dividerRect_ = {};
    SIZE dockSize_ = {560, 88};
    int hoverIndex_ = -1;
    int tooltipIndex_ = -1;
    std::wstring tooltipText_;
    bool mouseInside_ = false;
    bool hidden_ = false;
    int currentY_ = 0;
    std::chrono::steady_clock::time_point lastInsideTime_ = std::chrono::steady_clock::now();
};

} // namespace MyDock
