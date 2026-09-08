#pragma once
#include "Animation.h"
#include "Theme.h"
#include <include/core/SkRefCnt.h>
#include <include/core/SkTypeface.h>
#include <array>
#include <functional>
#include <string>

class SkCanvas;

namespace vox::ui {

enum class PopupState { Hidden, Idle, Listening, Processing, Result, Cancelled };

/// Pure view: owns no threads, no audio, no network. Fed by AppController.
class PopupView {
public:
    static constexpr int   kBars       = 16;
    static constexpr float kBaseWidth  = 200.f;
    static constexpr float kWideWidth  = 280.f;
    static constexpr float kBaseHeight = 38.f;

    PopupView();

    void setState(PopupState);
    PopupState state() const { return state_; }

    /// Called from the UI thread with data drained from the audio ring buffer.
    void pushAudioLevel(float rms, const float* spectrum = nullptr, size_t bins = 0);
    void setPartialTranscript(std::string);
    void setResultLabel(std::string s) { resultLabel_ = std::move(s); }
    void setTypeface(sk_sp<SkTypeface> tf) { typefaceUI_ = std::move(tf); }

    void update(float dt);              // vsync-driven
    void draw(SkCanvas*, float w, float h);

    std::function<void()> onDismissed;  // fires once the exit animation ends

private:
    void drawWaveform(SkCanvas*, float w, float h, float alpha);
    void drawCaption (SkCanvas*, float w, float h, float alpha);
    void drawResult  (SkCanvas*, float w, float h, float alpha);
    void drawCancelled(SkCanvas*, float w, float h, float alpha);
    void drawMicGlyph(SkCanvas*, float cx, float cy, float alpha);
    void drawHint    (SkCanvas*, float w, float cy, float alpha);

    struct Bar { anim::Spring<float> spring; float jitter{0.f}; };
    std::array<Bar, kBars> bars_{};

    anim::Spring<float> scale_, opacity_, width_, shake_, captionAlpha_{300.f, 30.f};
    anim::Tween checkTween_{0.42f, anim::EaseCurve(theme::motion::easeOutExpo)};

    PopupState state_{PopupState::Hidden}, prevState_{PopupState::Hidden};
    float time_{0.f}, stateTime_{0.f}, level_{0.f}, glow_{0.2f}, borderAngle_{0.f};
    std::string partial_, resultLabel_{"Inserted"};
    sk_sp<SkTypeface> typefaceUI_;
};

} // namespace vox::ui
