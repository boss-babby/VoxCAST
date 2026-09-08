// ============================================================================
//  VoxCast — audio/Vad.h
//  Energy + zero-crossing voice activity detector with an adaptive noise
//  floor, hysteresis and hang-over. Designed for 16 kHz mono, 20 ms frames
//  (320 samples). Cheap enough to run inline on the capture thread.
// ============================================================================
#pragma once

#include <cmath>
#include <cstddef>
#include <algorithm>

namespace vox::audio {

struct VadConfig {
    int   sampleRate        = 16000;
    int   frameSamples      = 320;    // 20 ms
    float onsetSnrDb        = 8.0f;   // speech starts when frame > floor + this
    float releaseSnrDb      = 4.0f;   // speech ends below floor + this (hysteresis)
    float hangoverMs        = 320.f;  // keep "speaking" this long after drop
    float minSpeechMs       = 140.f;  // reject clicks/keyboard taps
    float silenceStopMs     = 900.f;  // auto-stop in toggle mode
    float floorAttack       = 0.35f;  // fast adaptation when quieter
    float floorRelease      = 0.006f; // slow adaptation when louder
    float absoluteFloorDb   = -70.f;
};

struct VadFrameResult {
    bool  speech        = false;   // debounced speech decision
    bool  onset         = false;   // rising edge
    bool  endpoint      = false;   // silenceStopMs elapsed after speech
    float rms           = 0.f;     // linear 0..1
    float rmsDb         = -120.f;
    float noiseFloorDb  = -120.f;
    float zcr           = 0.f;     // zero-crossing rate 0..1
};

class Vad {
public:
    explicit Vad(VadConfig cfg = {}) : cfg_(cfg) { reset(); }

    void reset() {
        floorDb_ = -60.f; speaking_ = false; hangoverMs_ = 0.f;
        speechMs_ = 0.f; silenceMs_ = 0.f; sawSpeech_ = false;
    }
    const VadConfig& config() const { return cfg_; }
    void setConfig(const VadConfig& c) { cfg_ = c; }

    /// Process exactly one frame of mono float samples in [-1, 1].
    VadFrameResult process(const float* x, size_t n) {
        VadFrameResult r;
        if (!x || n == 0) return r;

        double acc = 0.0; size_t crossings = 0;
        for (size_t i = 0; i < n; ++i) {
            acc += double(x[i]) * double(x[i]);
            if (i && ((x[i] < 0.f) != (x[i-1] < 0.f))) ++crossings;
        }
        r.rms   = float(std::sqrt(acc / double(n)));
        r.rmsDb = std::max(cfg_.absoluteFloorDb, 20.f * std::log10(std::max(r.rms, 1e-7f)));
        r.zcr   = float(crossings) / float(n ? n - 1 : 1);

        // Adaptive noise floor: snaps down quickly, creeps up slowly.
        const float a = (r.rmsDb < floorDb_) ? cfg_.floorAttack : cfg_.floorRelease;
        floorDb_ += a * (r.rmsDb - floorDb_);
        r.noiseFloorDb = floorDb_;

        const float frameMs = 1000.f * float(n) / float(cfg_.sampleRate);
        const bool  loudOn  = r.rmsDb > floorDb_ + cfg_.onsetSnrDb;
        const bool  loudOff = r.rmsDb > floorDb_ + cfg_.releaseSnrDb;
        // Voiced speech has moderate ZCR; > 0.45 is usually hiss/fricative noise.
        const bool  plausible = r.zcr < 0.48f;

        if (!speaking_) {
            if (loudOn && plausible) {
                speechMs_ += frameMs;
                if (speechMs_ >= cfg_.minSpeechMs) {
                    speaking_ = true; r.onset = true; sawSpeech_ = true;
                    hangoverMs_ = cfg_.hangoverMs; silenceMs_ = 0.f;
                }
            } else {
                speechMs_ = 0.f;
            }
        } else {
            if (loudOff) { hangoverMs_ = cfg_.hangoverMs; silenceMs_ = 0.f; }
            else {
                hangoverMs_ -= frameMs;
                if (hangoverMs_ <= 0.f) { speaking_ = false; speechMs_ = 0.f; }
            }
        }

        if (!speaking_ && sawSpeech_) {
            silenceMs_ += frameMs;
            if (silenceMs_ >= cfg_.silenceStopMs) { r.endpoint = true; sawSpeech_ = false; }
        }
        r.speech = speaking_;
        return r;
    }

private:
    VadConfig cfg_;
    float floorDb_{-60.f}, hangoverMs_{0.f}, speechMs_{0.f}, silenceMs_{0.f};
    bool  speaking_{false}, sawSpeech_{false};
};

} // namespace vox::audio
