#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ui/Animation.h"

using namespace vox::anim;

TEST_CASE("Spring converges to target and settles", "[anim]") {
    Spring<float> s(420.f, 30.f);
    s.snap(0.f); s.setTarget(1.f);
    for (int i = 0; i < 600; ++i) s.update(1.f / 120.f);
    REQUIRE(s.value() == Catch::Approx(1.f).margin(1e-3));
    REQUIRE(s.settled());
}

TEST_CASE("Spring is frame-rate independent", "[anim]") {
    Spring<float> a(420.f, 30.f), b(420.f, 30.f);
    a.snap(0.f); b.snap(0.f); a.setTarget(1.f); b.setTarget(1.f);
    for (int i = 0; i < 60; ++i)  a.update(1.f / 60.f);
    for (int i = 0; i < 240; ++i) b.update(1.f / 240.f);
    REQUIRE(a.value() == Catch::Approx(b.value()).margin(0.01));
}

TEST_CASE("Spring clamps pathological dt", "[anim]") {
    Spring<float> s(900.f, 20.f);
    s.snap(0.f); s.setTarget(1.f);
    s.update(5.0f);                       // debugger pause
    REQUIRE(std::isfinite(s.value()));
    REQUIRE(std::abs(s.value()) < 4.f);   // no explosion
}

TEST_CASE("EaseCurve endpoints and monotonicity", "[anim]") {
    EaseCurve e(0.16f, 1.f, 0.30f, 1.f);
    REQUIRE(e(0.f) == Catch::Approx(0.f).margin(1e-4));
    REQUIRE(e(1.f) == Catch::Approx(1.f).margin(1e-4));
    float prev = -1.f;
    for (int i = 0; i <= 100; ++i) { float v = e(i / 100.f); REQUIRE(v >= prev - 1e-4f); prev = v; }
    REQUIRE(e(0.25f) > 0.5f);             // easeOutExpo front-loads
}

TEST_CASE("Linear control points behave linearly", "[anim]") {
    EaseCurve lin(0.f, 0.f, 1.f, 1.f);
    REQUIRE(lin(0.37f) == Catch::Approx(0.37f).margin(1e-3));
}

TEST_CASE("breathe() is a bounded sine", "[anim]") {
    for (int i = 0; i < 200; ++i) {
        float v = breathe(i * 0.05f, 2.5f);
        REQUIRE(v >= -1e-5f); REQUIRE(v <= 1.f + 1e-5f);
    }
    REQUIRE(breathe(0.f, 2.5f) == Catch::Approx(0.f).margin(1e-5));
    REQUIRE(breathe(1.25f, 2.5f) == Catch::Approx(1.f).margin(1e-5));
}

TEST_CASE("Tween completes exactly once", "[anim]") {
    Tween t(0.2f, EaseCurve(0.16f, 1.f, 0.3f, 1.f));
    t.restart();
    int completions = 0;
    for (int i = 0; i < 60; ++i) if (t.update(1.f / 60.f)) ++completions;
    REQUIRE(completions == 1);
    REQUIRE(t.raw() == Catch::Approx(1.f));
}
