// ============================================================================
//  VoxCast — ui/Animation.h
//  Frame-rate independent animation primitives: analytic critically-damped
//  springs, cubic-bezier easing with Newton-Raphson inversion, and a tiny
//  tween driver. Nothing in the UI is allowed to lerp linearly.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <array>

namespace vox::anim {

// ---------------------------------------------------------------------------
// Spring<T> — semi-implicit Euler with sub-stepping so behaviour is identical
// at 60 / 120 / 240 Hz. Deterministic, unit-tested (see tests/test_spring.cpp).
// ---------------------------------------------------------------------------
template <typename T = float>
class Spring {
public:
    Spring() = default;
    Spring(float stiffness, float damping, float mass = 1.f)
        : k_(stiffness), c_(damping), m_(mass) {}

    void configure(float stiffness, float damping, float mass = 1.f) {
        k_ = stiffness; c_ = damping; m_ = mass;
    }
    void snap(T v)      { value_ = v; target_ = v; velocity_ = T{}; }
    void setTarget(T v) { target_ = v; }
    T    value() const  { return value_; }
    T    target() const { return target_; }
    T    velocity() const { return velocity_; }

    bool settled(float epsilon = 1e-3f) const {
        return std::abs(float(target_ - value_)) < epsilon &&
               std::abs(float(velocity_))        < epsilon;
    }

    T update(float dt) {
        // Clamp pathological frame spikes (alt-tab, debugger break).
        dt = std::clamp(dt, 0.f, 0.100f);
        constexpr float kMaxStep = 1.f / 240.f;
        int steps = std::max(1, int(std::ceil(dt / kMaxStep)));
        float h = dt / float(steps);
        for (int i = 0; i < steps; ++i) {
            T   dx    = value_ - target_;
            T   force = -k_ * dx - c_ * velocity_;
            velocity_ = velocity_ + (force / m_) * h;
            value_    = value_ + velocity_ * h;
        }
        return value_;
    }

private:
    float k_{380.f}, c_{28.f}, m_{1.f};
    T value_{}, target_{}, velocity_{};
};

// ---------------------------------------------------------------------------
// Cubic bezier easing (CSS-compatible), solved for x then evaluated for y.
// ---------------------------------------------------------------------------
class EaseCurve {
public:
    constexpr EaseCurve(float x1, float y1, float x2, float y2)
        : x1_(x1), y1_(y1), x2_(x2), y2_(y2) {}
    constexpr explicit EaseCurve(const std::array<float,4>& c)
        : EaseCurve(c[0], c[1], c[2], c[3]) {}

    float operator()(float t) const {
        t = std::clamp(t, 0.f, 1.f);
        if (x1_ == y1_ && x2_ == y2_) return t;   // identity == linear
        return sampleY(solveX(t));
    }

private:
    static constexpr float A(float a1, float a2) { return 1.f - 3.f * a2 + 3.f * a1; }
    static constexpr float B(float a1, float a2) { return 3.f * a2 - 6.f * a1; }
    static constexpr float C(float a1)           { return 3.f * a1; }

    float sampleX(float t) const { return ((A(x1_,x2_)*t + B(x1_,x2_))*t + C(x1_))*t; }
    float sampleY(float t) const { return ((A(y1_,y2_)*t + B(y1_,y2_))*t + C(y1_))*t; }
    float slopeX(float t) const  { return 3.f*A(x1_,x2_)*t*t + 2.f*B(x1_,x2_)*t + C(x1_); }

    float solveX(float x) const {
        float t = x;
        for (int i = 0; i < 8; ++i) {               // Newton-Raphson
            float d = slopeX(t);
            if (std::abs(d) < 1e-6f) break;
            float e = sampleX(t) - x;
            if (std::abs(e) < 1e-7f) return t;
            t -= e / d;
        }
        float lo = 0.f, hi = 1.f; t = x;            // bisection fallback
        for (int i = 0; i < 20; ++i) {
            float e = sampleX(t) - x;
            if (std::abs(e) < 1e-7f) break;
            (e > 0.f ? hi : lo) = t;
            t = (lo + hi) * 0.5f;
        }
        return t;
    }
    float x1_, y1_, x2_, y2_;
};

// ---------------------------------------------------------------------------
// Tween — a 0..1 progress driver with an easing curve and completion callback.
// ---------------------------------------------------------------------------
class Tween {
public:
    Tween(float duration, EaseCurve curve) : dur_(duration), curve_(curve) {}
    void restart()          { elapsed_ = 0.f; running_ = true; }
    void finish()           { elapsed_ = dur_; running_ = false; }
    bool running() const    { return running_; }
    float raw() const       { return dur_ <= 0.f ? 1.f : std::clamp(elapsed_/dur_, 0.f, 1.f); }
    float eased() const     { return curve_(raw()); }
    bool update(float dt) {                        // returns true on completion frame
        if (!running_) return false;
        elapsed_ += dt;
        if (elapsed_ >= dur_) { elapsed_ = dur_; running_ = false; return true; }
        return false;
    }
private:
    float dur_, elapsed_{0.f};
    bool running_{false};
    EaseCurve curve_;
};

// Handy periodic helpers -----------------------------------------------------
inline float breathe(float timeSec, float cycleSec) {   // 0..1 sine, ease-in-out
    return 0.5f - 0.5f * std::cos(6.2831853f * timeSec / cycleSec);
}
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

} // namespace vox::anim
