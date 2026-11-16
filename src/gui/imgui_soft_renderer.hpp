// Software (CPU) renderer for Dear ImGui: rasterizes the draw lists into a
// 32-bit framebuffer, for displays where no OpenGL context can be had (e.g.
// remote X sessions without direct/indirect GLX). Same role as an ImGui
// renderer backend (texture requests included), no GPU involved.
#pragma once

#include <cstdint>

struct ImDrawData;

// Sets the renderer backend flags/name on the current ImGui context.
void ImGui_SoftRenderer_Init();
void ImGui_SoftRenderer_Shutdown();

// Clears the framebuffer to `clearRgb` and draws `drawData` into it.
// Pixels are 0x00RRGGBB, `width` x `height`, rows `width` pixels apart.
void ImGui_SoftRenderer_RenderDrawData(ImDrawData* drawData, uint32_t* pixels, int width, int height,
                                       uint32_t clearRgb);
