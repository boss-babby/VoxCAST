// ============================================================================
//  VoxCast — ui/Theme.h
//  Design-token system. Every colour, radius, spacing unit, duration and
//  easing curve used by the renderer resolves through this header so that
//  theming (dark / light / auto) is a single switch, not a grep-and-replace.
// ============================================================================
#pragma once

#include <cstdint>
#include <array>
#include <string_view>

namespace vox::theme {

// ---------------------------------------------------------------------------
// Colour primitive. Stored premultiplied-ready as straight RGBA float.
// ---------------------------------------------------------------------------
struct Color {
    float r{}, g{}, b{}, a{1.f};

    static constexpr Color hex(uint32_t rgb, float alpha = 1.f) {
        return Color{ ((rgb >> 16) & 0xFF) / 255.f,
                      ((rgb >> 8)  & 0xFF) / 255.f,
                      ( rgb        & 0xFF) / 255.f,
                      alpha };
    }
    constexpr Color withAlpha(float alpha) const { return Color{r, g, b, alpha}; }
    constexpr uint32_t toSkColor() const {
        return (uint32_t(a * 255) << 24) | (uint32_t(r * 255) << 16) |
               (uint32_t(g * 255) << 8)  |  uint32_t(b * 255);
    }
    static Color lerp(const Color& x, const Color& y, float t) {
        return { x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t,
                 x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t };
    }
};

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
struct Palette {
    Color bg;             // window base
    Color surface;        // card / popup material (used *under* native blur)
    Color surfaceRaised;  // hovered card
    Color stroke;         // 1px hairline borders
    Color textPrimary;
    Color textSecondary;
    Color textGhost;      // streaming partial captions
    Color accentA;        // gradient stop 0
    Color accentB;        // gradient stop 1
    Color success;
    Color danger;
    float materialOpacity; // opacity applied over native blur backdrop
};

inline constexpr Palette kDark {
    /* bg             */ Color::hex(0x121214),
    /* surface        */ Color::hex(0x1A1A1F, 0.78f),
    /* surfaceRaised  */ Color::hex(0x24242B, 0.92f),
    /* stroke         */ Color::hex(0xFFFFFF, 0.08f),
    /* textPrimary    */ Color::hex(0xF4F4F7),
    /* textSecondary  */ Color::hex(0x9A9AA8),
    /* textGhost      */ Color::hex(0xF4F4F7, 0.42f),
    /* accentA        */ Color::hex(0x8B5CF6),   // violet-500
    /* accentB        */ Color::hex(0x22D3EE),   // cyan-400
    /* success        */ Color::hex(0x34D399),
    /* danger         */ Color::hex(0xF87171),
    /* materialOpacity*/ 0.82f
};

inline constexpr Palette kLight {
    Color::hex(0xF7F7FA),
    Color::hex(0xFFFFFF, 0.74f),
    Color::hex(0xFFFFFF, 0.94f),
    Color::hex(0x0B0B12, 0.08f),
    Color::hex(0x14141A),
    Color::hex(0x5D5D6E),
    Color::hex(0x14141A, 0.40f),
    Color::hex(0x7C3AED),
    Color::hex(0x0891B2),
    Color::hex(0x059669),
    Color::hex(0xDC2626),
    0.70f
};

// ---------------------------------------------------------------------------
// Metrics — 4pt spacing scale, capsule radii, type ramp
// ---------------------------------------------------------------------------
namespace space {
    inline constexpr float xs = 4.f,  sm = 8.f,  md = 12.f,
                           lg = 16.f, xl = 24.f, xxl = 32.f;
}
namespace radius {
    inline constexpr float sm = 8.f, md = 12.f, lg = 18.f, xl = 26.f, pill = 999.f;
}
namespace type {
    // Inter (Windows fallback: Segoe UI Variable, macOS fallback: SF Pro Text)
    inline constexpr std::string_view kFamily     = "Inter";
    inline constexpr std::string_view kFamilyMono = "JetBrains Mono";
    inline constexpr float display = 28.f, title = 20.f, body = 14.f,
                           caption = 12.f, micro = 11.f;
    inline constexpr float trackingTight = -0.014f; // em, applied to display/title
}

// ---------------------------------------------------------------------------
// Motion tokens (seconds)
// ---------------------------------------------------------------------------
namespace motion {
    inline constexpr float microInteraction = 0.150f;
    inline constexpr float popupEnter       = 0.220f;
    inline constexpr float popupExit        = 0.180f;
    inline constexpr float listHover        = 0.100f;
    inline constexpr float resultHold       = 0.900f;
    inline constexpr float breathCycle      = 2.500f;

    // cubic-bezier control points
    inline constexpr std::array<float,4> easeOutExpo { 0.16f, 1.f,  0.30f, 1.f  };
    inline constexpr std::array<float,4> easeInExpo  { 0.70f, 0.f,  0.84f, 0.f  };
    inline constexpr std::array<float,4> easeStandard{ 0.20f, 0.f,  0.00f, 1.f  };

    // Waveform bar spring (critically damped-ish, slight overshoot)
    inline constexpr float barStiffness = 420.f;
    inline constexpr float barDamping   = 30.f;
    inline constexpr float barMass      = 1.0f;
}

// ---------------------------------------------------------------------------
// Active theme resolution
// ---------------------------------------------------------------------------
enum class Mode { Dark, Light, Auto };

class Theme {
public:
    static Theme& get();
    void setMode(Mode m);
    Mode mode() const { return mode_; }
    const Palette& p() const { return *palette_; }
    /// Sampled accent gradient at t in [0,1].
    Color accentAt(float t) const { return Color::lerp(p().accentA, p().accentB, t); }
private:
    Mode mode_{Mode::Dark};
    const Palette* palette_{&kDark};
};

} // namespace vox::theme
