// ============================================================================
//  mini-Skia — a source-compatible subset of the Skia API
//
//  WHY THIS EXISTS
//  Building real Skia requires depot_tools, GN and ~20 minutes. To let VoxCast
//  build and run end-to-end anywhere (including cross-compiled under
//  MinGW + Wine), this header implements the *exact* subset of the Skia
//  surface that ui/PopupView.cpp consumes, backed by an analytic
//  signed-distance-field software rasteriser.
//
//  ui/PopupView.cpp is compiled VERBATIM against this. Set SKIA_ROOT in CMake
//  and VOXCAST_HAS_SKIA=1 to link the real GPU Skia instead — not one line of
//  PopupView.cpp changes.
//
//  Implemented: rounded rects, circles, capsule strokes, polyline paths, arcs,
//  linear/radial/sweep gradients, separable blur image filter, dash path
//  effect, rrect clipping, save/restore with an affine CTM, and a built-in
//  5x7 text face with bilinear coverage sampling.
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// ---------------------------------------------------------------- sk_sp -----
template <typename T>
class sk_sp {
public:
    sk_sp() = default;
    sk_sp(std::nullptr_t) {}
    explicit sk_sp(T* p) : p_(p) {}
    sk_sp(const sk_sp&) = default;
    sk_sp(sk_sp&&) noexcept = default;
    sk_sp& operator=(const sk_sp&) = default;
    sk_sp& operator=(sk_sp&&) noexcept = default;
    template <typename U> sk_sp(const sk_sp<U>& o) : p_(o.raw()) {}
    T* get() const { return p_.get(); }
    T* raw() const { return p_.get(); }
    std::shared_ptr<T> shared() const { return p_; }
    T& operator*() const { return *p_; }
    T* operator->() const { return p_.get(); }
    explicit operator bool() const { return bool(p_); }
    static sk_sp adopt(std::shared_ptr<T> s) { sk_sp r; r.p_ = std::move(s); return r; }
private:
    std::shared_ptr<T> p_;
};
template <typename T, typename... A> sk_sp<T> sk_make_sp(A&&... a) {
    return sk_sp<T>::adopt(std::make_shared<T>(std::forward<A>(a)...));
}

// -------------------------------------------------------------- scalars -----
using SkScalar = float;
using SkColor  = uint32_t;

static inline constexpr SkColor SkColorSetARGB(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}
static inline constexpr uint8_t SkColorGetA(SkColor c) { return uint8_t(c >> 24); }
static inline constexpr uint8_t SkColorGetR(SkColor c) { return uint8_t(c >> 16); }
static inline constexpr uint8_t SkColorGetG(SkColor c) { return uint8_t(c >> 8);  }
static inline constexpr uint8_t SkColorGetB(SkColor c) { return uint8_t(c);       }

inline constexpr SkColor SK_ColorTRANSPARENT = 0x00000000u;
inline constexpr SkColor SK_ColorBLACK       = 0xFF000000u;
inline constexpr SkColor SK_ColorWHITE       = 0xFFFFFFFFu;

enum class SkTileMode { kClamp, kRepeat, kMirror, kDecal };
enum class SkTextEncoding { kUTF8, kUTF16, kUTF32, kGlyphID };

// ------------------------------------------------------- geometry types -----
struct SkPoint {
    SkScalar fX{0}, fY{0};
    static SkPoint Make(SkScalar x, SkScalar y) { return {x, y}; }
    SkScalar x() const { return fX; }
    SkScalar y() const { return fY; }
};

struct SkRect {
    SkScalar fLeft{0}, fTop{0}, fRight{0}, fBottom{0};
    static SkRect MakeWH(SkScalar w, SkScalar h) { return {0, 0, w, h}; }
    static SkRect MakeXYWH(SkScalar x, SkScalar y, SkScalar w, SkScalar h) {
        return {x, y, x + w, y + h};
    }
    static SkRect MakeLTRB(SkScalar l, SkScalar t, SkScalar r, SkScalar b) {
        return {l, t, r, b};
    }
    SkScalar width()  const { return fRight - fLeft; }
    SkScalar height() const { return fBottom - fTop; }
    SkScalar centerX() const { return (fLeft + fRight) * 0.5f; }
    SkScalar centerY() const { return (fTop + fBottom) * 0.5f; }
    SkRect makeOutset(SkScalar dx, SkScalar dy) const {
        return {fLeft - dx, fTop - dy, fRight + dx, fBottom + dy};
    }
    SkRect makeOffset(SkScalar dx, SkScalar dy) const {
        return {fLeft + dx, fTop + dy, fRight + dx, fBottom + dy};
    }
    void join(const SkRect& o) {
        fLeft = std::fmin(fLeft, o.fLeft);   fTop = std::fmin(fTop, o.fTop);
        fRight = std::fmax(fRight, o.fRight); fBottom = std::fmax(fBottom, o.fBottom);
    }
};

class SkRRect {
public:
    static SkRRect MakeRectXY(const SkRect& r, SkScalar rx, SkScalar ry) {
        SkRRect o; o.rect_ = r;
        SkScalar mx = std::fmin(r.width(), r.height()) * 0.5f;
        o.rx_ = std::fmin(rx, mx); o.ry_ = std::fmin(ry, mx);
        return o;
    }
    static SkRRect MakeRect(const SkRect& r) { return MakeRectXY(r, 0, 0); }
    SkRRect makeOffset(SkScalar dx, SkScalar dy) const {
        SkRRect o = *this; o.rect_ = rect_.makeOffset(dx, dy); return o;
    }
    const SkRect& rect() const { return rect_; }
    SkScalar rx() const { return rx_; }
    SkScalar ry() const { return ry_; }
private:
    SkRect rect_{}; SkScalar rx_{0}, ry_{0};
};

// ------------------------------------------------------------- SkMatrix -----
class SkMatrix {
public:
    SkMatrix() { setIdentity(); }
    void setIdentity() { a_ = 1; b_ = 0; c_ = 0; d_ = 1; tx_ = 0; ty_ = 0; }
    void setRotate(SkScalar deg, SkScalar px, SkScalar py) {
        SkScalar r = deg * 0.01745329252f, s = std::sin(r), co = std::cos(r);
        a_ = co; b_ = s; c_ = -s; d_ = co;
        tx_ = px - (co * px - s * py);
        ty_ = py - (s * px + co * py);
    }
    void setTranslate(SkScalar x, SkScalar y) { setIdentity(); tx_ = x; ty_ = y; }
    void setScale(SkScalar sx, SkScalar sy) { setIdentity(); a_ = sx; d_ = sy; }
    SkPoint mapPoint(SkPoint p) const {
        return { a_ * p.fX + c_ * p.fY + tx_, b_ * p.fX + d_ * p.fY + ty_ };
    }
    /// Concatenation: result = this * m (apply m first, then this).
    SkMatrix concat(const SkMatrix& m) const {
        SkMatrix r;
        r.a_ = a_ * m.a_ + c_ * m.b_;   r.b_ = b_ * m.a_ + d_ * m.b_;
        r.c_ = a_ * m.c_ + c_ * m.d_;   r.d_ = b_ * m.c_ + d_ * m.d_;
        r.tx_ = a_ * m.tx_ + c_ * m.ty_ + tx_;
        r.ty_ = b_ * m.tx_ + d_ * m.ty_ + ty_;
        return r;
    }
    bool invert(SkMatrix* out) const {
        SkScalar det = a_ * d_ - b_ * c_;
        if (std::fabs(det) < 1e-9f) return false;
        SkScalar id = 1.f / det;
        out->a_ =  d_ * id; out->b_ = -b_ * id;
        out->c_ = -c_ * id; out->d_ =  a_ * id;
        out->tx_ = (c_ * ty_ - d_ * tx_) * id;
        out->ty_ = (b_ * tx_ - a_ * ty_) * id;
        return true;
    }
    /// Mean linear scale factor — used to keep stroke widths correct.
    SkScalar meanScale() const {
        return std::sqrt(std::fabs(a_ * d_ - b_ * c_));
    }
    SkScalar a_{1}, b_{0}, c_{0}, d_{1}, tx_{0}, ty_{0};
};

// ------------------------------------------------------------- shading -----
struct SkRGBA { float r{0}, g{0}, b{0}, a{0}; };

inline SkRGBA unpack(SkColor c) {
    return { SkColorGetR(c) / 255.f, SkColorGetG(c) / 255.f,
             SkColorGetB(c) / 255.f, SkColorGetA(c) / 255.f };
}

class SkShader {
public:
    enum class Kind { Linear, Radial, Sweep };
    virtual ~SkShader() = default;
    /// Evaluate in *local* (pre-CTM) space.
    SkRGBA eval(SkScalar x, SkScalar y) const;

    Kind kind{Kind::Linear};
    SkPoint p0{}, p1{};
    SkScalar radius{1};
    std::vector<SkRGBA> colors;
    std::vector<SkScalar> stops;
    SkTileMode tile{SkTileMode::kClamp};
    SkMatrix localInverse{};
    bool hasLocal{false};
};

class SkGradientShader {
public:
    static sk_sp<SkShader> MakeLinear(const SkPoint pts[2], const SkColor colors[],
                                      const SkScalar pos[], int count,
                                      SkTileMode mode, uint32_t flags = 0,
                                      const SkMatrix* local = nullptr);
    static sk_sp<SkShader> MakeRadial(SkPoint center, SkScalar radius,
                                      const SkColor colors[], const SkScalar pos[],
                                      int count, SkTileMode mode, uint32_t flags = 0,
                                      const SkMatrix* local = nullptr);
    static sk_sp<SkShader> MakeSweep(SkScalar cx, SkScalar cy,
                                     const SkColor colors[], const SkScalar pos[],
                                     int count, uint32_t flags = 0,
                                     const SkMatrix* local = nullptr);
};

// ------------------------------------------------------- filters/effects ----
class SkImageFilter {
public:
    virtual ~SkImageFilter() = default;
    SkScalar sigmaX{0}, sigmaY{0};
};
class SkImageFilters {
public:
    static sk_sp<SkImageFilter> Blur(SkScalar sx, SkScalar sy, sk_sp<SkImageFilter> = nullptr);
};

class SkPathEffect {
public:
    virtual ~SkPathEffect() = default;
    std::vector<SkScalar> intervals;
    SkScalar phase{0};
};
class SkDashPathEffect {
public:
    static sk_sp<SkPathEffect> Make(const SkScalar intervals[], int count, SkScalar phase);
};

// ---------------------------------------------------------------- paint -----
class SkPaint {
public:
    enum Style { kFill_Style, kStroke_Style, kStrokeAndFill_Style };
    enum Cap   { kButt_Cap, kRound_Cap, kSquare_Cap };
    enum Join  { kMiter_Join, kRound_Join, kBevel_Join };

    void setAntiAlias(bool a) { aa_ = a; }
    bool isAntiAlias() const { return aa_; }
    void setColor(SkColor c) { color_ = c; }
    SkColor getColor() const { return color_; }
    void setStyle(Style s) { style_ = s; }
    Style getStyle() const { return style_; }
    void setStrokeWidth(SkScalar w) { strokeW_ = w; }
    SkScalar getStrokeWidth() const { return strokeW_; }
    void setStrokeCap(Cap c) { cap_ = c; }
    Cap getStrokeCap() const { return cap_; }
    void setStrokeJoin(Join j) { join_ = j; }
    Join getStrokeJoin() const { return join_; }
    void setShader(sk_sp<SkShader> s) { shader_ = std::move(s); }
    const sk_sp<SkShader>& getShader() const { return shader_; }
    void setImageFilter(sk_sp<SkImageFilter> f) { filter_ = std::move(f); }
    const sk_sp<SkImageFilter>& getImageFilter() const { return filter_; }
    void setPathEffect(sk_sp<SkPathEffect> e) { effect_ = std::move(e); }
    const sk_sp<SkPathEffect>& getPathEffect() const { return effect_; }
private:
    bool aa_{false};
    SkColor color_{SK_ColorBLACK};
    Style style_{kFill_Style};
    Cap cap_{kButt_Cap};
    Join join_{kMiter_Join};
    SkScalar strokeW_{1};
    sk_sp<SkShader> shader_;
    sk_sp<SkImageFilter> filter_;
    sk_sp<SkPathEffect> effect_;
};

// ----------------------------------------------------------------- path -----
class SkPath {
public:
    void moveTo(SkScalar x, SkScalar y);
    void lineTo(SkScalar x, SkScalar y);
    void close();
    /// Elliptical arc flattened into a polyline (Skia semantics: degrees, CW+).
    void addArc(const SkRect& oval, SkScalar startDeg, SkScalar sweepDeg);
    void addCircle(SkScalar cx, SkScalar cy, SkScalar r);
    void reset() { contours_.clear(); }
    bool isEmpty() const { return contours_.empty(); }

    struct Contour { std::vector<SkPoint> pts; bool closed{false}; };
    const std::vector<Contour>& contours() const { return contours_; }
    SkRect bounds() const;
private:
    std::vector<Contour> contours_;
};

// ------------------------------------------------------------- typeface -----
class SkTypeface {
public:
    static sk_sp<SkTypeface> MakeDefault() { return sk_make_sp<SkTypeface>(); }
    static sk_sp<SkTypeface> MakeFromName(const char*, int = 0) { return MakeDefault(); }
};

class SkFont {
public:
    enum class Edging { kAlias, kAntiAlias, kSubpixelAntiAlias };
    SkFont() = default;
    SkFont(sk_sp<SkTypeface> tf, SkScalar size) : tf_(std::move(tf)), size_(size) {}
    void setEdging(Edging e) { edging_ = e; }
    void setSize(SkScalar s) { size_ = s; }
    SkScalar getSize() const { return size_; }
    SkScalar measureText(const void* text, size_t byteLength, SkTextEncoding) const;
private:
    friend class SkCanvas;
    sk_sp<SkTypeface> tf_;
    SkScalar size_{12};
    Edging edging_{Edging::kAntiAlias};
};

// --------------------------------------------------------------- surface ----
/// 32-bit premultiplied BGRA, top-down — directly consumable by
/// UpdateLayeredWindow() on Win32 and by the PNG writer.
class SkBitmapSurface {
public:
    SkBitmapSurface(int w, int h) : w_(w), h_(h), px_(size_t(w) * size_t(h), 0u) {}
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t* pixels() { return px_.data(); }
    const uint32_t* pixels() const { return px_.data(); }
    size_t rowBytes() const { return size_t(w_) * 4; }
private:
    int w_, h_;
    std::vector<uint32_t> px_;
};

// ---------------------------------------------------------------- canvas ----
class SkCanvas {
public:
    explicit SkCanvas(SkBitmapSurface* s) : surf_(s) { stack_.push_back(State{}); }

    void clear(SkColor c);
    int  save();
    void restore();
    void translate(SkScalar dx, SkScalar dy);
    void scale(SkScalar sx, SkScalar sy);
    void rotate(SkScalar deg);
    void concat(const SkMatrix& m);

    void clipRRect(const SkRRect& r, bool doAntiAlias);
    void clipRect(const SkRect& r, bool doAntiAlias);

    void drawRRect(const SkRRect& r, const SkPaint& p);
    void drawRect(const SkRect& r, const SkPaint& p);
    void drawCircle(SkScalar cx, SkScalar cy, SkScalar radius, const SkPaint& p);
    void drawLine(SkScalar x0, SkScalar y0, SkScalar x1, SkScalar y1, const SkPaint& p);
    void drawPath(const SkPath& path, const SkPaint& p);
    void drawSimpleText(const void* text, size_t byteLength, SkTextEncoding,
                        SkScalar x, SkScalar y, const SkFont& font, const SkPaint& p);

private:
    struct State {
        SkMatrix ctm;
        bool     hasClip{false};
        SkRRect  clip;
        SkMatrix clipCtmInverse;   // device -> clip-local
    };
    // Coverage callback: device pixel -> [0,1] inside-ness.
    template <typename SdfFn>
    void rasterise(const SkRect& localBounds, const SkPaint&, SdfFn sdfLocal);
    void blitLayer(const SkBitmapSurface& layer, SkScalar sigmaX, SkScalar sigmaY);

    SkBitmapSurface* surf_;
    std::vector<State> stack_;
};

// ------------------------------------------------------------ PNG output ----
/// Zero-dependency PNG writer (stored/uncompressed deflate + CRC32).
bool SkWritePNG(const SkBitmapSurface&, const char* path);
