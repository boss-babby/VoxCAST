#include "SkiaRenderer.h"
#include "platform/Platform.h"

#include <algorithm>

namespace vox::platform {
// Implemented in platform/win/WinPlatform.cpp (and mac/MacWindow.mm).
void PresentToWindow(IPlatformWindow*, const uint32_t*, int, int);
}

namespace vox::ui {

SkiaRenderer::SkiaRenderer(void* /*nativeSurface*/, float backingScale)
    : scale_(std::max(backingScale, 0.5f)) {
    resize(int(320 * scale_), int(72 * scale_));
    typeface_ = SkTypeface::MakeDefault();
}

SkiaRenderer::~SkiaRenderer() = default;

void SkiaRenderer::resize(int pxW, int pxH) {
    if (pxW == width_ && pxH == height_ && surface_) return;
    width_ = std::max(pxW, 1);
    height_ = std::max(pxH, 1);
    surface_ = std::make_unique<SkBitmapSurface>(width_, height_);
    canvas_ = std::make_unique<SkCanvas>(surface_.get());
}

void SkiaRenderer::beginFrame() {
    canvas_->clear(SK_ColorTRANSPARENT);
    canvas_->save();
    canvas_->scale(scale_, scale_);   // draw in DIPs, rasterise at device res
}

void SkiaRenderer::endFrame() {
    canvas_->restore();
    if (window_)
        platform::PresentToWindow(window_, surface_->pixels(), width_, height_);
}

const uint32_t* SkiaRenderer::pixels() const { return surface_->pixels(); }

} // namespace vox::ui
