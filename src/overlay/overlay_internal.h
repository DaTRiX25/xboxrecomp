/*
 * overlay_internal.h - between overlay_ui.cpp (the pages, the input, the
 * ImGui frame) and a graphics backend (overlay_metal.mm, overlay_vulkan.cpp).
 *
 * A backend provides:
 *   - its init, with the objects the official imgui_impl_* renderer wants;
 *   - a draw that runs, in this order,
 *         the renderer's NewFrame, overlay_begin_frame(), overlay_end_frame(),
 *         the renderer's RenderDrawData
 *     with the draw data overlay_end_frame returns.
 * overlay_begin_frame does ImGui_ImplSDL2_NewFrame, ImGui::NewFrame and
 * builds the pages; it is false when there is nothing to draw (the backend
 * then skips the rest). overlay_end_frame is ImGui::Render.
 */
#ifndef XBOX_OVERLAY_INTERNAL_H
#define XBOX_OVERLAY_INTERNAL_H

#include "imgui.h"

/* The ImGui context and the SDL platform backend, made at the first draw.
 * `vulkan` selects ImGui_ImplSDL2_InitForVulkan. */
bool overlay_core_init(bool vulkan);
bool overlay_begin_frame();
ImDrawData *overlay_end_frame();

#endif
