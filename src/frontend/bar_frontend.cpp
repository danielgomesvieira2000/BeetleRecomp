#include "frontend/bar_frontend.h"

#include <cstdio>

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"

namespace {

// Must match the recomp::GameEntry registered in main.cpp — recompui drives the launcher's
// "Load ROM" / "Start Game" state from librecomp's validation of this id, which is in turn backed by
// the entry's XXH3-64 rom_hash. A mismatch here silently yields a launcher that can never start.
constexpr const char8_t* kGameId      = u8"bar.n64.us";
constexpr const char*    kModGameId   = "bar";
constexpr const char*    kDisplayName = "Beetle Adventure Racing!";

// Assets are resolved as <exe dir>/assets/<name> (recompui::file::get_asset_path). The primary font
// is REQUIRED: recompui throws "No primary font was registered" without it. The family name is what
// the stylesheet must reference, and is not necessarily the filename.
constexpr const char* kPrimaryFontFile   = "LatoLatin-Regular.ttf";
constexpr const char* kPrimaryFontFamily = "LatoLatin";   // the font file's real family name

// TEMPORARY bring-up tracing. The frontend build currently exits during renderer/UI setup with no
// diagnostic, so these mark how far the sequence gets. Remove once it starts cleanly.
void trace(const char* msg) {
    std::fprintf(stderr, "[frontend] %s\n", msg);
    std::fflush(stderr);
}

} // namespace

void bar::frontend::install() {
    trace("install: begin");

    recompui::programconfig::set_program_name(kDisplayName);
    recompui::programconfig::set_program_id(kGameId);

    recompui::register_primary_font(kPrimaryFontFile, kPrimaryFontFamily);

    // Built when recompui creates its menus. The library owns the ROM flow entirely:
    // add_start_game_or_load_rom_option() shows "Load ROM" until librecomp reports a valid ROM for
    // kGameId and "Start Game" afterwards, with a native file picker behind it — so ROM loading and
    // hash verification need no code on this side.
    recompui::register_launcher_init_callback([](recompui::LauncherMenu* menu) {
        trace("launcher init callback: begin");

        recompui::GameOptionsMenu* options = menu->init_game_options_menu(
            kGameId, kModGameId, kDisplayName,
            {},                                   // no thumbnail bundled yet
            recompui::GameOptionsMenuLayout::Center);

        options->add_start_game_or_load_rom_option();
        options->add_setup_controls_option();
        options->add_settings_option();
        // Mod support is deliberately out of scope for the beta, so no add_mods_option() yet.
        options->add_exit_option();

        trace("launcher init callback: menu built");
    });

    // Config tabs. recompui's create_menus() calls config::init_modal(), which throws
    // "Configurations have not been loaded. Call recompui::config::finalize() first." unless the
    // tabs exist and finalize() has run — so this must happen here, before recomp::start() brings
    // the renderer up. Each tab persists to its own <id>.json in the app config directory.
    //
    // The prefab tabs cover everything the beta needs, so no bespoke options yet. Rumble strength is
    // enabled because this port serves the rumble motor register alongside the Controller Pak, so a
    // pad really can vibrate; gyro and mouse are off, since BAR has no use for either. The mods tab
    // is deliberately omitted while mod support is out of scope.
    recompui::config::create_general_tab(recompui::config::GeneralTabOptions{
        .has_rumble_strength  = true,
        .has_gyro_sensitivity = false,
        .has_mouse_sensitivity = false,
    });
    recompui::config::create_graphics_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_controls_tab();
    recompui::config::finalize();

    trace("install: done");
}

std::unique_ptr<ultramodern::renderer::RendererContext>
bar::frontend::create_render_context(uint8_t* rdram,
                                     ultramodern::renderer::WindowHandle window_handle,
                                     bool developer_mode) {
    // Console presentation: present strictly from the VI origin, at VI time, exactly as hardware
    // does. BAR's main-menu film-roll transition steps the VI origin across a framebuffer without
    // ever redrawing, so any mode that presents only freshly-rendered content collapses the pan to
    // an instant swap. Do not "optimise" this to PresentEarly/SkipBuffering.
    trace("create_render_context: begin");

    auto ctx = recompui::renderer::create_render_context(
        rdram, window_handle, ultramodern::renderer::PresentationMode::Console, developer_mode);

    trace(ctx ? "create_render_context: ok" : "create_render_context: returned null");
    return ctx;
}
