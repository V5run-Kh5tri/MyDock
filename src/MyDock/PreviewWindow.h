#pragma once

#include "Models.h"

#include <Dwmapi.h>

#include <vector>

namespace MyDock {

class PreviewWindow {
public:
    explicit PreviewWindow(HINSTANCE instance);
    ~PreviewWindow();

    bool Create(HWND owner);
    void ShowFor(const ApplicationGroup& app, const RECT& anchor);
    void Hide();
    bool IsVisible() const;

private:
    struct ThumbnailBinding {
        HTHUMBNAIL handle = nullptr;
        RECT destination = {};
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    void RegisterThumbnails();
    void ClearThumbnails();
    int HitTestRow(POINT point) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    ApplicationGroup app_;
    std::vector<RECT> rowRects_;
    std::vector<ThumbnailBinding> thumbnails_;
};

} // namespace MyDock

