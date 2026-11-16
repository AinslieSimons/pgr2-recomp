// Headlight shadows, which the console never had. At night the road around the followed car is lit by the light pool: a 256x256
// target drawn from above each frame (0x180230) that the road samples in stage 3. Each car's headlight beam is one triangle
// strip in it (0x32850 -> 0x1F2A0 -> 0x1D4A0): flat, in the car's own space (y -0.3, forward -z), textured, added into the pool's
// alpha (blend ONE, ONE). Here the strip of a car with others in front of it is taken as the game makes it (its vertex calls are
// caught instead of sent), the shadows of those cars are cut out of it on the CPU, and what is left is drawn in its place with
// the game's own state and matrix. The pool has no stencil (its depth buffer is LIN_D16), so the cut is geometry.
// A shadow is the occluder's footprint pushed away from a lamp to past the beam's end. Two lamps, at the front corners, each draw
// the beam at half strength with their own shadows cut out: the half shadow between them is the soft edge.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace pgr2 {
extern bool headlight_shadows;  // src/common/game_info.cpp
}

X86_EXTERN_FUNC(__imp__sub_0001F2A0);
X86_EXTERN_FUNC(__imp__sub_001C0C50);
X86_EXTERN_FUNC(__imp__sub_001C0C10);
X86_EXTERN_FUNC(__imp__sub_001C0CF0);

namespace {
using kernel::R32;

constexpr uint32_t kCarList = 0x297390, kCarCount = 0x0C, kCars = 0x1C00, kCarStride = 0x6990;
constexpr uint32_t kRow0 = 0x14, kRow2 = 0x34, kPlace = 0x44, kRadius = 0xA4, kWidth = 0xA8, kLength = 0xAC;
constexpr uint32_t kBegin = 0x1C0D40, kEnd = 0x1C0D80;                         // D3DDevice_Begin (primitive), End
constexpr uint32_t kVertex4f = 0x1C0C50, kVertex2f = 0x1C0C10, kColour = 0x1C0CF0;  // (register, ...), stdcall
constexpr uint32_t kPosition = 0xFFFFFFFFu, kTexture0 = 9, kDiffuse = 3, kTriangleList = 5;
constexpr float kFootprint = 0.9f;  // of +0xA8 and +0xAC: they are a little larger than the body
constexpr float kReach = 60.0f;     // a shadow's length past its footprint, beyond any beam's end

float F(uint32_t va) {
    const uint32_t v = R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// a vertex of the beam: position x y z w, texture u v, colour a r g b (0 to 255), all linear across the flat strip
struct Vtx {
    float a[10];
    float x() const { return a[0]; }
    float z() const { return a[2]; }
};
Vtx Lerp(const Vtx& p, const Vtx& q, float t) {
    Vtx r;
    for (int i = 0; i < 10; i++) r.a[i] = p.a[i] + (q.a[i] - p.a[i]) * t;
    return r;
}
using Poly = std::vector<Vtx>;

struct P2 {
    float x, z;
};
float Cross(P2 o, P2 a, P2 b) { return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x); }

// convex hull, counter-clockwise in x / z (monotone chain)
std::vector<P2> Hull(std::vector<P2> p) {
    std::sort(p.begin(), p.end(), [](P2 a, P2 b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
    std::vector<P2> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); i++) {
        while (k >= 2 && Cross(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && Cross(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    h.resize(k - 1);
    return h;
}

// the part of a convex polygon on one side of the line a -> b: left (inside a counter-clockwise hull) or right
Poly Clip(const Poly& in, P2 a, P2 b, bool left) {
    Poly out;
    auto side = [&](const Vtx& v) {
        const float s = Cross(a, b, {v.x(), v.z()});
        return left ? s : -s;
    };
    for (size_t i = 0; i < in.size(); i++) {
        const Vtx &p = in[i], &q = in[(i + 1) % in.size()];
        const float sp = side(p), sq = side(q);
        if (sp >= 0) out.push_back(p);
        if ((sp >= 0) != (sq >= 0)) out.push_back(Lerp(p, q, sp / (sp - sq)));
    }
    return out;
}

float Area(const Poly& p) {
    float a = 0;
    for (size_t i = 0; i < p.size(); i++) {
        const Vtx &u = p[i], &v = p[(i + 1) % p.size()];
        a += u.x() * v.z() - v.x() * u.z();
    }
    return std::fabs(a) * 0.5f;
}

// pieces minus a convex hull: for each of its edges, what lies outside it (and inside the ones before) is kept
void Subtract(std::vector<Poly>& pieces, const std::vector<P2>& hull, std::vector<Poly>* inside = nullptr) {
    std::vector<Poly> out;
    for (const Poly& piece : pieces) {
        Poly rest = piece;
        for (size_t e = 0; e < hull.size() && rest.size() >= 3; e++) {
            const P2 a = hull[e], b = hull[(e + 1) % hull.size()];
            Poly outside = Clip(rest, a, b, false);
            if (outside.size() >= 3 && Area(outside) > 1e-4f) out.push_back(std::move(outside));
            rest = Clip(rest, a, b, true);
        }
        if (inside && rest.size() >= 3 && Area(rest) > 1e-4f) inside->push_back(std::move(rest));
    }
    pieces.swap(out);
}

// the strip's vertex calls while a beam is caught
bool g_catch = false;
float g_uv[2] = {0, 0};
uint32_t g_colour = 0xFFFFFFFFu;
std::vector<Vtx> g_strip;

struct Car {
    float right[3], forward[3], place[3];
    float half_width, half_length, radius;
};
Car ReadCar(uint32_t car) {
    Car c;
    for (int i = 0; i < 3; i++) {
        c.right[i] = F(car + kRow0 + i * 4);
        c.forward[i] = F(car + kRow2 + i * 4);  // the car's +z; it drives along -z
        c.place[i] = F(car + kPlace + i * 4);
    }
    c.half_width = F(car + kWidth) * 0.5f * kFootprint;
    c.half_length = F(car + kLength) * 0.5f * kFootprint;
    c.radius = F(car + kRadius);
    return c;
}
// a world point in a car's own x / z
P2 ToCar(const Car& c, const float w[3]) {
    const float d[3] = {w[0] - c.place[0], w[1] - c.place[1], w[2] - c.place[2]};
    return {d[0] * c.right[0] + d[1] * c.right[1] + d[2] * c.right[2], d[0] * c.forward[0] + d[1] * c.forward[1] + d[2] * c.forward[2]};
}

// the footprints, in the beam car's x / z, of the other cars that may fall within its beam (z from near to far, |x| below wide)
std::vector<std::vector<P2>> Occluders(uint32_t beam_car, float near, float far, float wide) {
    std::vector<std::vector<P2>> found;
    const Car self = ReadCar(beam_car);
    const uint32_t count = std::min<uint32_t>(R32(kCarList + kCarCount), 16);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t car = kCarList + kCars + i * kCarStride;
        if (car == beam_car) continue;
        const Car o = ReadCar(car);
        const P2 centre = ToCar(self, o.place);
        if (o.radius <= 0 || centre.z > near + o.radius || centre.z < far - o.radius || std::fabs(centre.x) > wide + o.radius) continue;
        std::vector<P2> corners;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                float w[3];
                for (int k = 0; k < 3; k++) w[k] = o.place[k] + sx * o.half_width * o.right[k] + sz * o.half_length * o.forward[k];
                corners.push_back(ToCar(self, w));
            }
        found.push_back(std::move(corners));
    }
    return found;
}

void Emit(X86Context& ctx, const std::vector<Poly>& pieces, float strength) {
    kernel::CallGuest(ctx, kBegin, {kTriangleList});
    for (const Poly& p : pieces)
        for (size_t i = 1; i + 1 < p.size(); i++)
            for (const Vtx* v : {&p[0], &p[i], &p[i + 1]}) {
                uint32_t colour = 0;
                for (int c = 0; c < 4; c++) {
                    // the beam is added (ONE, ONE): every byte scaled, its alpha too (the strip fades to 0 at its far end)
                    const float f = std::clamp(v->a[6 + c] * strength, 0.0f, 255.0f);
                    colour |= uint32_t(std::lround(f)) << (24 - c * 8);
                }
                kernel::CallGuest(ctx, kColour, {kDiffuse, colour});
                kernel::CallGuest(ctx, kVertex2f, {kTexture0, Bits(v->a[4]), Bits(v->a[5])});
                kernel::CallGuest(ctx, kVertex4f, {kPosition, Bits(v->a[0]), Bits(v->a[1]), Bits(v->a[2]), Bits(v->a[3])});
            }
    kernel::CallGuest(ctx, kEnd, {});
}
}  // namespace

// a car's beam into the light pool (thiscall on the car, its only caller 0x32850)
X86_FUNC(sub_0001F2A0) {
    const uint32_t car = ctx.ecx;
    if (!pgr2::headlight_shadows || !car) {
        __imp__sub_0001F2A0(ctx, base);
        return;
    }
    // the strip as the game makes it, kept here: its Begin and End still reach the device, with nothing between them
    g_strip.clear();
    g_catch = true;
    __imp__sub_0001F2A0(ctx, base);
    g_catch = false;
    if (g_strip.size() < 3) return;

    float near = -1e9f, far = 1e9f, wide = 0;
    for (const Vtx& v : g_strip) near = std::max(near, v.z()), far = std::min(far, v.z()), wide = std::max(wide, std::fabs(v.x()));
    std::vector<Poly> beam;
    for (size_t i = 0; i + 2 < g_strip.size(); i++) {
        const Vtx &a = g_strip[i], &b = g_strip[i + 1], &c = g_strip[i + 2];
        beam.push_back(i & 1 ? Poly{b, a, c} : Poly{a, b, c});  // the strip's winding kept
    }
    const auto occluders = Occluders(car, near, far, wide);
    // testing: PGR2_HL_LOG=1 logs the cars in the beam as they change, and now and then the area cut;
    // PGR2_HL_SHOW=1 draws the beam whole and its shadows brighter on top, to see where they fall
    static size_t last = 0;
    static const bool log = std::getenv("PGR2_HL_LOG") != nullptr;
    if (log && occluders.size() != last) LOGR("headlight shadows: %zu cars in the beam of %08X", occluders.size(), car);
    last = occluders.size();
    if (occluders.empty()) {
        Emit(ctx, beam, 1.0f);
        return;
    }
    // two lamps at the front corners of the beam's near edge, each half the beam with its own shadows cut out
    float near_half = 0;
    for (const Vtx& v : g_strip)
        if (v.z() >= near - 0.01f) near_half = std::max(near_half, std::fabs(v.x()));
    for (float side : {-0.7f, 0.7f}) {
        const P2 lamp = {side * near_half, near};
        std::vector<Poly> pieces = beam, shadows;
        static const bool show = std::getenv("PGR2_HL_SHOW") != nullptr;
        for (const auto& footprint : occluders) {
            std::vector<P2> points = footprint;
            for (const P2& c : footprint) {
                const float dx = c.x - lamp.x, dz = c.z - lamp.z, len = std::sqrt(dx * dx + dz * dz);
                if (len < 1e-3f) continue;
                points.push_back({c.x + dx / len * kReach, c.z + dz / len * kReach});
            }
            Subtract(pieces, Hull(points), (show || log) ? &shadows : nullptr);
        }
        if (log && side > 0) {
            static int n = 0;
            float area = 0, beam_area = 0;
            for (const Poly& p : shadows) area += Area(p);
            for (const Poly& p : beam) beam_area += Area(p);
            if (n++ % 120 == 0) {
                const auto& f = occluders[0];
                LOGR("headlight shadows: beam area %.1f, shadow %.1f; first occluder corners (%.1f %.1f) (%.1f %.1f) (%.1f %.1f) (%.1f %.1f); near %.1f half %.2f",
                     beam_area, area, f[0].x, f[0].z, f[1].x, f[1].z, f[2].x, f[2].z, f[3].x, f[3].z, near, near_half);
            }
        }
        Emit(ctx, show ? beam : pieces, 0.5f);
        if (show) Emit(ctx, shadows, 1.5f);
    }
}

// SetVertexData4f (register, x, y, z, w; ret 0x14): a position ends a vertex
X86_FUNC(sub_001C0C50) {
    if (g_catch && R32(ctx.esp + 4) == kPosition) {
        Vtx v;
        for (int i = 0; i < 4; i++) v.a[i] = F(ctx.esp + 8 + i * 4);
        v.a[4] = g_uv[0], v.a[5] = g_uv[1];
        for (int c = 0; c < 4; c++) v.a[6 + c] = float(g_colour >> (24 - c * 8) & 0xFF);
        g_strip.push_back(v);
        ctx.esp += 4 + 0x14;
        return;
    }
    __imp__sub_001C0C50(ctx, base);
}

// SetVertexData2f (register, a, b; ret 0xC): texture coordinates
X86_FUNC(sub_001C0C10) {
    if (g_catch && R32(ctx.esp + 4) == kTexture0) {
        g_uv[0] = F(ctx.esp + 8), g_uv[1] = F(ctx.esp + 12);
        ctx.esp += 4 + 0xC;
        return;
    }
    __imp__sub_001C0C10(ctx, base);
}

// SetVertexDataColor (register, D3DCOLOR; ret 8): the diffuse
X86_FUNC(sub_001C0CF0) {
    if (g_catch && R32(ctx.esp + 4) == kDiffuse) {
        g_colour = R32(ctx.esp + 8);
        ctx.esp += 4 + 8;
        return;
    }
    __imp__sub_001C0CF0(ctx, base);
}
