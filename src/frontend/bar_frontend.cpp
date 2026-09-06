#include "frontend/bar_frontend.h"

#include <cstdio>

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"
#include "recompinput/input_events.h"
#include "recompinput/profiles.h"
#include "librecomp/game.hpp"
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

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

// Implemented in src/main/bar_frustum.cpp, which owns the frustum the game draws and culls with.
extern "C" void bar_set_draw_distance(float scale);

// Draw Distance — a BAR-specific option appended to recompui's prefab Graphics tab.
//
// It is added from here rather than from RecompFrontend because that submodule is upstream
// (N64Recomp/RecompFrontend) and this port cannot push to it; the tab's Config is returned by
// create_graphics_tab(), so an extra option can simply be added to it. It has to happen BEFORE
// recompui::config::finalize(), which is what loads the JSON — an option added afterwards would
// never see its saved value.
//
// The value reaches the renderer through bar_set_draw_distance (src/main/bar_frustum.cpp), which
// multiplies the far plane of BAR's own view frustum — and, because the game derives its culling
// planes from the same numbers, the distance it is willing to submit geometry for.
namespace {

// Keys are what land in graphics.json, so they must stay stable; the third field is the label.
const std::vector<recomp::config::ConfigOptionEnumOption> kDrawDistanceOptions = {
    { 0u, "1x", "1x" },
    { 1u, "2x", "2x" },
    { 2u, "4x", "4x" },
};

const std::string kDrawDistanceOption = "draw_distance";

float draw_distance_scale_for(uint32_t index) {
    switch (index) {
        case 0:  return 1.0f;
        case 1:  return 2.0f;
        default: return 4.0f;
    }
}

void add_draw_distance_option() {
    recomp::config::Config &config = recompui::config::get_config(recompui::config::graphics::id);
    config.add_enum_option(
        kDrawDistanceOption,
        "Draw Distance",
        "How far into the distance the game draws. The original hardware drew 300 units ahead; "
        "<recomp-color primary>2x</recomp-color> and <recomp-color primary>4x</recomp-color> extend "
        "that, and extend the distance the game submits geometry for to match, so scenery stops "
        "appearing out of the fog.",
        kDrawDistanceOptions,
        2u /* 4x, matching the port's default */
    );
}

// Read the applied (not pending) value and hand it to the renderer. Called after finalize() and once
// per frame from pump_events, rather than through set_save_callback, because the Graphics tab has
// already installed its own save callback and replacing it would stop every other graphics setting
// applying. A map lookup per frame is far cheaper than that bug.
void push_draw_distance() {

    const recomp::config::ConfigValueVariant value =
        recompui::config::get_config(recompui::config::graphics::id).get_option_value(kDrawDistanceOption);
    if (const uint32_t *index = std::get_if<uint32_t>(&value)) {
        bar_set_draw_distance(draw_distance_scale_for(*index));
    }
}

} // namespace

void bar::frontend::install() {
    // BAR_DBG_UI=1 sends the frontend's diagnostics to bar_ui_trace.log in the working directory.
    // Worth keeping: this build links as /SUBSYSTEM:WINDOWS, so it has no console and shell
    // redirection of stderr captures nothing — without this, frontend problems are silent.
    if (std::getenv("BAR_DBG_UI") != nullptr) {
        static std::FILE* log = std::freopen("bar_ui_trace.log", "w", stderr);
        (void)log;
    }

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
    add_draw_distance_option();
    recompui::config::create_sound_tab();
    recompui::config::create_controls_tab();
    recompui::config::finalize();

    // finalize() is what reads graphics.json, so the saved Draw Distance only exists after it.
    push_draw_distance();

    // Input bindings. This is what makes the menus DRIVEABLE, and nothing else calls it:
    // load_controls_config() is the only public entry point that reaches
    // profiles::initialize_input_bindings(), which builds the key/button -> menu-action mapping
    // recompui navigates with. Without it the UI still renders and RmlUi still highlights on hover
    // (its own hit-testing), but no key or button maps to Accept/Back/navigate, so nothing can be
    // activated — the menu looks alive and is completely inert.
    //
    // The call also seeds sensible keyboard and controller defaults when the file does not exist
    // yet, and writes controls.json alongside the other config files.
    const std::filesystem::path controls_path = recomp::get_config_path() / "controls.json";
    const bool loaded = recompinput::profiles::load_controls_config(controls_path);
    trace(loaded ? "controls config loaded" : "controls config created from defaults");

    trace("install: done");
}


void bar::frontend::pump_events() {
    // recompinput owns the SDL pump: it polls, filters, forwards to recompui, and applies cursor
    // visibility / relative-mouse mode from recompui's flag (recompui itself never calls SDL).
    recompinput::handle_events();

    // Pick up Draw Distance as soon as the player applies it. See push_draw_distance().
    push_draw_distance();
}

bool bar::frontend::menu_capturing_input() {
    return recompui::is_context_capturing_input();
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
