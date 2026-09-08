// ============================================================================
//  VoxCast — tools/render_frames.cpp
//  Offscreen harness: drives the REAL ui/PopupView through its full state
//  machine at a fixed timestep and writes PNG frames. Used to verify the
//  renderer without a display, and to generate the filmstrips in the docs.
//
//  Usage: render_frames <outdir> [--fps 60] [--scale 2]
// ============================================================================
#include "ui/PopupView.h"
#include "ui/Theme.h"

#include <include/core/SkCanvas.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vox;

namespace {

struct Shot { const char* name; float atSec; };

/// A deterministic, speech-like amplitude envelope so frames are reproducible
/// (syllable bursts at ~4.5 Hz under a slow phrase contour, plus micro noise).
float fakeSpeechRms(float t) {
    float syllable = std::pow(std::fabs(std::sin(t * 14.1f)), 2.2f);
    float phrase   = 0.55f + 0.45f * std::sin(t * 1.7f);
    float micro    = 0.06f * std::sin(t * 61.f);
    float breath   = (std::fmod(t, 2.6f) > 2.2f) ? 0.12f : 1.f;   // pause
    return std::fmax(0.f, (0.055f + 0.10f * syllable * phrase + micro) * breath);
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir = (argc > 1) ? argv[1] : ".";
    int   fps   = 60;
    float scale = 2.f;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fps") && i + 1 < argc)   fps = std::atoi(argv[++i]);
        if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = float(std::atof(argv[++i]));
    }

    theme::Theme::get().setMode(theme::Mode::Dark);

    const int W = int(480 * scale), H = int(170 * scale);
    const float dt = 1.f / float(fps);

    ui::PopupView popup;
    popup.setTypeface(SkTypeface::MakeDefault());

    // ---- Scripted session: idle → listening → processing → result ---------
    struct Step { float at; int kind; const char* text; };
    const std::vector<Step> script = {
        { 0.00f, 0, nullptr },                          // idle
        { 0.60f, 1, nullptr },                          // listening
        { 1.35f, 3, "hey team" },
        { 2.05f, 3, "hey team um the deploy is" },
        { 2.75f, 3, "hey team um the deploy is green comma ship it period" },
        { 3.40f, 2, nullptr },                          // processing
        { 4.30f, 4, "9 words - Chat" },                 // result
        { 5.40f, 5, nullptr },                          // hidden
    };

    // Frames we want as stills for the docs.
    const std::vector<Shot> shots = {
        { "01-idle",        0.35f },
        { "02-listening",   1.15f },
        { "03-captions",    2.90f },
        { "04-processing",  3.85f },
        { "05-result",      4.65f },
    };

    const float duration = 5.9f;
    const int   total    = int(duration * float(fps));
    size_t nextStep = 0, nextShot = 0;
    int written = 0;

    SkBitmapSurface surface(W, H);
    SkCanvas canvas(&surface);

    for (int f = 0; f < total; ++f) {
        const float t = float(f) * dt;

        while (nextStep < script.size() && t >= script[nextStep].at) {
            const Step& s = script[nextStep++];
            switch (s.kind) {
                case 0: popup.setState(ui::PopupState::Idle); break;
                case 1: popup.setState(ui::PopupState::Listening); break;
                case 2: popup.setPartialTranscript({});
                        popup.setState(ui::PopupState::Processing); break;
                case 3: popup.setPartialTranscript(s.text); break;
                case 4: popup.setResultLabel(s.text);
                        popup.setState(ui::PopupState::Result); break;
                default: popup.setState(ui::PopupState::Hidden); break;
            }
        }

        popup.pushAudioLevel(popup.state() == ui::PopupState::Listening
                             ? fakeSpeechRms(t - 0.6f) : 0.f);
        popup.update(dt);

        // Scale the whole scene for crisp 2x output.
        canvas.clear(SK_ColorTRANSPARENT);
        canvas.save();
        canvas.scale(scale, scale);
        popup.draw(&canvas, 480.f, 170.f);
        canvas.restore();

        if (nextShot < shots.size() && t >= shots[nextShot].atSec) {
            std::string p = outDir + "/" + shots[nextShot].name + ".png";
            if (SkWritePNG(surface, p.c_str())) {
                std::printf("  wrote %-28s  t=%.2fs\n", shots[nextShot].name, t);
                ++written;
            } else {
                std::printf("  FAILED %s\n", p.c_str());
            }
            ++nextShot;
        }
    }

    std::printf("rendered %d frames at %dfps (%dx%d), %d stills written\n",
                total, fps, W, H, written);
    return written == int(shots.size()) ? 0 : 1;
}
