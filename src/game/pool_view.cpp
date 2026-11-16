// PGR2_POOL_VIEW=1 (debug): the night light pool, the 256x256 target the road samples in stage 3 (0x180230 draws it with its own
// camera, kind 3: the world's lights in its colour, the followed car's headlight beam in its alpha, 0x32850), drawn at the end of
// each frame in the top left corner: its alpha on the left, its colour beside it. Through the game's own D3D calls, so every
// renderer shows it; each state it sets is read from the XDK's own copies first (D3D_RenderState 0x1D0740, D3D_TextureState
// 0x1D0540) and put back after. Two runtime traps for any drawing from a hook, met here:
// - Begin/End vertices are inline commands the runtime reads at the next HLE call; Swap reads them after it has moved to the next
//   back buffer, which is then cleared. So the view ends with an HLE call (SetVertexShader back to the game's).
// - The Metal backend draws a full screen depth copy instead of any draw that reads a colour target in stage 0 into another target
//   (MetalBackend::DepthCopy), so the pool is read in stage 1.
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "cpu/x86_ops.h"
#include "gpu/state.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace {
using kernel::R32;

constexpr uint32_t kPoolSurface = 0x48639C;  // the target, made by 0x178950
constexpr uint32_t kPoolTexture = 0x486384;  // its texture header (XGSetTextureHeader at 0x178A3E / 0x178A74)
constexpr uint32_t kFrameColour = 0x485DFC;  // the frame's colour surface
constexpr uint32_t kRenderState = 0x1D0740, kTextureState = 0x1D0540, kSimpleEncode = 0x2759D0;
constexpr uint32_t kSetRenderState = 0x1C0E30;  // D3DDevice_SetRenderStateNotInline (state, value)
constexpr uint32_t kSetStageState = 0x1C1870;   // D3DDevice_SetTextureStageStateNotInline (stage, type, value)
constexpr uint32_t kSetTexture = 0x1C3FE0;      // D3DDevice_SetTexture (stage, texture)
constexpr uint32_t kSetVertexShader = 0x1C03F0, kSetPixelShader = 0x1C25C0;
constexpr uint32_t kGameVertexShader = 0x485E04, kGamePixelShader = 0x485E0C;  // the game's own record of what it set
constexpr uint32_t kBegin = 0x1C0D40, kEnd = 0x1C0D80;
constexpr uint32_t kVertex4f = 0x1C0C50, kVertex2f = 0x1C0C10;  // (register, ...): -1 the position, 9 texture coordinate 0
constexpr uint32_t kQuadList = 8, kFvfXyzrhwTex1 = 0x104;
constexpr uint32_t kTexture = 2, kDiffuse = 0, kCurrent = 1, kAlphaReplicate = 0x20, kOpDisable = 1, kOpSelectArg1 = 2;

uint32_t SimpleIndex(uint32_t method) {  // the D3DRS of a simple state, from the XDK's encode table
    for (uint32_t i = 0; i < gpu::kRsDeferredFirst; i++)
        if ((R32(kSimpleEncode + i * 4) & 0xFFFF) == method) return i;
    return ~0u;
}

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct Saved {
    uint32_t state, value;
};

// z 0.5: at 0 a pretransformed vertex sits on the near clip plane
void Quad(X86Context& ctx, float x, float y, float size, bool alpha) {
    constexpr float z = 0.5f;
    kernel::CallGuest(ctx, kSetStageState, {1, gpu::kTssColorArg1, kTexture | (alpha ? kAlphaReplicate : 0)});
    kernel::CallGuest(ctx, kBegin, {kQuadList});
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& c : corners) {
        // a linear texture's coordinates are in texels; set 1 for stage 1
        kernel::CallGuest(ctx, kVertex2f, {10, Bits(c[0] * 256), Bits(c[1] * 256)});
        kernel::CallGuest(ctx, kVertex4f, {0xFFFFFFFFu, Bits(x + c[0] * size), Bits(y + c[1] * size), Bits(z), Bits(1)});
    }
    kernel::CallGuest(ctx, kEnd, {});
}
}  // namespace

namespace pgr2 {
void PoolView(X86Context& ctx) {
    static const bool on = [] { const char* e = std::getenv("PGR2_POOL_VIEW"); return e && *e == '1'; }();
    // into the frame's own target only
    if (!on || !R32(kPoolSurface) || d3d::State().rt.header != R32(kFrameColour)) return;
    static const uint32_t simple[] = {SimpleIndex(gpu::kMAlphaTestEnable), SimpleIndex(gpu::kMBlendEnable),
                                      SimpleIndex(gpu::kMColorWriteEnable), SimpleIndex(gpu::kMZWriteEnable)};
    for (uint32_t s : simple)
        if (s == ~0u) return;
    const uint32_t texture0 = d3d::State().textures[0], texture1 = d3d::State().textures[1];

    // render states: alpha test, blending off, all colour written, no depth, no fog, no culling, no stencil
    const Saved rs[] = {{simple[0], 0}, {simple[1], 0}, {simple[2], 0x01010101}, {simple[3], 0}, {gpu::kRsZEnable, 0},
                        {gpu::kRsFogEnable, 0}, {gpu::kRsCullMode, 0}, {gpu::kRsStencilEnable, 0}};
    // the pool in stage 1, not 0: the Metal backend takes a draw from one target into another with a target in stage 0 for a
    // depth copy (MetalBackend::DepthCopy) and draws that instead. Stage 0 passes the diffuse on
    const Saved tss[] = {{gpu::kTssColorOp, kOpSelectArg1}, {gpu::kTssColorArg1, kDiffuse}, {gpu::kTssAlphaOp, kOpSelectArg1},
                         {gpu::kTssAlphaArg1, kDiffuse},  // 32 +: stage 1, 64 +: stage 2
                         {32 + gpu::kTssColorOp, kOpSelectArg1}, {32 + gpu::kTssColorArg1, kTexture}, {32 + gpu::kTssAlphaOp, kOpSelectArg1},
                         {32 + gpu::kTssAlphaArg1, kCurrent}, {32 + gpu::kTssMinFilter, 1}, {32 + gpu::kTssMagFilter, 1},
                         {64 + gpu::kTssColorOp, kOpDisable}, {64 + gpu::kTssAlphaOp, kOpDisable}};
    uint32_t rs_was[std::size(rs)], tss_was[std::size(tss)];
    for (size_t i = 0; i < std::size(rs); i++) {
        rs_was[i] = R32(kRenderState + rs[i].state * 4);
        kernel::CallGuest(ctx, kSetRenderState, {rs[i].state, rs[i].value});
    }
    for (size_t i = 0; i < std::size(tss); i++) {
        tss_was[i] = R32(kTextureState + tss[i].state * 4);
        kernel::CallGuest(ctx, kSetStageState, {tss[i].state / 32, tss[i].state % 32, tss[i].value});
    }
    const uint32_t game_vs = R32(kGameVertexShader);
    kernel::CallGuest(ctx, kSetPixelShader, {0});
    kernel::W32(kGamePixelShader, 0);
    kernel::CallGuest(ctx, kSetVertexShader, {kFvfXyzrhwTex1});
    kernel::W32(kGameVertexShader, kFvfXyzrhwTex1);
    kernel::CallGuest(ctx, kSetTexture, {0, 0});
    kernel::CallGuest(ctx, kSetTexture, {1, kPoolTexture});

    Quad(ctx, 8, 8, 192, true);
    Quad(ctx, 208, 8, 192, false);

    // the game's vertex shader back: an HLE call, so the runtime reads the quads' inline commands now. Left for Swap, they
    // were read after it had moved to the next back buffer, and cleared with it
    if (game_vs) kernel::CallGuest(ctx, kSetVertexShader, {game_vs});
    kernel::W32(kGameVertexShader, game_vs);
    kernel::CallGuest(ctx, kSetTexture, {0, texture0});
    kernel::CallGuest(ctx, kSetTexture, {1, texture1});
    for (size_t i = std::size(tss); i-- > 0;)
        kernel::CallGuest(ctx, kSetStageState, {tss[i].state / 32, tss[i].state % 32, tss_was[i]});
    for (size_t i = std::size(rs); i-- > 0;) kernel::CallGuest(ctx, kSetRenderState, {rs[i].state, rs_was[i]});
}
}  // namespace pgr2
