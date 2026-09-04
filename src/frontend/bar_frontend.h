#pragma once

// BeetleRecomp <-> RecompFrontend glue.
//
// RecompFrontend (recompui + recompinput) is the launcher/menu and input layer extracted from
// Zelda 64: Recompiled. It replaces the bespoke src/ui menu; see docs/PINNED_REVISIONS.md for the
// build constraints. Everything here is compiled only when -DBEETLE_ENABLE_FRONTEND=ON.

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace bar::frontend {

// Register program identity, fonts and the launcher layout with recompui. Must be called BEFORE
// recomp::start(), because recompui builds its menus during renderer bring-up.
void install();

// Render-context factory for ultramodern's renderer_callbacks.create_render_context.
//
// recompui ships its own RT64Context (it has to, since it draws the UI over the game), so the port
// uses that instead of src/main/rt64_render_context.cpp when the frontend is enabled. The runtime's
// callback is (rdram, window_handle, developer_mode) while recompui's factory also takes a
// presentation mode, so this adapts between them — and that extra parameter is exactly where BAR's
// requirement is expressed: the main-menu film-roll is a VI-origin pan that never redraws, so it
// only animates in RT64's Console presentation mode.
std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

} // namespace bar::frontend
