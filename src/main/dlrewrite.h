// The display-list rewriter: anchors BAR's 2D layer to the window's edges in widescreen.
//
// RT64 can pin a 2D element to the left or right edge of a widened frame, or stretch it across
// the frame, but only when the display list carries its extended-GBI alignment commands. BAR
// emits none, so this pass sits between the runtime and RT64, walks each frame's F3DEX2 list,
// classifies every 2D draw by where it sits on the 320-wide screen, and writes a copy of the
// list with the alignment commands inserted. The game's own list is never modified.
//
// Modelled on wave-race-64-recomp's src/dlrewrite.cpp, re-targeted from Fast3D to F3DEX2.
//
//   BAR_NO_REWRITE=1    pass every list through untouched
//   BAR_HUD_OFF=1       walk, but leave the 2D layer alone
//   BAR_HUD_NOEMIT=1    classify and trace, but insert nothing (the diagnostic mode)
//   BAR_HUD_NO_ANCHORS=1 never anchor to an edge (stretching of full-width elements still applies)
//   BAR_HUD_TRACE=1     print each 2D draw with its identity, extent and class, for hud.json
#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace bar::dlrewrite {

// Returns the virtual address of the rewritten list, or 0 to submit the original unchanged.
uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr);

// Wraps a renderer context so every display list passes through rewrite() before send_dl().
std::unique_ptr<ultramodern::renderer::RendererContext>
wrap(uint8_t* rdram, std::unique_ptr<ultramodern::renderer::RendererContext> inner);

}  // namespace bar::dlrewrite
