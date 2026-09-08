// ============================================================================
//  VoxCast — ui/PopupView.cpp
//  The centerpiece. A capsule drawn entirely with Skia on the GPU
//  (Metal / D3D11) over a native compositor-blurred backdrop.
//
//  State machine:  Idle → Listening → Processing → Result → (exit)
//                        └──────── Cancelled ────────┘
//  Every visual property is driven by a Spring or a bezier Tween; there is
//  not a single linear interpolation or instant jump in this file.
// ============================================================================
#include "PopupView.h"
#include "Theme.h"
#include "Animation.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>
#include <include/core/SkFont.h>
#include <include/core/SkTextBlob.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkImageFilters.h>
#include <include/effects/SkDashPathEffect.h>

#include <algorithm>
#include <cmath>

namespace vox::ui {

using namespace vox::theme;
using namespace vox::anim;

namespace {
inline SkColor sk(const Color& c) { return SkColorSetARGB(
    uint8_t(std::clamp(c.a,0.f,1.f)*255), uint8_t(c.r*255), uint8_t(c.g*255), uint8_t(c.b*255)); }
}

// ---------------------------------------------------------------------------
PopupView::PopupView() {
    for (auto& b : bars_) {
        b.spring.configure(motion::barStiffness, motion::barDamping, motion::barMass);
        b.spring.snap(0.06f);
    }
    scale_.configure(560.f, 32.f);
    scale_.snap(0.90f);
    opacity_.configure(420.f, 34.f);
    opacity_.snap(0.f);
    width_.configure(300.f, 30.f);
    width_.snap(kBaseWidth);
    shake_.configure(900.f, 14.f);
    shake_.snap(0.f);
}

// ---------------------------------------------------------------------------
void PopupView::setState(PopupState s) {
    if (s == state_) return;
    prevState_ = state_;
    state_ = s;
    stateTime_ = 0.f;

    switch (s) {
        case PopupState::Hidden:
            opacity_.setTarget(0.f); scale_.setTarget(0.92f);
            break;
        case PopupState::Idle:
            opacity_.setTarget(1.f); scale_.setTarget(1.f);
            width_.setTarget(kBaseWidth);
            for (auto& b : bars_) b.spring.setTarget(0.06f);
            break;
        case PopupState::Listening:
            opacity_.setTarget(1.f); scale_.setTarget(1.f);
            width_.setTarget(kBaseWidth);
            break;
        case PopupState::Processing:
            for (auto& b : bars_) b.spring.setTarget(0.28f);
            break;
        case PopupState::Result:
            checkTween_.restart();
            width_.setTarget(kBaseWidth + 40.f);
            for (auto& b : bars_) b.spring.setTarget(0.04f);
            break;
        case PopupState::Cancelled:
            shake_.snap(1.f); shake_.setTarget(0.f);
            opacity_.setTarget(0.f); scale_.setTarget(0.94f);
            break;
    }
}

void PopupView::pushAudioLevel(float rms, const float* spectrum, size_t bins) {
    // Perceptual compression: raw RMS is far too quiet to look alive.
    level_ = std::pow(std::clamp(rms * 6.2f, 0.f, 1.f), 0.62f);
    for (size_t i = 0; i < bars_.size(); ++i) {
        float energy = level_;
        if (spectrum && bins) {
            size_t lo = i * bins / bars_.size();
            size_t hi = std::max(lo + 1, (i + 1) * bins / bars_.size());
            float acc = 0.f;
            for (size_t k = lo; k < hi; ++k) acc += spectrum[k];
            energy = std::pow(std::clamp(acc / float(hi - lo) * 8.f, 0.f, 1.f), 0.6f);
        } else {
            // No FFT: shape a pleasing centre-weighted envelope around the RMS.
            float x = (float(i) / float(bars_.size() - 1)) * 2.f - 1.f;
            energy *= 0.55f + 0.45f * std::cos(x * 1.35f);
            energy *= 0.85f + 0.30f * bars_[i].jitter;
        }
        bars_[i].spring.setTarget(std::clamp(energy, 0.05f, 1.f));
    }
}

void PopupView::setPartialTranscript(std::string text) {
    partial_ = std::move(text);
    captionAlpha_.setTarget(partial_.empty() ? 0.f : 1.f);
    // Grow the capsule to accommodate a caption line.
    width_.setTarget(partial_.empty() ? kBaseWidth : kWideWidth);
}

// ---------------------------------------------------------------------------
void PopupView::update(float dt) {
    time_      += dt;
    stateTime_ += dt;

    // Deterministic per-bar jitter (no rand() on the render thread).
    for (size_t i = 0; i < bars_.size(); ++i)
        bars_[i].jitter = 0.5f + 0.5f * std::sin(time_ * (3.1f + 0.37f * i) + float(i));

    if (state_ == PopupState::Idle) {
        float b = breathe(time_, motion::breathCycle);
        glow_ = lerp(0.18f, 0.42f, b);
        for (auto& bar : bars_) bar.spring.setTarget(lerp(0.05f, 0.12f, b));
    } else if (state_ == PopupState::Listening) {
        glow_ = lerp(0.35f, 0.85f, level_);
    } else if (state_ == PopupState::Processing) {
        // Travelling wave: bars become a looping "thinking" pulse.
        for (size_t i = 0; i < bars_.size(); ++i) {
            float phase = time_ * 3.4f - float(i) * 0.36f;
            bars_[i].spring.setTarget(0.16f + 0.52f * std::pow(std::max(0.f, std::sin(phase)), 3.f));
        }
        glow_ = 0.55f + 0.25f * std::sin(time_ * 2.2f);
    }

    for (auto& b : bars_) b.spring.update(dt);
    scale_.update(dt);
    opacity_.update(dt);
    width_.update(dt);
    shake_.update(dt);
    captionAlpha_.update(dt);
    checkTween_.update(dt);
    borderAngle_ = std::fmod(borderAngle_ + dt * 0.55f, 6.2831853f);

    if (state_ == PopupState::Result && stateTime_ > motion::resultHold)
        setState(PopupState::Hidden);
    if (state_ == PopupState::Hidden && opacity_.value() < 0.01f && onDismissed)
        { onDismissed(); onDismissed = nullptr; }
}

// ---------------------------------------------------------------------------
void PopupView::draw(SkCanvas* canvas, float w, float h) {
    const auto& P = Theme::get().p();
    canvas->clear(SK_ColorTRANSPARENT);

    const float alpha = std::clamp(opacity_.value(), 0.f, 1.f);
    if (alpha <= 0.002f) return;

    const float capW = width_.value();
    const float capH = kBaseHeight + (partial_.empty() ? 0.f : 20.f * captionAlpha_.value());
    const float cx   = w * 0.5f + shake_.value() * 6.f * std::sin(time_ * 46.f);
    const float cy   = h * 0.5f;

    canvas->save();
    canvas->translate(cx, cy);
    canvas->scale(scale_.value(), scale_.value());
    canvas->translate(-capW * 0.5f, -capH * 0.5f);

    const SkRect  rect  = SkRect::MakeWH(capW, capH);
    const float   r     = capH * 0.5f;
    const SkRRect rrect = SkRRect::MakeRectXY(rect, r, r);

    // ---- 1. Ambient drop shadow (two-layer: contact + diffuse) -------------
    {
        SkPaint sh;
        sh.setAntiAlias(true);
        sh.setColor(SkColorSetARGB(uint8_t(85 * alpha), 0, 0, 0));
        sh.setImageFilter(SkImageFilters::Blur(8.f, 8.f, nullptr));
        canvas->save(); canvas->translate(0, 4.f);
        canvas->drawRRect(rrect, sh);
        canvas->restore();

        sh.setColor(SkColorSetARGB(uint8_t(50 * alpha), 0, 0, 0));
        sh.setImageFilter(SkImageFilters::Blur(2.5f, 2.5f, nullptr));
        canvas->save(); canvas->translate(0, 1.5f);
        canvas->drawRRect(rrect, sh);
        canvas->restore();
    }

    // ---- 2. Accent bloom behind the capsule (state-reactive) ---------------
    if (glow_ > 0.01f) {
        SkPaint bloom;
        bloom.setAntiAlias(true);
        const SkColor stops[2] = { sk(Theme::get().accentAt(0.f).withAlpha(0.40f * glow_ * alpha)),
                                   sk(Theme::get().accentAt(1.f).withAlpha(0.0f)) };
        const SkPoint c = { capW * 0.5f, capH * 0.5f };
        bloom.setShader(SkGradientShader::MakeRadial(c, capW * 0.65f, stops, nullptr, 2,
                                                     SkTileMode::kClamp));
        bloom.setImageFilter(SkImageFilters::Blur(12.f, 12.f, nullptr));
        canvas->drawRRect(SkRRect::MakeRectXY(rect.makeOutset(8, 8), r, r), bloom);
    }

    // ---- 3. Glass material (sits over native NSVisualEffect / Acrylic) -----
    {
        SkPaint mat;
        mat.setAntiAlias(true);
        const SkColor stops[2] = {
            sk(P.surface.withAlpha(P.surface.a * alpha * 1.04f)),
            sk(P.surface.withAlpha(P.surface.a * alpha * 0.88f)) };
        const SkPoint pts[2] = { {0, 0}, {0, capH} };
        mat.setShader(SkGradientShader::MakeLinear(pts, stops, nullptr, 2, SkTileMode::kClamp));
        canvas->drawRRect(rrect, mat);

        // Specular top highlight — the "glass" tell.
        SkPaint spec;
        spec.setAntiAlias(true);
        const SkColor hs[2] = { SkColorSetARGB(uint8_t(46 * alpha), 255, 255, 255),
                                SkColorSetARGB(0, 255, 255, 255) };
        const SkPoint hp[2] = { {0, 0}, {0, capH * 0.55f} };
        spec.setShader(SkGradientShader::MakeLinear(hp, hs, nullptr, 2, SkTileMode::kClamp));
        canvas->save();
        canvas->clipRRect(rrect, true);
        canvas->drawRect(SkRect::MakeWH(capW, capH * 0.55f), spec);
        canvas->restore();
    }

    // ---- 4. Animated conic gradient border ---------------------------------
    {
        SkPaint border;
        border.setAntiAlias(true);
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(1.25f);

        const bool active = (state_ == PopupState::Listening || state_ == PopupState::Processing);
        const float aBoost = active ? 1.f : 0.35f;
        const SkColor stops[4] = {
            sk(Theme::get().accentAt(0.f).withAlpha(0.95f * alpha * aBoost)),
            sk(Theme::get().accentAt(1.f).withAlpha(0.95f * alpha * aBoost)),
            sk(P.stroke.withAlpha(P.stroke.a * alpha)),
            sk(Theme::get().accentAt(0.f).withAlpha(0.95f * alpha * aBoost)) };
        const SkScalar pos[4] = { 0.f, 0.28f, 0.62f, 1.f };
        SkMatrix rot; rot.setRotate(borderAngle_ * 57.2957795f, capW * 0.5f, capH * 0.5f);
        border.setShader(SkGradientShader::MakeSweep(capW * 0.5f, capH * 0.5f,
                                                     stops, pos, 4, 0, &rot));
        canvas->drawRRect(rrect.makeOffset(0, 0), border);
    }

    // ---- 5. Content ---------------------------------------------------------
    canvas->save();
    canvas->clipRRect(rrect, true);
    switch (state_) {
        case PopupState::Result:    drawResult(canvas, capW, capH, alpha); break;
        case PopupState::Cancelled: drawCancelled(canvas, capW, capH, alpha); break;
        default:                    drawWaveform(canvas, capW, capH, alpha); break;
    }
    if (!partial_.empty() && captionAlpha_.value() > 0.01f)
        drawCaption(canvas, capW, capH, alpha * captionAlpha_.value());
    canvas->restore();

    canvas->restore();
}

// ---------------------------------------------------------------------------
void PopupView::drawWaveform(SkCanvas* canvas, float w, float h, float alpha) {
    const float padX     = 18.f;
    const float rowH     = kBaseHeight;
    const float maxH     = rowH * 0.38f;
    const float n        = float(bars_.size());
    const float slot     = (w - padX * 2.f) / n;
    const float barW     = 2.2f;
    const float midY     = rowH * 0.5f;

    SkPaint p; p.setAntiAlias(true);
    const SkColor stops[2] = { sk(Theme::get().accentAt(0.f).withAlpha(alpha)),
                               sk(Theme::get().accentAt(1.f).withAlpha(alpha)) };
    const SkPoint pts[2] = { {padX, 0}, {w - padX, 0} };
    p.setShader(SkGradientShader::MakeLinear(pts, stops, nullptr, 2, SkTileMode::kClamp));

    for (size_t i = 0; i < bars_.size(); ++i) {
        const float v  = std::clamp(bars_[i].spring.value(), 0.03f, 1.f);
        const float bh = std::max(barW, maxH * 2.f * v);
        const float x  = padX + slot * (float(i) + 0.5f) - barW * 0.5f;
        SkRect br = SkRect::MakeXYWH(x, midY - bh * 0.5f, barW, bh);
        canvas->drawRRect(SkRRect::MakeRectXY(br, barW * 0.5f, barW * 0.5f), p);
    }

    // Mic glyph on the left + hint text on the right, both fading with state.
    drawMicGlyph(canvas, padX * 0.50f, midY, alpha * 0.9f);
    if (state_ == PopupState::Idle) drawHint(canvas, w, midY, alpha * 0.55f);
}

void PopupView::drawMicGlyph(SkCanvas* canvas, float cx, float cy, float alpha) {
    const auto& P = Theme::get().p();
    SkPaint p; p.setAntiAlias(true);
    p.setColor(sk(P.textPrimary.withAlpha(alpha)));
    SkRect body = SkRect::MakeXYWH(cx - 2.2f, cy - 5.0f, 4.4f, 7.0f);
    canvas->drawRRect(SkRRect::MakeRectXY(body, 2.2f, 2.2f), p);
    p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(1.2f); p.setStrokeCap(SkPaint::kRound_Cap);
    SkPath arc; arc.addArc(SkRect::MakeXYWH(cx - 4.2f, cy - 2.8f, 8.4f, 8.4f), 0, 180);
    canvas->drawPath(arc, p);
    canvas->drawLine(cx, cy + 5.6f, cx, cy + 7.6f, p);
}

void PopupView::drawHint(SkCanvas* canvas, float w, float cy, float alpha) {
    const auto& P = Theme::get().p();
    SkFont font(typefaceUI_, 10.f);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    SkPaint p; p.setAntiAlias(true);
    p.setColor(sk(P.textSecondary.withAlpha(alpha)));
    const char* s = "Hold to talk";
    SkScalar tw = font.measureText(s, strlen(s), SkTextEncoding::kUTF8);
    canvas->drawSimpleText(s, strlen(s), SkTextEncoding::kUTF8,
                           w - 16.f - tw, cy + 3.5f, font, p);
}

void PopupView::drawCaption(SkCanvas* canvas, float w, float h, float alpha) {
    const auto& P = Theme::get().p();
    SkFont font(typefaceUI_, 11.f);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    SkPaint p; p.setAntiAlias(true);
    p.setColor(sk(P.textGhost.withAlpha(P.textGhost.a * alpha)));

    // Right-anchored tail of the partial so the newest words are always visible.
    std::string s = partial_;
    const float maxW = w - 36.f;
    while (!s.empty() && font.measureText(s.c_str(), s.size(), SkTextEncoding::kUTF8) > maxW)
        s.erase(0, 1);
    canvas->drawSimpleText(s.c_str(), s.size(), SkTextEncoding::kUTF8,
                           18.f, kBaseHeight + 13.f, font, p);
}

void PopupView::drawResult(SkCanvas* canvas, float w, float h, float alpha) {
    const auto& P  = Theme::get().p();
    const float t  = EaseCurve(motion::easeOutExpo)(checkTween_.raw());
    const float cy = kBaseHeight * 0.5f;
    const float cx = 20.f;

    // Ring
    SkPaint ring; ring.setAntiAlias(true); ring.setStyle(SkPaint::kStroke_Style);
    ring.setStrokeWidth(1.3f);
    ring.setColor(sk(P.success.withAlpha(0.55f * alpha * t)));
    canvas->drawCircle(cx, cy, 8.0f * (0.85f + 0.15f * t), ring);

    // Checkmark path, stroke-dash animated for a draw-on effect.
    SkPath check;
    check.moveTo(cx - 3.8f, cy + 0.2f);
    check.lineTo(cx - 1.2f, cy + 2.8f);
    check.lineTo(cx + 4.0f, cy - 3.0f);
    SkPaint cp; cp.setAntiAlias(true); cp.setStyle(SkPaint::kStroke_Style);
    cp.setStrokeWidth(1.6f); cp.setStrokeCap(SkPaint::kRound_Cap);
    cp.setStrokeJoin(SkPaint::kRound_Join);
    cp.setColor(sk(P.success.withAlpha(alpha)));
    const SkScalar len = 15.f;
    const SkScalar intervals[2] = { len * t, len };
    cp.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0));
    canvas->drawPath(check, cp);

    // Injected word count / preview
    SkFont font(typefaceUI_, 12.f);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    SkPaint tp; tp.setAntiAlias(true);
    tp.setColor(sk(P.textPrimary.withAlpha(alpha * t)));
    canvas->drawSimpleText(resultLabel_.c_str(), resultLabel_.size(),
                           SkTextEncoding::kUTF8, 36.f, cy + 4.f, font, tp);
}

void PopupView::drawCancelled(SkCanvas* canvas, float w, float h, float alpha) {
    const auto& P = Theme::get().p();
    SkFont font(typefaceUI_, 12.f);
    SkPaint p; p.setAntiAlias(true);
    p.setColor(sk(P.textSecondary.withAlpha(alpha)));
    const char* s = "Cancelled";
    SkScalar tw = font.measureText(s, strlen(s), SkTextEncoding::kUTF8);
    canvas->drawSimpleText(s, strlen(s), SkTextEncoding::kUTF8,
                           (w - tw) * 0.5f, kBaseHeight * 0.5f + 4.f, font, p);
}

} // namespace vox::ui
