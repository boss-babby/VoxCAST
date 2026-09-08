// ============================================================================
//  mini-Skia implementation — analytic SDF software rasteriser.
//
//  Every primitive PopupView.cpp draws reduces to a signed distance field in
//  local space. Coverage = smoothstep across one device pixel of the distance,
//  which gives clean analytic anti-aliasing with no supersampling.
// ============================================================================
#include "MiniSkia.h"

#include <algorithm>
#include <cstdio>

// ============================================================================
// Shading
// ============================================================================
static SkRGBA sampleStops(const std::vector<SkRGBA>& c,
                          const std::vector<SkScalar>& s, float t) {
    if (c.empty()) return {};
    if (c.size() == 1) return c[0];
    t = std::clamp(t, 0.f, 1.f);
    for (size_t i = 1; i < c.size(); ++i) {
        if (t <= s[i]) {
            float span = s[i] - s[i - 1];
            float u = span > 1e-6f ? (t - s[i - 1]) / span : 0.f;
            const SkRGBA &a = c[i - 1], &b = c[i];
            return { a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u,
                     a.b + (b.b - a.b) * u, a.a + (b.a - a.a) * u };
        }
    }
    return c.back();
}

SkRGBA SkShader::eval(SkScalar x, SkScalar y) const {
    SkPoint p{x, y};
    if (hasLocal) p = localInverse.mapPoint(p);
    float t = 0.f;
    switch (kind) {
        case Kind::Linear: {
            float dx = p1.fX - p0.fX, dy = p1.fY - p0.fY;
            float len2 = dx * dx + dy * dy;
            t = len2 > 1e-9f ? ((p.fX - p0.fX) * dx + (p.fY - p0.fY) * dy) / len2 : 0.f;
            break;
        }
        case Kind::Radial: {
            float dx = p.fX - p0.fX, dy = p.fY - p0.fY;
            t = radius > 1e-6f ? std::sqrt(dx * dx + dy * dy) / radius : 0.f;
            break;
        }
        case Kind::Sweep: {
            float ang = std::atan2(p.fY - p0.fY, p.fX - p0.fX);
            if (ang < 0) ang += 6.2831853071795865f;
            t = ang / 6.2831853071795865f;
            break;
        }
    }
    if (tile == SkTileMode::kRepeat) t = t - std::floor(t);
    return sampleStops(colors, stops, t);
}

static sk_sp<SkShader> makeShader(SkShader::Kind k, const SkColor colors[],
                                  const SkScalar pos[], int count,
                                  SkTileMode mode, const SkMatrix* local) {
    auto sh = sk_make_sp<SkShader>();
    sh->kind = k;
    sh->tile = mode;
    for (int i = 0; i < count; ++i) sh->colors.push_back(unpack(colors[i]));
    for (int i = 0; i < count; ++i)
        sh->stops.push_back(pos ? pos[i] : (count > 1 ? float(i) / float(count - 1) : 0.f));
    if (local) {
        SkMatrix inv;
        if (local->invert(&inv)) { sh->localInverse = inv; sh->hasLocal = true; }
    }
    return sh;
}

sk_sp<SkShader> SkGradientShader::MakeLinear(const SkPoint pts[2], const SkColor colors[],
                                             const SkScalar pos[], int count,
                                             SkTileMode mode, uint32_t, const SkMatrix* local) {
    auto s = makeShader(SkShader::Kind::Linear, colors, pos, count, mode, local);
    s->p0 = pts[0]; s->p1 = pts[1];
    return s;
}
sk_sp<SkShader> SkGradientShader::MakeRadial(SkPoint center, SkScalar radius,
                                             const SkColor colors[], const SkScalar pos[],
                                             int count, SkTileMode mode, uint32_t,
                                             const SkMatrix* local) {
    auto s = makeShader(SkShader::Kind::Radial, colors, pos, count, mode, local);
    s->p0 = center; s->radius = radius;
    return s;
}
sk_sp<SkShader> SkGradientShader::MakeSweep(SkScalar cx, SkScalar cy,
                                            const SkColor colors[], const SkScalar pos[],
                                            int count, uint32_t, const SkMatrix* local) {
    auto s = makeShader(SkShader::Kind::Sweep, colors, pos, count, SkTileMode::kClamp, local);
    s->p0 = {cx, cy};
    return s;
}

sk_sp<SkImageFilter> SkImageFilters::Blur(SkScalar sx, SkScalar sy, sk_sp<SkImageFilter>) {
    auto f = sk_make_sp<SkImageFilter>();
    f->sigmaX = sx; f->sigmaY = sy;
    return f;
}
sk_sp<SkPathEffect> SkDashPathEffect::Make(const SkScalar intervals[], int count, SkScalar phase) {
    auto e = sk_make_sp<SkPathEffect>();
    for (int i = 0; i < count; ++i) e->intervals.push_back(intervals[i]);
    e->phase = phase;
    return e;
}

// ============================================================================
// Path
// ============================================================================
void SkPath::moveTo(SkScalar x, SkScalar y) {
    contours_.push_back(Contour{});
    contours_.back().pts.push_back({x, y});
}
void SkPath::lineTo(SkScalar x, SkScalar y) {
    if (contours_.empty()) moveTo(x, y);
    else contours_.back().pts.push_back({x, y});
}
void SkPath::close() { if (!contours_.empty()) contours_.back().closed = true; }

void SkPath::addArc(const SkRect& oval, SkScalar startDeg, SkScalar sweepDeg) {
    const float rx = oval.width() * 0.5f, ry = oval.height() * 0.5f;
    const float cx = oval.centerX(), cy = oval.centerY();
    const int steps = std::max(8, int(std::fabs(sweepDeg) / 4.f));
    contours_.push_back(Contour{});
    auto& c = contours_.back();
    for (int i = 0; i <= steps; ++i) {
        float a = (startDeg + sweepDeg * float(i) / float(steps)) * 0.01745329252f;
        c.pts.push_back({ cx + rx * std::cos(a), cy + ry * std::sin(a) });
    }
}
void SkPath::addCircle(SkScalar cx, SkScalar cy, SkScalar r) {
    addArc(SkRect::MakeXYWH(cx - r, cy - r, r * 2, r * 2), 0, 360);
    close();
}
SkRect SkPath::bounds() const {
    if (contours_.empty()) return {};
    SkRect b{1e9f, 1e9f, -1e9f, -1e9f};
    for (auto& c : contours_)
        for (auto& p : c.pts) {
            b.fLeft = std::fmin(b.fLeft, p.fX);   b.fTop = std::fmin(b.fTop, p.fY);
            b.fRight = std::fmax(b.fRight, p.fX); b.fBottom = std::fmax(b.fBottom, p.fY);
        }
    return b;
}

// ============================================================================
// SDF primitives (all in local space)
// ============================================================================
static inline float sdRoundBox(float px, float py, float cx, float cy,
                               float hx, float hy, float r) {
    float qx = std::fabs(px - cx) - (hx - r);
    float qy = std::fabs(py - cy) - (hy - r);
    float ax = std::fmax(qx, 0.f), ay = std::fmax(qy, 0.f);
    return std::sqrt(ax * ax + ay * ay) + std::fmin(std::fmax(qx, qy), 0.f) - r;
}
static inline float sdCircle(float px, float py, float cx, float cy, float r) {
    float dx = px - cx, dy = py - cy;
    return std::sqrt(dx * dx + dy * dy) - r;
}
static inline float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float d = bax * bax + bay * bay;
    float h = d > 1e-9f ? std::clamp((pax * bax + pay * bay) / d, 0.f, 1.f) : 0.f;
    float dx = pax - bax * h, dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

// ============================================================================
// Canvas
// ============================================================================
void SkCanvas::clear(SkColor c) {
    uint32_t* px = surf_->pixels();
    const size_t n = size_t(surf_->width()) * size_t(surf_->height());
    SkRGBA k = unpack(c);
    uint32_t v = (uint32_t(k.a * 255) << 24) | (uint32_t(k.r * k.a * 255) << 16) |
                 (uint32_t(k.g * k.a * 255) << 8) | uint32_t(k.b * k.a * 255);
    std::fill(px, px + n, v);
}

int  SkCanvas::save() { stack_.push_back(stack_.back()); return int(stack_.size()) - 1; }
void SkCanvas::restore() { if (stack_.size() > 1) stack_.pop_back(); }

void SkCanvas::translate(SkScalar dx, SkScalar dy) {
    SkMatrix t; t.setTranslate(dx, dy);
    stack_.back().ctm = stack_.back().ctm.concat(t);
}
void SkCanvas::scale(SkScalar sx, SkScalar sy) {
    SkMatrix t; t.setScale(sx, sy);
    stack_.back().ctm = stack_.back().ctm.concat(t);
}
void SkCanvas::rotate(SkScalar deg) {
    SkMatrix t; t.setRotate(deg, 0, 0);
    stack_.back().ctm = stack_.back().ctm.concat(t);
}
void SkCanvas::concat(const SkMatrix& m) {
    stack_.back().ctm = stack_.back().ctm.concat(m);
}

void SkCanvas::clipRRect(const SkRRect& r, bool) {
    State& s = stack_.back();
    s.hasClip = true;
    s.clip = r;
    s.ctm.invert(&s.clipCtmInverse);
}
void SkCanvas::clipRect(const SkRect& r, bool aa) { clipRRect(SkRRect::MakeRect(r), aa); }

// ---------------------------------------------------------------------------
// Core rasteriser. sdfLocal(x, y) returns a signed distance in LOCAL units.
// ---------------------------------------------------------------------------
template <typename SdfFn>
void SkCanvas::rasterise(const SkRect& localBounds, const SkPaint& paint, SdfFn sdfLocal) {
    const State& st = stack_.back();
    const SkMatrix& ctm = st.ctm;
    const float scale = std::fmax(ctm.meanScale(), 1e-4f);

    // Device-space bounding box (map the 4 local corners).
    SkPoint c0 = ctm.mapPoint({localBounds.fLeft,  localBounds.fTop});
    SkPoint c1 = ctm.mapPoint({localBounds.fRight, localBounds.fTop});
    SkPoint c2 = ctm.mapPoint({localBounds.fLeft,  localBounds.fBottom});
    SkPoint c3 = ctm.mapPoint({localBounds.fRight, localBounds.fBottom});
    float minX = std::fmin(std::fmin(c0.fX, c1.fX), std::fmin(c2.fX, c3.fX));
    float maxX = std::fmax(std::fmax(c0.fX, c1.fX), std::fmax(c2.fX, c3.fX));
    float minY = std::fmin(std::fmin(c0.fY, c1.fY), std::fmin(c2.fY, c3.fY));
    float maxY = std::fmax(std::fmax(c0.fY, c1.fY), std::fmax(c2.fY, c3.fY));

    int x0 = std::max(0, int(std::floor(minX)) - 2);
    int y0 = std::max(0, int(std::floor(minY)) - 2);
    int x1 = std::min(surf_->width(),  int(std::ceil(maxX)) + 2);
    int y1 = std::min(surf_->height(), int(std::ceil(maxY)) + 2);
    if (x0 >= x1 || y0 >= y1) return;

    SkMatrix inv;
    if (!ctm.invert(&inv)) return;

    const SkRGBA base = unpack(paint.getColor());
    const SkShader* sh = paint.getShader().get();
    const bool stroke = paint.getStyle() == SkPaint::kStroke_Style;
    const float halfW = paint.getStrokeWidth() * 0.5f;
    // One device pixel expressed in local units — the AA band width.
    const float aaLocal = paint.isAntiAlias() ? (1.0f / scale) : 1e-4f;

    uint32_t* px = surf_->pixels();
    const int W = surf_->width();

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            SkPoint lp = inv.mapPoint({float(x) + 0.5f, float(y) + 0.5f});
            float d = sdfLocal(lp.fX, lp.fY);
            if (stroke) d = std::fabs(d) - halfW;

            // Analytic AA: coverage across one device pixel.
            float cov = std::clamp(0.5f - d / aaLocal, 0.f, 1.f);
            if (cov <= 0.0015f) continue;

            // Clip (evaluated in the clip's own local space).
            if (st.hasClip) {
                SkPoint cp = st.clipCtmInverse.mapPoint({float(x) + 0.5f, float(y) + 0.5f});
                const SkRect& cr = st.clip.rect();
                float cd = sdRoundBox(cp.fX, cp.fY, cr.centerX(), cr.centerY(),
                                      cr.width() * 0.5f, cr.height() * 0.5f, st.clip.rx());
                cov *= std::clamp(0.5f - cd / aaLocal, 0.f, 1.f);
                if (cov <= 0.0015f) continue;
            }

            SkRGBA src = sh ? sh->eval(lp.fX, lp.fY) : base;
            float a = src.a * cov;
            if (a <= 0.0015f) continue;

            // Premultiplied source-over into BGRA.
            uint32_t dst = px[size_t(y) * size_t(W) + size_t(x)];
            float da = float(dst >> 24) / 255.f;
            float dr = float((dst >> 16) & 0xFF) / 255.f;
            float dg = float((dst >> 8) & 0xFF) / 255.f;
            float db = float(dst & 0xFF) / 255.f;

            float sr = src.r * a, sg = src.g * a, sb = src.b * a;
            float ia = 1.f - a;
            float orr = sr + dr * ia, og = sg + dg * ia, ob = sb + db * ia;
            float oa = a + da * ia;

            px[size_t(y) * size_t(W) + size_t(x)] =
                (uint32_t(std::clamp(oa, 0.f, 1.f) * 255.f + 0.5f) << 24) |
                (uint32_t(std::clamp(orr, 0.f, 1.f) * 255.f + 0.5f) << 16) |
                (uint32_t(std::clamp(og, 0.f, 1.f) * 255.f + 0.5f) << 8) |
                 uint32_t(std::clamp(ob, 0.f, 1.f) * 255.f + 0.5f);
        }
    }
}

// --- blurred variant: render to a scratch layer, blur, composite -----------
void SkCanvas::blitLayer(const SkBitmapSurface& layer, SkScalar sigmaX, SkScalar sigmaY) {
    const int W = surf_->width(), H = surf_->height();
    std::vector<float> a(size_t(W) * H), r(size_t(W) * H), g(size_t(W) * H), b(size_t(W) * H);
    const uint32_t* lp = layer.pixels();
    for (size_t i = 0; i < size_t(W) * H; ++i) {
        a[i] = float(lp[i] >> 24) / 255.f;
        r[i] = float((lp[i] >> 16) & 0xFF) / 255.f;
        g[i] = float((lp[i] >> 8) & 0xFF) / 255.f;
        b[i] = float(lp[i] & 0xFF) / 255.f;
    }
    // Three box passes ≈ Gaussian (central limit theorem).
    // Sliding-window running sum: O(1) per pixel regardless of radius.
    std::vector<float> tmp(size_t(W) * H);
    auto boxBlur = [&](std::vector<float>& src, int radius, bool horizontal) {
        if (radius < 1) return;
        const int n = 2 * radius + 1;
        const float inv = 1.f / float(n);
        if (horizontal) {
            for (int y = 0; y < H; ++y) {
                const float* row = &src[size_t(y) * W];
                float* dst = &tmp[size_t(y) * W];
                float acc = 0.f;
                for (int k = -radius; k <= radius; ++k) acc += row[std::clamp(k, 0, W - 1)];
                for (int x = 0; x < W; ++x) {
                    dst[x] = acc * inv;
                    acc += row[std::clamp(x + radius + 1, 0, W - 1)];
                    acc -= row[std::clamp(x - radius, 0, W - 1)];
                }
            }
        } else {
            for (int x = 0; x < W; ++x) {
                float acc = 0.f;
                for (int k = -radius; k <= radius; ++k)
                    acc += src[size_t(std::clamp(k, 0, H - 1)) * W + x];
                for (int y = 0; y < H; ++y) {
                    tmp[size_t(y) * W + x] = acc * inv;
                    acc += src[size_t(std::clamp(y + radius + 1, 0, H - 1)) * W + x];
                    acc -= src[size_t(std::clamp(y - radius, 0, H - 1)) * W + x];
                }
            }
        }
        src.swap(tmp);
    };
    const int rx = int(sigmaX * 1.2f), ry = int(sigmaY * 1.2f);
    for (int pass = 0; pass < 3; ++pass) {
        boxBlur(a, rx, true);  boxBlur(a, ry, false);
        boxBlur(r, rx, true);  boxBlur(r, ry, false);
        boxBlur(g, rx, true);  boxBlur(g, ry, false);
        boxBlur(b, rx, true);  boxBlur(b, ry, false);
    }
    uint32_t* px = surf_->pixels();
    for (size_t i = 0; i < size_t(W) * H; ++i) {
        float sa = a[i];
        if (sa <= 0.0015f) continue;
        uint32_t dst = px[i];
        float da = float(dst >> 24) / 255.f;
        float dr = float((dst >> 16) & 0xFF) / 255.f;
        float dg = float((dst >> 8) & 0xFF) / 255.f;
        float db = float(dst & 0xFF) / 255.f;
        float ia = 1.f - sa;
        px[i] = (uint32_t(std::clamp(sa + da * ia, 0.f, 1.f) * 255.f + 0.5f) << 24) |
                (uint32_t(std::clamp(r[i] + dr * ia, 0.f, 1.f) * 255.f + 0.5f) << 16) |
                (uint32_t(std::clamp(g[i] + dg * ia, 0.f, 1.f) * 255.f + 0.5f) << 8) |
                 uint32_t(std::clamp(b[i] + db * ia, 0.f, 1.f) * 255.f + 0.5f);
    }
}

// --- helper: run a draw through the blur pipeline when a filter is set -----
#define VOX_MAYBE_BLUR(DRAWCALL)                                              \
    do {                                                                      \
        if (paint.getImageFilter()) {                                         \
            SkBitmapSurface layer(surf_->width(), surf_->height());           \
            SkCanvas lc(&layer);                                              \
            lc.stack_ = stack_;                                               \
            SkPaint np = paint;                                               \
            np.setImageFilter(nullptr);                                       \
            { const SkPaint& paint = np; DRAWCALL; }                          \
            blitLayer(layer, paint.getImageFilter()->sigmaX,                  \
                             paint.getImageFilter()->sigmaY);                 \
            return;                                                           \
        }                                                                     \
    } while (0)

void SkCanvas::drawRRect(const SkRRect& rr, const SkPaint& paint) {
    VOX_MAYBE_BLUR(lc.drawRRect(rr, paint));
    const SkRect& r = rr.rect();
    const float cx = r.centerX(), cy = r.centerY();
    const float hx = r.width() * 0.5f, hy = r.height() * 0.5f;
    const float rad = std::fmin(rr.rx(), std::fmin(hx, hy));
    float pad = paint.getStrokeWidth() + 2.f;
    rasterise(r.makeOutset(pad, pad), paint,
              [=](float x, float y) { return sdRoundBox(x, y, cx, cy, hx, hy, rad); });
}

void SkCanvas::drawRect(const SkRect& r, const SkPaint& paint) {
    drawRRect(SkRRect::MakeRect(r), paint);
}

void SkCanvas::drawCircle(SkScalar cx, SkScalar cy, SkScalar radius, const SkPaint& paint) {
    VOX_MAYBE_BLUR(lc.drawCircle(cx, cy, radius, paint));
    float pad = paint.getStrokeWidth() + 2.f;
    SkRect b = SkRect::MakeXYWH(cx - radius, cy - radius, radius * 2, radius * 2);
    rasterise(b.makeOutset(pad, pad), paint,
              [=](float x, float y) { return sdCircle(x, y, cx, cy, radius); });
}

void SkCanvas::drawLine(SkScalar ax, SkScalar ay, SkScalar bx, SkScalar by, const SkPaint& paint) {
    VOX_MAYBE_BLUR(lc.drawLine(ax, ay, bx, by, paint));
    const float hw = paint.getStrokeWidth() * 0.5f;
    SkRect b = SkRect::MakeLTRB(std::fmin(ax, bx), std::fmin(ay, by),
                                std::fmax(ax, bx), std::fmax(ay, by));
    // Stroke is already baked into the SDF here, so draw as a filled capsule.
    SkPaint fp = paint;
    fp.setStyle(SkPaint::kFill_Style);
    rasterise(b.makeOutset(hw + 2.f, hw + 2.f), fp,
              [=](float x, float y) { return sdSegment(x, y, ax, ay, bx, by) - hw; });
}

void SkCanvas::drawPath(const SkPath& path, const SkPaint& paint) {
    VOX_MAYBE_BLUR(lc.drawPath(path, paint));
    if (path.isEmpty()) return;

    // Flatten every contour into segments, applying the dash effect if present.
    struct Seg { float ax, ay, bx, by; };
    std::vector<Seg> segs;
    const SkPathEffect* dash = paint.getPathEffect().get();

    for (const auto& c : path.contours()) {
        if (c.pts.size() < 2) {
            if (c.pts.size() == 1) segs.push_back({c.pts[0].fX, c.pts[0].fY,
                                                   c.pts[0].fX, c.pts[0].fY});
            continue;
        }
        size_t n = c.pts.size();
        float travelled = dash ? -dash->phase : 0.f;
        for (size_t i = 0; i + 1 < n + (c.closed ? 1 : 0); ++i) {
            const SkPoint& a = c.pts[i % n];
            const SkPoint& b = c.pts[(i + 1) % n];
            if (!dash || dash->intervals.size() < 2) {
                segs.push_back({a.fX, a.fY, b.fX, b.fY});
                continue;
            }
            // Walk the segment, emitting only the "on" spans of the dash.
            const float on = dash->intervals[0], off = dash->intervals[1];
            const float period = std::fmax(on + off, 1e-4f);
            const float len = std::hypot(b.fX - a.fX, b.fY - a.fY);
            const float step = std::fmax(len / 96.f, 0.25f);
            bool run = false; float rsx = 0, rsy = 0;
            for (float t = 0.f; t <= len + 1e-4f; t += step) {
                float u = len > 1e-6f ? t / len : 0.f;
                float x = a.fX + (b.fX - a.fX) * u, y = a.fY + (b.fY - a.fY) * u;
                float ph = std::fmod(std::fmax(travelled + t, 0.f), period);
                bool inOn = (travelled + t >= 0.f) && (ph < on);
                if (inOn && !run) { run = true; rsx = x; rsy = y; }
                else if (!inOn && run) { run = false; segs.push_back({rsx, rsy, x, y}); }
            }
            if (run) segs.push_back({rsx, rsy, b.fX, b.fY});
            travelled += len;
        }
    }
    if (segs.empty()) return;

    const bool stroke = paint.getStyle() == SkPaint::kStroke_Style;
    const float hw = stroke ? paint.getStrokeWidth() * 0.5f : 0.5f;

    SkRect b = path.bounds().makeOutset(hw + 2.f, hw + 2.f);
    SkPaint fp = paint;
    fp.setStyle(SkPaint::kFill_Style);   // capsule union is already a fill
    rasterise(b, fp, [&segs, hw](float x, float y) {
        float d = 1e9f;
        for (const auto& s : segs) d = std::fmin(d, sdSegment(x, y, s.ax, s.ay, s.bx, s.by));
        return d - hw;   // round caps + round joins fall out of the union
    });
}

// ============================================================================
// Text — compact 5x7 face, bilinear coverage sampling, SDF-style AA.
// ============================================================================
namespace {

// Bit 0..4 of each byte = one column, 7 rows. ASCII 32..126.
const uint8_t kFont5x7[95][5] = {
{0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},
{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},
{0,5,3,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x14,8,0x3E,8,0x14},
{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},
{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},
{6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{0,8,0x14,0x22,0x41},
{0x14,0x14,0x14,0x14,0x14},{0x41,0x22,0x14,8,0},{2,1,0x51,9,6},
{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},
{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
{0x7F,2,0xC,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},
{0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},
{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,8,0x14,0x63},
{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7F,0x41,0x41,0},{2,4,8,0x10,0x20},
{0,0x41,0x41,0x7F,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},{0,1,2,4,0},
{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{8,0x7E,9,1,2},
{0x0C,0x52,0x52,0x52,0x3E},{0x7F,8,4,4,0x78},{0,0x44,0x7D,0x40,0},
{0x20,0x40,0x44,0x3D,0},{0x7F,0x10,0x28,0x44,0},{0,0x41,0x7F,0x40,0},
{0x7C,4,0x18,4,0x78},{0x7C,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},
{0x7C,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7C},{0x7C,8,4,4,8},
{0x48,0x54,0x54,0x54,0x20},{4,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},
{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0,8,0x36,0x41,0},
{0,0,0x7F,0,0},{0,0x41,0x36,8,0},{8,4,8,0x10,8},
};

// Advance width in "font cells" (5 cols + 1 space), scaled by size/7.
constexpr float kCellW = 6.f, kCellH = 7.f;

inline float glyphCoverage(int ch, float u, float v) {
    if (ch < 32 || ch > 126) return 0.f;
    const uint8_t* g = kFont5x7[ch - 32];
    // Bilinear sample of the 5x7 bitmap for smooth edges at small sizes.
    float fx = u * 5.f - 0.5f, fy = v * 7.f - 0.5f;
    int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    float tx = fx - float(x0), ty = fy - float(y0);
    auto bit = [&](int x, int y) -> float {
        if (x < 0 || x > 4 || y < 0 || y > 6) return 0.f;
        return (g[x] >> y) & 1 ? 1.f : 0.f;
    };
    float a = bit(x0, y0) * (1 - tx) * (1 - ty) + bit(x0 + 1, y0) * tx * (1 - ty)
            + bit(x0, y0 + 1) * (1 - tx) * ty + bit(x0 + 1, y0 + 1) * tx * ty;
    return a;
}

} // namespace

SkScalar SkFont::measureText(const void* text, size_t byteLength, SkTextEncoding) const {
    const float s = size_ / kCellH;
    return float(byteLength) * kCellW * s * 0.92f;
}

void SkCanvas::drawSimpleText(const void* text, size_t byteLength, SkTextEncoding,
                              SkScalar x, SkScalar y, const SkFont& font, const SkPaint& paint) {
    const char* str = static_cast<const char*>(text);
    const float s = font.getSize() / kCellH;
    const float glyphW = 5.f * s, glyphH = 7.f * s;
    const float advance = kCellW * s * 0.92f;

    const State& st = stack_.back();
    SkMatrix inv;
    if (!st.ctm.invert(&inv)) return;
    const SkRGBA base = unpack(paint.getColor());
    const SkShader* sh = paint.getShader().get();
    uint32_t* px = surf_->pixels();
    const int W = surf_->width(), H = surf_->height();

    for (size_t i = 0; i < byteLength; ++i) {
        const unsigned char ch = (unsigned char)str[i];
        if (ch == ' ') continue;
        // Baseline at y; the 7-row cell sits above it.
        const float gx = x + float(i) * advance;
        const float gy = y - glyphH;

        SkRect lb = SkRect::MakeXYWH(gx, gy, glyphW, glyphH).makeOutset(1.f, 1.f);
        SkPoint c0 = st.ctm.mapPoint({lb.fLeft, lb.fTop});
        SkPoint c3 = st.ctm.mapPoint({lb.fRight, lb.fBottom});
        int px0 = std::max(0, int(std::floor(std::fmin(c0.fX, c3.fX))) - 1);
        int py0 = std::max(0, int(std::floor(std::fmin(c0.fY, c3.fY))) - 1);
        int px1 = std::min(W, int(std::ceil(std::fmax(c0.fX, c3.fX))) + 1);
        int py1 = std::min(H, int(std::ceil(std::fmax(c0.fY, c3.fY))) + 1);

        for (int dy = py0; dy < py1; ++dy) {
            for (int dx = px0; dx < px1; ++dx) {
                SkPoint lp = inv.mapPoint({float(dx) + 0.5f, float(dy) + 0.5f});
                float u = (lp.fX - gx) / glyphW, v = (lp.fY - gy) / glyphH;
                if (u < -0.05f || u > 1.05f || v < -0.05f || v > 1.05f) continue;
                float cov = glyphCoverage(ch, u, v);
                if (cov <= 0.02f) continue;
                cov = std::clamp(cov * 1.25f, 0.f, 1.f);

                if (st.hasClip) {
                    SkPoint cp = st.clipCtmInverse.mapPoint({float(dx) + 0.5f, float(dy) + 0.5f});
                    const SkRect& cr = st.clip.rect();
                    float cd = sdRoundBox(cp.fX, cp.fY, cr.centerX(), cr.centerY(),
                                          cr.width() * 0.5f, cr.height() * 0.5f, st.clip.rx());
                    cov *= std::clamp(0.5f - cd, 0.f, 1.f);
                    if (cov <= 0.0015f) continue;
                }

                SkRGBA src = sh ? sh->eval(lp.fX, lp.fY) : base;
                float a = src.a * cov;
                if (a <= 0.0015f) continue;
                uint32_t dst = px[size_t(dy) * W + dx];
                float da = float(dst >> 24) / 255.f;
                float dr = float((dst >> 16) & 0xFF) / 255.f;
                float dg = float((dst >> 8) & 0xFF) / 255.f;
                float db = float(dst & 0xFF) / 255.f;
                float ia = 1.f - a;
                px[size_t(dy) * W + dx] =
                    (uint32_t(std::clamp(a + da * ia, 0.f, 1.f) * 255.f + 0.5f) << 24) |
                    (uint32_t(std::clamp(src.r * a + dr * ia, 0.f, 1.f) * 255.f + 0.5f) << 16) |
                    (uint32_t(std::clamp(src.g * a + dg * ia, 0.f, 1.f) * 255.f + 0.5f) << 8) |
                     uint32_t(std::clamp(src.b * a + db * ia, 0.f, 1.f) * 255.f + 0.5f);
            }
        }
    }
}

// ============================================================================
// PNG writer — stored (uncompressed) deflate blocks, no zlib dependency.
// ============================================================================
namespace {
uint32_t crcTable[256];
bool crcInit = false;
void initCrc() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
    crcInit = true;
}
uint32_t crc32buf(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    if (!crcInit) initCrc();
    for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32buf(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
} // namespace

bool SkWritePNG(const SkBitmapSurface& s, const char* path) {
    const int W = s.width(), H = s.height();
    // Un-premultiply into RGBA rows with a leading filter byte.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(H) * (size_t(W) * 4 + 1));
    const uint32_t* px = s.pixels();
    for (int y = 0; y < H; ++y) {
        raw.push_back(0);
        for (int x = 0; x < W; ++x) {
            uint32_t p = px[size_t(y) * W + x];
            float a = float(p >> 24) / 255.f;
            auto un = [&](uint32_t c) -> uint8_t {
                if (a <= 0.001f) return 0;
                return uint8_t(std::clamp(float(c) / 255.f / a, 0.f, 1.f) * 255.f + 0.5f);
            };
            raw.push_back(un((p >> 16) & 0xFF));
            raw.push_back(un((p >> 8) & 0xFF));
            raw.push_back(un(p & 0xFF));
            raw.push_back(uint8_t(p >> 24));
        }
    }
    // zlib stream with stored deflate blocks.
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = (pos + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 0xFF)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xFF)); z.push_back(uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
    }
    uint32_t a1 = 1, a2 = 0;
    for (uint8_t b : raw) { a1 = (a1 + b) % 65521; a2 = (a2 + a1) % 65521; }
    be32(z, (a2 << 16) | a1);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(W)); be32(ihdr, uint32_t(H));
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0);
    ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}
