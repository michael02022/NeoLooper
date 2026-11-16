// See imgui_soft_renderer.hpp. ImGui only ever draws textured, vertex-
// colored triangles inside clip rectangles, blended "source over": that is
// all this rasterizer does - edge functions with a top-left fill rule (no
// pixel drawn twice on a shared edge), perspective-free interpolation,
// bilinear texture sampling (ImGui's baked anti-aliased lines rely on it).
#include "imgui_soft_renderer.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace {

struct Color {
    float r, g, b, a; // 0..1
};

Color unpack(ImU32 c) { // IM_COL32 = A B G R from the high byte down
    return {float(c & 0xFF) / 255.f, float((c >> 8) & 0xFF) / 255.f,
            float((c >> 16) & 0xFF) / 255.f, float((c >> 24) & 0xFF) / 255.f};
}

// Bilinear sample of an RGBA32 / Alpha8 texture at normalized (u, v).
Color sample(const ImTextureData* tex, float u, float v) {
    if (!tex || !tex->Pixels) return {1, 1, 1, 1};
    const int w = tex->Width, h = tex->Height;
    const float x = u * w - 0.5f, y = v * h - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    auto texel = [&](int tx, int ty) -> Color {
        tx = std::clamp(tx, 0, w - 1);
        ty = std::clamp(ty, 0, h - 1);
        if (tex->Format == ImTextureFormat_Alpha8)
            return {1, 1, 1, float(((const unsigned char*)tex->Pixels)[ty * w + tx]) / 255.f};
        const unsigned char* p = (const unsigned char*)tex->Pixels + (ty * w + tx) * 4;
        return {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f};
    };
    const Color c00 = texel(x0, y0), c10 = texel(x0 + 1, y0);
    const Color c01 = texel(x0, y0 + 1), c11 = texel(x0 + 1, y0 + 1);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    auto mix = [&](float a00, float a10, float a01, float a11) {
        return lerp(lerp(a00, a10, fx), lerp(a01, a11, fx), fy);
    };
    return {mix(c00.r, c10.r, c01.r, c11.r), mix(c00.g, c10.g, c01.g, c11.g),
            mix(c00.b, c10.b, c01.b, c11.b), mix(c00.a, c10.a, c01.a, c11.a)};
}

inline void blend(uint32_t& dst, float r, float g, float b, float a) {
    if (a <= 0.f) return;
    if (a >= 1.f) {
        dst = (uint32_t(r * 255.f + 0.5f) << 16) | (uint32_t(g * 255.f + 0.5f) << 8) |
              uint32_t(b * 255.f + 0.5f);
        return;
    }
    const float ia = 1.f - a;
    const float dr = float((dst >> 16) & 0xFF), dg = float((dst >> 8) & 0xFF),
                db = float(dst & 0xFF);
    dst = (uint32_t(r * 255.f * a + dr * ia + 0.5f) << 16) |
          (uint32_t(g * 255.f * a + dg * ia + 0.5f) << 8) | uint32_t(b * 255.f * a + db * ia + 0.5f);
}

struct Target {
    uint32_t* pixels;
    int width, height;
};

// Edge function of (a, b) at p; > 0 on the left of a->b for our winding.
inline float edge(const ImVec2& a, const ImVec2& b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

// Top-left rule: a pixel center exactly on an edge belongs to the triangle
// only for top or left edges, so two triangles sharing an edge never both
// draw it (no double blending along a rectangle's diagonal).
inline bool isTopLeft(const ImVec2& a, const ImVec2& b) {
    return (a.y == b.y && b.x < a.x) || (b.y < a.y);
}

void drawTriangle(const Target& t, const ImDrawVert& v0, ImDrawVert v1, ImDrawVert v2,
                  const ImTextureData* tex, int cx0, int cy0, int cx1, int cy1) {
    float area = edge(v0.pos, v1.pos, v2.pos.x, v2.pos.y);
    if (area == 0.f) return;
    if (area < 0.f) { // make the winding consistent
        std::swap(v1, v2);
        area = -area;
    }
    const ImVec2 &p0 = v0.pos, &p1 = v1.pos, &p2 = v2.pos;
    const int x0 = std::max(cx0, (int)std::floor(std::min({p0.x, p1.x, p2.x})));
    const int y0 = std::max(cy0, (int)std::floor(std::min({p0.y, p1.y, p2.y})));
    const int x1 = std::min(cx1, (int)std::ceil(std::max({p0.x, p1.x, p2.x})));
    const int y1 = std::min(cy1, (int)std::ceil(std::max({p0.y, p1.y, p2.y})));
    if (x0 >= x1 || y0 >= y1) return;

    const bool tl0 = isTopLeft(p1, p2), tl1 = isTopLeft(p2, p0), tl2 = isTopLeft(p0, p1);
    const Color c0 = unpack(v0.col), c1 = unpack(v1.col), c2 = unpack(v2.col);
    const bool flatColor = v0.col == v1.col && v1.col == v2.col;
    // Solid fills point all three UVs at the atlas' white texel: one sample.
    const bool flatUv = v0.uv.x == v1.uv.x && v0.uv.x == v2.uv.x && v0.uv.y == v1.uv.y &&
                        v0.uv.y == v2.uv.y;
    const Color flatTex = flatUv ? sample(tex, v0.uv.x, v0.uv.y) : Color{1, 1, 1, 1};
    const float invArea = 1.f / area;

    for (int y = y0; y < y1; ++y) {
        const float py = y + 0.5f;
        uint32_t* row = t.pixels + (size_t)y * t.width;
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f;
            const float w0 = edge(p1, p2, px, py), w1 = edge(p2, p0, px, py),
                        w2 = edge(p0, p1, px, py);
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            if ((w0 == 0.f && !tl0) || (w1 == 0.f && !tl1) || (w2 == 0.f && !tl2)) continue;
            const float l0 = w0 * invArea, l1 = w1 * invArea, l2 = w2 * invArea;
            Color c = flatColor ? c0
                                : Color{c0.r * l0 + c1.r * l1 + c2.r * l2, c0.g * l0 + c1.g * l1 + c2.g * l2,
                                        c0.b * l0 + c1.b * l1 + c2.b * l2, c0.a * l0 + c1.a * l1 + c2.a * l2};
            const Color tc = flatUv ? flatTex
                                    : sample(tex, v0.uv.x * l0 + v1.uv.x * l1 + v2.uv.x * l2,
                                             v0.uv.y * l0 + v1.uv.y * l1 + v2.uv.y * l2);
            blend(row[x], c.r * tc.r, c.g * tc.g, c.b * tc.b, c.a * tc.a);
        }
    }
}

// ImGui 1.92 texture requests: the pixels stay in ImTextureData (CPU
// memory), so "uploading" is just handing out an id that points back at it.
void updateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        tex->SetTexID((ImTextureID)(intptr_t)tex);
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

} // namespace

void ImGui_SoftRenderer_Init() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_soft_renderer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
}

void ImGui_SoftRenderer_Shutdown() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
}

void ImGui_SoftRenderer_RenderDrawData(ImDrawData* dd, uint32_t* pixels, int width, int height,
                                       uint32_t clearRgb) {
    std::fill(pixels, pixels + (size_t)width * height, clearRgb);
    if (!dd) return;
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK) updateTexture(tex);

    const Target t{pixels, width, height};
    const ImVec2 off = dd->DisplayPos;
    for (const ImDrawList* list : dd->CmdLists) {
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            const int cx0 = std::max(0, (int)std::floor(cmd.ClipRect.x - off.x));
            const int cy0 = std::max(0, (int)std::floor(cmd.ClipRect.y - off.y));
            const int cx1 = std::min(width, (int)std::ceil(cmd.ClipRect.z - off.x));
            const int cy1 = std::min(height, (int)std::ceil(cmd.ClipRect.w - off.y));
            if (cx0 >= cx1 || cy0 >= cy1) continue;
            const auto* tex = (const ImTextureData*)(intptr_t)cmd.GetTexID();
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                ImDrawVert a = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + i]];
                ImDrawVert b = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + i + 1]];
                ImDrawVert c = vtx[cmd.VtxOffset + idx[cmd.IdxOffset + i + 2]];
                a.pos.x -= off.x, a.pos.y -= off.y;
                b.pos.x -= off.x, b.pos.y -= off.y;
                c.pos.x -= off.x, c.pos.y -= off.y;
                drawTriangle(t, a, b, c, tex, cx0, cy0, cx1, cy1);
            }
        }
    }
}
