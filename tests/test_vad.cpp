#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "audio/Vad.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace vox::audio;

static std::vector<float> tone(int n, float freq, float amp, int sr = 16000) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i) v[i] = amp * std::sin(6.28318f * freq * i / sr);
    return v;
}
static std::vector<float> noise(int n, float amp) {
    std::vector<float> v(n);
    uint32_t s = 12345;
    for (int i = 0; i < n; ++i) { s = s * 1664525u + 1013904223u;
        v[i] = amp * ((float(s >> 8 & 0xFFFF) / 32768.f) - 1.f); }
    return v;
}

TEST_CASE("VAD stays silent on low-level noise", "[vad]") {
    Vad vad;
    auto n = noise(320, 0.002f);
    bool any = false;
    for (int f = 0; f < 60; ++f) any |= vad.process(n.data(), n.size()).speech;
    REQUIRE_FALSE(any);
}

TEST_CASE("VAD detects speech-like tone after minSpeechMs", "[vad]") {
    VadConfig cfg; cfg.minSpeechMs = 100.f;
    Vad vad(cfg);
    auto quiet = noise(320, 0.001f);
    for (int f = 0; f < 40; ++f) vad.process(quiet.data(), quiet.size());  // learn floor

    auto voice = tone(320, 180.f, 0.28f);
    bool onset = false;
    int frames = 0;
    for (; frames < 20 && !onset; ++frames) onset = vad.process(voice.data(), voice.size()).onset;
    REQUIRE(onset);
    REQUIRE(frames <= 8);            // ≤160 ms latency
}

TEST_CASE("VAD reports endpoint after configured silence", "[vad]") {
    VadConfig cfg; cfg.minSpeechMs = 60.f; cfg.silenceStopMs = 400.f; cfg.hangoverMs = 100.f;
    Vad vad(cfg);
    auto quiet = noise(320, 0.001f);
    auto voice = tone(320, 200.f, 0.30f);
    for (int f = 0; f < 40; ++f) vad.process(quiet.data(), quiet.size());
    for (int f = 0; f < 25; ++f) vad.process(voice.data(), voice.size());

    bool endpoint = false;
    for (int f = 0; f < 60 && !endpoint; ++f)
        endpoint = vad.process(quiet.data(), quiet.size()).endpoint;
    REQUIRE(endpoint);
}

TEST_CASE("VAD rms/db conversion is sane", "[vad]") {
    Vad vad;
    std::vector<float> half(320, 0.5f);
    auto r = vad.process(half.data(), half.size());
    REQUIRE(r.rms == Catch::Approx(0.5f).margin(1e-4));
    REQUIRE(r.rmsDb == Catch::Approx(-6.02f).margin(0.1));
}

TEST_CASE("High zero-crossing hiss is rejected", "[vad]") {
    Vad vad;
    std::vector<float> alternating(320);
    for (size_t i = 0; i < alternating.size(); ++i) alternating[i] = (i % 2) ? 0.3f : -0.3f;
    bool speech = false;
    for (int f = 0; f < 30; ++f) speech |= vad.process(alternating.data(), alternating.size()).speech;
    REQUIRE_FALSE(speech);
}
