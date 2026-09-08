#pragma once
#include <include/core/SkCanvas.h>
#include <include/core/SkTypeface.h>
#include <memory>

namespace vox::platform { class IPlatformWindow; }

namespace vox::ui {

/// Owns the drawing surface and presents it to a platform window.
///
/// With VOXCAST_HAS_SKIA=1 this wraps a GrDirectContext + SkSurface backed by
/// Metal (macOS) or D3D11 (Windows) and endFrame() flushes + presents on
/// vsync. Otherwise it wraps the mini-Skia software surface and presents via
/// UpdateLayeredWindow(). PopupView is identical in both cases.
class SkiaRenderer {
public:
    SkiaRenderer(void* nativeSurface, float backingScale);
    ~SkiaRenderer();

    void beginFrame();
    void endFrame();

    SkCanvas* canvas() { return canvas_.get(); }
    float widthDp() const  { return float(width_)  / scale_; }
    float heightDp() const { return float(height_) / scale_; }

    void resize(int pxWidth, int pxHeight);
    sk_sp<SkTypeface> uiTypeface() const { return typeface_; }

    /// Raw premultiplied BGRA — used by the layered-window presenter and by
    /// the offscreen PNG harness.
    const uint32_t* pixels() const;
    int pixelWidth() const  { return width_; }
    int pixelHeight() const { return height_; }

    /// Attach a window so endFrame() presents automatically.
    void attach(platform::IPlatformWindow* w) { window_ = w; }

private:
    std::unique_ptr<SkBitmapSurface> surface_;
    std::unique_ptr<SkCanvas> canvas_;
    sk_sp<SkTypeface> typeface_;
    platform::IPlatformWindow* window_{nullptr};
    int width_{0}, height_{0};
    float scale_{1.f};
};

} // namespace vox::ui
