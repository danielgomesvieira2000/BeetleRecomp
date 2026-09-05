// The display-list rewriter. See dlrewrite.h for what it is for and the switches it honours.
//
// How it works, in order:
//
// 1. The walk. Every command of the frame's list is copied into a scratch list, following G_DL
//    branches in place and recursing into pushed sub-lists only to measure them. Along the way it
//    tracks what the RSP would: segment table, the modelview stack, the projection, the current
//    viewport and scissor, and the current texture image.
//
// 2. The 2D layer. A draw is 2D when it happens under an orthographic projection (BAR's world
//    projections have m[3][3] == 0, its 2D ones m[3][3] == 1), or when it is an RDP rectangle
//    (texture rectangles, which in F3DEX2 are three commands, and fill rectangles). Each is
//    classified by its horizontal extent on the 320-wide screen: an element covering the frame is
//    stretched across the widened frame; otherwise, when anchoring is on and the frame is a race,
//    an element whose centre lies in the left third is anchored to the left edge and one in the
//    right third to the right edge; everything else stays where the game put it, in the middle.
//    A tag table (hud.json in the settings folder) overrides the class per texture or sub-list.
//
// 3. Emission. RT64 anchors vertex geometry by the origin carried on its viewport, anchors
//    rectangles by gEXSetRectAlign, and stretches by an aspect flag on the projection's matrix
//    group -- all per projection or viewport rather than per draw -- so each change of class
//    inserts the alignment command and then re-issues the game's own viewport or projection
//    command after it. RT64 displaces an anchored viewport by the origin's fraction of the
//    framebuffer width; origin_cancel() undoes that so the game's viewport data is used unchanged
//    and only the anchor moves.
//
// 4. Memory. RDRAM in this runtime stores 32-bit words natively, so a command is two host words at
//    its physical offset and 16-bit values sit at their offset XOR 2 -- which is also how RT64
//    reads them. The scratch list lives at physical 0xC00000: the game is told it has 8 MB and
//    never looks above it, the runtime allocates far more, and RT64 addresses 16 MB.
//
// Frame identification is read from the frame itself rather than from a game-state variable: a
// race draws its world first, under a perspective-style projection, and then switches to an
// orthographic projection for its HUD. Anchoring is only applied to frames of that shape.
//
// Adapted from wave-race-64-recomp's src/dlrewrite.cpp (Fast3D); opcodes and encodings here are
// F3DEX2, which is what BAR's uvgfxmgr loads (gspF3DEX2_fifo).
#include "dlrewrite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "librecomp/game.hpp"
#include "ultramodern/config.hpp"
#include "json/json.hpp"

// BAR runs F3DEX2, and rt64_extended_gbi.h picks the hook opcode from this: 0xE0 (G_SPNOOP) for
// F3DEX2, 0x00 otherwise. Without it every rewritten list opened with an 0x00 command carrying the
// hook magic, RT64 did not recognise it, and the whole list was dropped -- a black screen even in
// trace-only mode.
#ifndef F3DEX_GBI_2
#define F3DEX_GBI_2
#endif
#include "rt64_extended_gbi.h"

namespace {

constexpr uint32_t kScratch = 0x80C00000u;       // above the 8 MB the game sees, inside RT64's 16 MB
constexpr uint32_t kScratchSize = 0x40000u;      // 256 KB: 32768 commands
constexpr uint32_t kMaxCommands = kScratchSize / 8;

// F3DEX2 opcodes the walk has to understand. Everything else is copied.
constexpr uint8_t kOpVtx = 0x01;
constexpr uint8_t kOpPopMtx = 0xD8;
constexpr uint8_t kOpMtx = 0xDA;
constexpr uint8_t kOpMoveWord = 0xDB;
constexpr uint8_t kOpMoveMem = 0xDC;
constexpr uint8_t kOpDisplayList = 0xDE;
constexpr uint8_t kOpEndDisplayList = 0xDF;
constexpr uint8_t kOpRdpHalf1 = 0xE1;
constexpr uint8_t kOpRdpHalf2 = 0xF1;
constexpr uint8_t kOpSetScissor = 0xED;         // RDP, 10.2 fixed point
constexpr uint8_t kOpSetTextureImage = 0xFD;
constexpr uint8_t kOpTexRect = 0xE4;
constexpr uint8_t kOpTexRectFlip = 0xE5;
constexpr uint8_t kOpFillRect = 0xF6;

bool is_rect(uint8_t op) {
    return op == kOpTexRect || op == kOpTexRectFlip || op == kOpFillRect;
}

// G_MTX parameters as stored in the command: gsSPMatrix XORs the push bit, so bit 0 CLEAR means push.
constexpr uint32_t kMtxProjection = 0x04;
constexpr uint32_t kMtxLoad = 0x02;
constexpr uint32_t kMtxNoPush = 0x01;
constexpr uint8_t kMoveWordSegment = 0x06;
constexpr uint8_t kMoveMemViewport = 0x08;

constexpr int kFramebufferWidth = 320;

int origin_cancel(uint32_t origin) {
    return -static_cast<int>((origin * kFramebufferWidth * 4) / G_EX_ORIGIN_RIGHT);
}

constexpr float kAnchorThird = 1.0f / 3.0f;

struct RaceTest {
    bool first_projection_seen = false;
    bool world_first = false;
    bool seen_ortho = false;
    // BAR submits more than one list per frame, and the HUD can arrive in a list of its own with no
    // world in it. Judged on that list alone the frame is "not a race" and its elements go back to
    // the centre, so they would jump between anchored and centred on alternate lists. The frame's
    // shape is therefore remembered across the last few lists (set by rewrite()).
    bool recent_world = false;
    bool race() const { return (world_first || recent_world) && seen_ortho; }
};

enum class Class { Auto, Left, Right, Stretch };

const char* class_name(Class c) {
    switch (c) {
        case Class::Left:    return "left";
        case Class::Right:   return "right";
        case Class::Stretch: return "stretch";
        default:             return "center";
    }
}

// The tag table: hud.json in the settings folder. Lists of identities -- "tex:0x01004A20" for a
// texture, "dl:0x0106F8A0" for a sub-list, as the trace prints them -- under "left", "right",
// "center" and "stretch".
struct Tags {
    std::unordered_map<std::string, Class> by_identity;
    bool loaded = false;

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    void load() {
        loaded = true;
        const std::filesystem::path path = recomp::get_config_path() / "hud.json";
        std::ifstream in(path);
        if (!in) {
            std::ofstream out(path);
            if (out) {
                out << "{\n"
                       "    \"help\": \"Run with BAR_HUD_TRACE=1 to list 2D draws with their identity"
                       " (tex:... or dl:...) and the class they were given; put an identity in one"
                       " of the lists below to override it.\",\n"
                       "    \"left\": [],\n"
                       "    \"right\": [],\n"
                       "    \"center\": [],\n"
                       "    \"stretch\": []\n"
                       "}\n";
            }
            return;
        }
        nlohmann::json doc;
        try {
            in >> doc;
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[bar] hud.json could not be read: %s\n", e.what());
            return;
        }
        const std::pair<const char*, Class> lists[] = {
            { "left", Class::Left }, { "right", Class::Right },
            { "center", Class::Auto }, { "stretch", Class::Stretch },
        };
        int count = 0;
        for (const auto& [key, cls] : lists) {
            if (!doc.contains(key) || !doc[key].is_array()) continue;
            for (const auto& item : doc[key]) {
                if (item.is_string()) {
                    by_identity[lower(item.get<std::string>())] = cls;
                    ++count;
                }
            }
        }
        if (count > 0) {
            std::fprintf(stderr, "[bar] hud.json: %d tagged elements\n", count);
        }
    }

    bool lookup(const std::string& identity, Class& out) const {
        const auto it = by_identity.find(lower(identity));
        if (it == by_identity.end()) return false;
        out = it->second;
        return true;
    }
};

struct Mat4 {
    float m[4][4];
    static Mat4 identity() {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }
    // Row-vector convention, as the N64's matrices are: v' = v * M.
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }
};

// The clip-space extent of a set of vertices, or of a rectangle.
struct Extent {
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    float min_w = 1e9f, max_w = -1e9f;      // clip-space w across the vertices
    bool empty() const { return max_x < min_x; }
    float left_px() const { return (min_x + 1.0f) * (kFramebufferWidth / 2.0f); }
    float right_px() const { return (max_x + 1.0f) * (kFramebufferWidth / 2.0f); }
    float center_x() const { return (min_x + max_x) / 2.0f; }
    // A screen-aligned element: every vertex at the same depth, so the same w. World geometry
    // drawn under the same perspective projection spans a range of w. This is what separates
    // BAR's HUD quads from the track around them.
    bool flat() const {
        if (max_w < min_w) return false;
        const float mag = std::max(std::abs(max_w), std::abs(min_w));
        return (max_w - min_w) <= 0.01f * std::max(mag, 1e-6f);
    }
    void add(const Extent& o) {
        min_x = std::min(min_x, o.min_x); max_x = std::max(max_x, o.max_x);
        min_y = std::min(min_y, o.min_y); max_y = std::max(max_y, o.max_y);
        min_w = std::min(min_w, o.min_w); max_w = std::max(max_w, o.max_w);
    }
};

struct Walker {
    uint8_t* rdram = nullptr;
    uint32_t segments[16] = {};
    uint32_t* out = nullptr;
    uint32_t written = 0;       // commands
    bool overflow = false;
    bool anchors = false;       // HUD ratio setting asks for edges
    bool hud = true;            // the 2D layer is handled at all
    bool noemit = false;        // classify and trace, but change nothing
    float inset = 0.0f;         // how far the visible picture's edge lies inside RT64's frame
    const Tags* tags = nullptr;
    std::string* trace = nullptr;

    bool have_viewport = false;
    uint32_t viewport_w0 = 0, viewport_w1 = 0;
    bool have_projection = false;
    uint32_t projection_w0 = 0, projection_w1 = 0;
    bool have_scissor_cmd = false;
    uint32_t scissor_w0 = 0, scissor_w1 = 0;
    bool scissor_widened = false;

    Mat4 projection = Mat4::identity();
    Mat4 projection_only = Mat4::identity();
    bool projection_is_perspective = false;
    Mat4 modelview[16];
    int modelview_depth = 0;
    uint32_t texture = 0;
    RaceTest race_test;
    Class cls = Class::Auto;
    uint32_t class_changes = 0;
    uint32_t draws_2d = 0;
    int scissor_left = 0x7FFF, scissor_top = 0x7FFF, scissor_right = -1, scissor_bottom = -1;

    Walker() { modelview[0] = Mat4::identity(); }

    uint32_t physical(uint32_t segmented) const {
        const uint32_t base = segments[(segmented >> 24) & 0xF];
        return (base + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }
    const uint32_t* words(uint32_t physical_addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (physical_addr & 0x00FFFFFFu));
    }
    uint16_t half(uint32_t physical_addr) const {
        return *reinterpret_cast<const uint16_t*>(rdram + ((physical_addr & 0x00FFFFFFu) ^ 2));
    }
    int16_t signed_half(uint32_t physical_addr) const {
        return static_cast<int16_t>(half(physical_addr));
    }
    Mat4 read_matrix(uint32_t physical_addr) const {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            const uint32_t full = (uint32_t(half(physical_addr + 2 * i)) << 16) |
                                  half(physical_addr + 32 + 2 * i);
            r.m[i / 4][i % 4] = static_cast<float>(static_cast<int32_t>(full)) / 65536.0f;
        }
        return r;
    }

    // Which projections are the world: any matrix whose w depends on z (m[3][3] == 0, m[2][3] != 0).
    // BAR's world uses several of these -- m[2][3] of -0.30 for the track and -1 for other passes --
    // and RT64 classes all of them as perspective. A version that took only |m[2][3]| == 1 as the
    // world treated the track's own projection as 2D and stretched the terrain across the frame.
    // BAR's genuine 2D projections are affine (m[3][3] == 1). The HUD, however, is largely drawn
    // UNDER the world projection as screen-aligned quads; those are told apart by flatness (see
    // Extent::flat), not by the projection.
    static bool perspective(const Mat4& p) {
        const float w = p.m[3][3];
        const float pz = p.m[2][3];
        return w > -1e-4f && w < 1e-4f && (pz > 1e-6f || pz < -1e-6f);
    }

    Extent extent(uint32_t vtx_physical, uint32_t count, const Mat4& mv) const {
        const Mat4 mvp = mv * projection;
        Extent e;
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            const uint32_t v = vtx_physical + i * 16;
            const float x = static_cast<float>(signed_half(v + 0));
            const float y = static_cast<float>(signed_half(v + 2));
            const float z = static_cast<float>(signed_half(v + 4));
            const float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
            const float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
            const float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
            if (cw == 0.0f) continue;
            e.min_x = std::min(e.min_x, cx / cw);
            e.max_x = std::max(e.max_x, cx / cw);
            e.min_y = std::min(e.min_y, cy / cw);
            e.max_y = std::max(e.max_y, cy / cw);
            e.min_w = std::min(e.min_w, cw);
            e.max_w = std::max(e.max_w, cw);
        }
        return e;
    }

    void emit(uint32_t w0, uint32_t w1) {
        if (written >= kMaxCommands) { overflow = true; return; }
        out[written * 2] = w0;
        out[written * 2 + 1] = w1;
        ++written;
    }
    GfxCommand* reserve(uint32_t count) {
        if (written + count > kMaxCommands) { overflow = true; return nullptr; }
        GfxCommand* cmd = reinterpret_cast<GfxCommand*>(out + written * 2);
        written += count;
        return cmd;
    }

    void align_viewport(uint32_t origin, int offset_x) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXSetViewportAlign(cmd, origin, offset_x, 0);
        }
        if (have_viewport) emit(viewport_w0, viewport_w1);
    }
    void projection_group(uint32_t aspect) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXMatrixGroup(cmd, G_EX_ID_AUTO, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, 1,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_ORDER_AUTO, G_EX_EDIT_NONE, aspect,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
        }
    }
    void projection_aspect(uint32_t aspect) {
        projection_group(aspect);
        if (have_projection) emit(projection_w0, projection_w1);
    }
    void leave_class(bool before_projection_load) {
        switch (cls) {
            case Class::Stretch:
                if (before_projection_load) projection_group(G_EX_ASPECT_AUTO);
                else projection_aspect(G_EX_ASPECT_AUTO);
                break;
            case Class::Left:
            case Class::Right:
                align_viewport(G_EX_ORIGIN_NONE, 0);
                break;
            default:
                break;
        }
        cls = Class::Auto;
    }
    void set_class(Class next) {
        if (next == cls) return;
        leave_class(false);
        const int inset_q = static_cast<int>(inset * 4.0f);
        switch (next) {
            case Class::Stretch:
                projection_aspect(G_EX_ASPECT_STRETCH);
                break;
            case Class::Left:
                align_viewport(G_EX_ORIGIN_LEFT, origin_cancel(G_EX_ORIGIN_LEFT) + inset_q);
                break;
            case Class::Right:
                align_viewport(G_EX_ORIGIN_RIGHT, origin_cancel(G_EX_ORIGIN_RIGHT) - inset_q);
                break;
            default:
                break;
        }
        cls = next;
        ++class_changes;
    }

    // ---- rectangles ----
    // Consecutive rectangles that sit on the same row and near each other are one element (a row of
    // digits, a bar), classified together so they cannot split between two classes.
    Class rect_cls = Class::Auto;
    std::vector<std::pair<uint32_t, uint32_t>> pending;
    std::vector<uint32_t> pending_textures;
    Extent pending_extent;
    bool pending_active = false;

    // Commands that may sit between the rectangles of one element without ending it: the texture
    // rectangle's own two trailing halves, texture setup, loads, syncs and colours.
    static bool run_command(uint8_t op) {
        switch (op) {
            case kOpTexRect: case kOpTexRectFlip: case kOpFillRect:
            case kOpRdpHalf1: case kOpRdpHalf2:
            case 0xFD: case 0xF5: case 0xF2: case 0xF3: case 0xF4: case 0xF0:
            case 0xE6: case 0xE7: case 0xE8: case 0xE9:
            case 0xFA: case 0xFB: case 0xFC: case 0xF7: case 0xF8: case 0xF9:
            case 0xE2: case 0xE3: case 0xEF:
                return true;
            default:
                return false;
        }
    }
    static bool chains(const Extent& run, const Extent& r) {
        const float gap = 12.0f / 160.0f;
        const bool overlap_y = r.min_y <= run.max_y && r.max_y >= run.min_y;
        const bool near_x = r.min_x <= run.max_x + gap && r.max_x >= run.min_x - gap;
        return overlap_y && near_x;
    }
    void push_rect(uint32_t w0, uint32_t w1) {
        const Extent r = rect_extent(w0, w1);
        if (pending_active && !chains(pending_extent, r)) flush_run();
        if (!pending_active) {
            pending_active = true;
            pending_extent = r;
            pending_textures.clear();
            pending_textures.push_back(texture);
        }
        else {
            pending_extent.add(r);
            if (std::find(pending_textures.begin(), pending_textures.end(), texture) == pending_textures.end()) {
                pending_textures.push_back(texture);
            }
        }
        pending.emplace_back(w0, w1);
    }
    void flush_run() {
        if (pending_active) classify_rect(pending_extent, 0, pending_textures);
        for (const auto& [w0, w1] : pending) emit(w0, w1);
        pending.clear();
        pending_active = false;
    }
    // An anchored rectangle would be clipped by the game's 320-wide scissor; widen it with origins.
    void widen_scissor(bool widen) {
        if (widen == scissor_widened || !have_scissor_cmd) return;
        scissor_widened = widen;
        if (widen) {
            const uint8_t mode = static_cast<uint8_t>((scissor_w1 >> 24) & 3);
            const int ulx = static_cast<int>((scissor_w0 >> 12) & 0xFFF) >> 2;
            const int uly = static_cast<int>(scissor_w0 & 0xFFF) >> 2;
            const int lrx = static_cast<int>((scissor_w1 >> 12) & 0xFFF) >> 2;
            const int lry = static_cast<int>(scissor_w1 & 0xFFF) >> 2;
            if (GfxCommand* cmd = reserve(2)) {
                gEXSetScissor(cmd, mode, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT,
                              ulx, uly, lrx - kFramebufferWidth, lry);
            }
        }
        else {
            emit(scissor_w0, scissor_w1);
        }
    }
    void set_rect_class(Class next) {
        if (next == rect_cls) return;
        widen_scissor(next == Class::Left || next == Class::Right);
        if (rect_cls == Class::Stretch) {
            if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_AUTO);
        }
        const int inset_q = static_cast<int>(inset * 4.0f);
        switch (next) {
            case Class::Left:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_LEFT, inset_q, 0, inset_q, 0);
                }
                break;
            case Class::Right: {
                const int off = origin_cancel(G_EX_ORIGIN_RIGHT) - inset_q;
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_RIGHT, G_EX_ORIGIN_RIGHT, off, 0, off, 0);
                }
                break;
            }
            case Class::Stretch:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
                }
                if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_STRETCH);
                break;
            default:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
                }
                break;
        }
        rect_cls = next;
        ++class_changes;
    }
    // Rectangle corners: lrx/lry in the first word, ulx/uly in the second, 10.2 fixed point.
    static Extent rect_extent(uint32_t w0, uint32_t w1) {
        const float ulx = float((w1 >> 12) & 0xFFF) / 4.0f;
        const float uly = float(w1 & 0xFFF) / 4.0f;
        const float lrx = float((w0 >> 12) & 0xFFF) / 4.0f;
        const float lry = float(w0 & 0xFFF) / 4.0f;
        Extent e;
        e.min_x = ulx / 160.0f - 1.0f;
        e.max_x = lrx / 160.0f - 1.0f;
        e.min_y = 1.0f - lry / 120.0f;
        e.max_y = 1.0f - uly / 120.0f;
        return e;
    }
    void classify_rect(const Extent& e, uint32_t dl_segmented, const std::vector<uint32_t>& textures) {
        ++draws_2d;
        const std::string tex_id = hex_identity("tex", textures.empty() ? texture : textures.front());
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        Class next = classify(e, tex_id, dl_id, /*screen_element=*/true);   // an RDP rectangle is always screen-space
        for (uint32_t t : textures) {
            Class tagged;
            if (tags != nullptr && tags->lookup(hex_identity("tex", t), tagged)) {
                next = (tagged == Class::Stretch || anchors) ? tagged : Class::Auto;
                break;
            }
        }
        if (trace != nullptr) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "[hud] rect %s %s px x[%.0f..%.0f] y[%.0f..%.0f] under %s -> %s\n",
                          tex_id.c_str(), dl_id.empty() ? "dl:-" : dl_id.c_str(),
                          (e.min_x + 1.0f) * 160.0f, (e.max_x + 1.0f) * 160.0f,
                          (1.0f - e.max_y) * 120.0f, (1.0f - e.min_y) * 120.0f,
                          projection_is_perspective ? "persp" : "ortho", class_name(next));
            trace->append(line);
        }
        if (!noemit) set_rect_class(next);
    }

    bool covers_width(const Extent& e) const {
        const float left = has_scissor() ? float(scissor_left) : 0.0f;
        const float right = has_scissor() ? float(scissor_right) : float(kFramebufferWidth);
        return (e.right_px() - e.left_px()) >= 0.9f * (right - left);
    }
    // `screen_element` says the draw is a screen-aligned element even though it sits under the world
    // projection (Extent::flat). Under a perspective projection nothing is ever stretched -- the
    // aspect flag is per projection group, and stretching the world's magnifies the picture -- and
    // only flat elements are anchored.
    Class classify(const Extent& e, const std::string& identity, const std::string& identity2, bool screen_element = false) {
        Class tagged;
        if (tags != nullptr && (tags->lookup(identity, tagged) || tags->lookup(identity2, tagged))) {
            if (tagged == Class::Stretch && projection_is_perspective) return Class::Auto;
            if (tagged == Class::Stretch || anchors) return tagged;
            return Class::Auto;
        }
        if (e.empty()) return Class::Auto;
        if (covers_width(e) && !projection_is_perspective) return Class::Stretch;
        if (projection_is_perspective && !screen_element) return Class::Auto;
        if (!anchors || !race_test.race()) return Class::Auto;
        if (e.min_x < -1.0f || e.max_x > 1.0f || e.min_y < -1.0f || e.max_y > 1.0f) return Class::Auto;
        const float c = e.center_x();
        if (c < -kAnchorThird) return Class::Left;
        if (c > kAnchorThird) return Class::Right;
        return Class::Auto;
    }
    static std::string hex_identity(const char* kind, uint32_t address) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s:0x%08X", kind, address);
        return buf;
    }
    // A vertex draw. Under the world projection only a flat (screen-aligned) draw is a 2D element;
    // the rest is the world and is neither counted nor touched.
    void classify_draw(const Extent& e, uint32_t dl_segmented) {
        const bool screen_element = e.flat();
        if (projection_is_perspective && !screen_element) {
            // Not a 2D element by the flatness test. Traced when it sits within the screen, so a HUD
            // element that fails the test (a quad whose depth varies a little) can be recognised and
            // the threshold or a tag adjusted.
            if (trace != nullptr && !e.empty() && e.min_x > -1.5f && e.max_x < 1.5f && e.min_y > -1.5f && e.max_y < 1.5f) {
                char line[220];
                std::snprintf(line, sizeof(line),
                              "[hud] deep %s %s clip x[%.2f..%.2f] y[%.2f..%.2f] w[%.3f..%.3f]\n",
                              hex_identity("tex", texture).c_str(),
                              dl_segmented != 0 ? hex_identity("dl", dl_segmented).c_str() : "dl:-",
                              e.min_x, e.max_x, e.min_y, e.max_y, e.min_w, e.max_w);
                trace->append(line);
            }
            return;
        }
        ++draws_2d;
        const std::string tex_id = hex_identity("tex", texture);
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        const Class next = classify(e, tex_id, dl_id, screen_element);
        if (trace != nullptr) {
            char line[220];
            std::snprintf(line, sizeof(line),
                          "[hud] draw %s %s clip x[%.2f..%.2f] y[%.2f..%.2f] w[%.2f..%.2f] under %s -> %s\n",
                          tex_id.c_str(), dl_id.empty() ? "dl:-" : dl_id.c_str(),
                          e.empty() ? 0.0f : e.min_x, e.empty() ? 0.0f : e.max_x,
                          e.empty() ? 0.0f : e.min_y, e.empty() ? 0.0f : e.max_y,
                          e.empty() ? 0.0f : e.min_w, e.empty() ? 0.0f : e.max_w,
                          projection_is_perspective ? "persp" : "ortho", class_name(next));
            trace->append(line);
        }
        if (!noemit) set_class(next);
    }

    // Measures a pushed sub-list without emitting it: the clip-space extent of its vertex loads
    // under the current projection, and the extent of any rectangles it draws.
    Extent scan(uint32_t physical_addr, int depth, Mat4 mv[16], int& mv_depth, Extent& rects) {
        Extent e;
        uint32_t cursor = physical_addr;
        for (uint32_t steps = 0; steps < 4096; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            if (op == kOpEndDisplayList) break;
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) { cursor = physical(w1); continue; }
                if (depth < 4) e.add(scan(physical(w1), depth + 1, mv, mv_depth, rects));
                continue;
            }
            if (op == kOpSetTextureImage) { texture = w1; continue; }
            if (is_rect(op)) { rects.add(rect_extent(w0, w1)); continue; }
            if (op == kOpPopMtx) { if (mv_depth > 0) --mv_depth; continue; }
            if (op == kOpMtx) {
                const uint32_t params = w0 & 0xFF;
                if (params & kMtxProjection) continue;    // rare in a called list; ignored
                const Mat4 loaded = read_matrix(physical(w1));
                if (!(params & kMtxNoPush) && mv_depth < 15) { mv[mv_depth + 1] = mv[mv_depth]; ++mv_depth; }
                mv[mv_depth] = (params & kMtxLoad) ? loaded : loaded * mv[mv_depth];
                continue;
            }
            if (op == kOpVtx) {
                const uint32_t count = (w0 >> 12) & 0xFF;
                e.add(extent(physical(w1), count, mv[mv_depth]));
            }
        }
        return e;
    }

    static bool affine(const Mat4& m) {
        return m.m[0][3] == 0.0f && m.m[1][3] == 0.0f && m.m[2][3] == 0.0f && m.m[3][3] == 1.0f;
    }
    void on_matrix(uint32_t w0, uint32_t w1) {
        const uint32_t params = w0 & 0xFF;
        const Mat4 loaded = read_matrix(physical(w1));
        if (params & kMtxProjection) {
            if (params & kMtxLoad) {
                projection = loaded;
                projection_only = loaded;
                have_projection = true;
                projection_w0 = w0;
                projection_w1 = w1;
            }
            else {
                projection = loaded * projection;
                if (!affine(loaded)) projection_only = loaded * projection_only;
            }
            projection_is_perspective = perspective(projection_only);
            if (!race_test.first_projection_seen) {
                race_test.first_projection_seen = true;
                race_test.world_first = projection_is_perspective;
            }
            if (!projection_is_perspective) race_test.seen_ortho = true;
            return;
        }
        if (!(params & kMtxNoPush) && modelview_depth < 15) {
            modelview[modelview_depth + 1] = modelview[modelview_depth];
            ++modelview_depth;
        }
        modelview[modelview_depth] = (params & kMtxLoad) ? loaded : loaded * modelview[modelview_depth];
    }
    void on_scissor(uint32_t w0, uint32_t w1) {
        const int ulx = static_cast<int>((w0 >> 12) & 0xFFF) >> 2;
        const int uly = static_cast<int>(w0 & 0xFFF) >> 2;
        const int lrx = static_cast<int>((w1 >> 12) & 0xFFF) >> 2;
        const int lry = static_cast<int>(w1 & 0xFFF) >> 2;
        scissor_left = std::min(scissor_left, ulx);
        scissor_top = std::min(scissor_top, uly);
        scissor_right = std::max(scissor_right, lrx);
        scissor_bottom = std::max(scissor_bottom, lry);
    }
    bool has_scissor() const {
        return scissor_right > scissor_left && scissor_bottom > scissor_top;
    }
};

}  // namespace

namespace bar::dlrewrite {

uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr) {
    static const bool disabled = std::getenv("BAR_NO_REWRITE") != nullptr;
    static const bool hud_off = std::getenv("BAR_HUD_OFF") != nullptr;
    static const bool tracing = std::getenv("BAR_HUD_TRACE") != nullptr;
    static const bool noemit = std::getenv("BAR_HUD_NOEMIT") != nullptr;
    static const bool anchors_disabled = std::getenv("BAR_HUD_NO_ANCHORS") != nullptr;
    static Tags tags;
    static uint32_t traced_lists = 0;
    static std::string trace_text;

    if (disabled) return 0;
    if (!tags.loaded) tags.load();

    // Diagnostics for a list RT64 will not render: BAR_HUD_SCRATCH_LOW=1 puts the scratch list at
    // 0x700000, inside the 8 MB the game sees (where wave-race keeps its own), and
    // BAR_HUD_NO_ENABLE=1 leaves out the extended-GBI enable command at the head of the list.
    static const bool scratch_low = std::getenv("BAR_HUD_SCRATCH_LOW") != nullptr;
    static const bool no_enable = std::getenv("BAR_HUD_NO_ENABLE") != nullptr;
    const uint32_t scratch = scratch_low ? 0x80700000u : kScratch;

    const auto& config = ultramodern::renderer::get_graphics_config();

    Walker w{};
    w.rdram = rdram;
    w.hud = !hud_off;
    w.noemit = noemit;
    w.anchors = !anchors_disabled && config.hr_option != ultramodern::renderer::HUDRatioMode::Original;
    w.inset = 0.0f;
    w.tags = &tags;
    static uint32_t lists_since_world = 1000;   // how many lists ago a world (perspective-first) list was seen
    w.race_test.recent_world = lists_since_world <= 2;
    w.out = reinterpret_cast<uint32_t*>(rdram + (scratch & 0x00FFFFFFu));
    if (tracing) {
        trace_text.clear();
        w.trace = &trace_text;
    }

    if (!no_enable) {
        if (GfxCommand* cmd = w.reserve(1)) gEXEnable(cmd);
    }

    uint32_t cursor = list_vaddr & 0x00FFFFFFu;
    for (uint32_t steps = 0; steps < kMaxCommands && !w.overflow; ++steps) {
        const uint32_t* c = w.words(cursor);
        const uint32_t w0 = c[0];
        const uint32_t w1 = c[1];
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        cursor += 8;

        if (w.hud && is_rect(op)) { w.push_rect(w0, w1); continue; }
        if (w.pending_active && Walker::run_command(op)) {
            if (op == kOpSetTextureImage) w.texture = w1;
            w.pending.emplace_back(w0, w1);
            continue;
        }
        w.flush_run();

        if (op == kOpEndDisplayList) {
            w.set_rect_class(Class::Auto);
            w.leave_class(false);
            w.emit(w0, w1);
            break;
        }
        if (op == kOpDisplayList) {
            const bool branch = ((w0 >> 16) & 0xFF) != 0;
            if (branch) { cursor = w.physical(w1); continue; }
            // Measured under either projection: under the world's, classify_draw keeps only flat
            // (screen-aligned) sub-lists, which is how BAR draws most of its HUD.
            if (w.hud && w.have_projection) {
                Mat4 mv[16];
                std::memcpy(mv, w.modelview, sizeof(mv));
                int depth = w.modelview_depth;
                Extent rects;
                const Extent e = w.scan(w.physical(w1), 0, mv, depth, rects);
                if (!e.empty()) w.classify_draw(e, w1);
                if (!rects.empty()) w.classify_rect(rects, w1, {});
            }
            w.emit(w0, w1);
            continue;
        }
        // F3DEX2 G_MOVEWORD (gsImmp21): index in the low byte, offset in bits 8..23. A segment
        // set is index 6 with offset = segment * 4. (Fast3D packs these the other way round, and a
        // first version copied that: every segment landed wrong and the walk followed branches into
        // garbage -- the screen went black.)
        if (op == kOpMoveWord && (w0 & 0xFF) == kMoveWordSegment) {
            const uint32_t offset = (w0 >> 8) & 0xFFFF;
            w.segments[(offset / 4) & 0xF] = w1 & 0x00FFFFFFu;
            w.emit(w0, w1);
            continue;
        }
        if (op == kOpMoveMem && (w0 & 0xFF) == kMoveMemViewport) {
            w.have_viewport = true;
            w.viewport_w0 = w0;
            w.viewport_w1 = w1;
            w.emit(w0, w1);
            continue;
        }
        if (op == kOpSetScissor) {
            w.on_scissor(w0, w1);
            w.have_scissor_cmd = true;
            w.scissor_w0 = w0;
            w.scissor_w1 = w1;
            w.emit(w0, w1);
            continue;
        }
        if (op == kOpSetTextureImage) { w.texture = w1; w.emit(w0, w1); continue; }
        if (op == kOpPopMtx) { if (w.modelview_depth > 0) --w.modelview_depth; w.emit(w0, w1); continue; }
        if (op == kOpMtx) {
            const uint32_t params = w0 & 0xFF;
            const bool projection_load = (params & kMtxProjection) && (params & kMtxLoad);
            if (projection_load && w.cls != Class::Auto) w.leave_class(true);
            if (projection_load && w.rect_cls != Class::Auto) w.set_rect_class(Class::Auto);
            w.on_matrix(w0, w1);
            if (projection_load && w.trace != nullptr) {
                char line[160];
                std::snprintf(line, sizeof(line), "[hud] projection load %s [1][1]=%.4f [2][3]=%.4f [3][3]=%.3f (viewport %s)\n",
                              w.projection_is_perspective ? "perspective" : "orthographic",
                              w.projection.m[1][1], w.projection.m[2][3], w.projection.m[3][3],
                              w.have_viewport ? "seen" : "not yet seen");
                w.trace->append(line);
            }
            w.emit(w0, w1);
            continue;
        }
        if (op == kOpVtx && w.hud && w.have_projection) {
            const uint32_t count = (w0 >> 12) & 0xFF;
            const Extent e = w.extent(w.physical(w1), count, w.modelview[w.modelview_depth]);
            w.classify_draw(e, 0);
            w.emit(w0, w1);
            continue;
        }
        w.emit(w0, w1);
    }

    if (w.overflow) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[bar] a display list did not fit the rewriter's scratch space; passed through unchanged\n");
            std::fflush(stderr);
        }
        return 0;
    }

    lists_since_world = w.race_test.world_first ? 0 : lists_since_world + 1;

    static uint32_t last_draws = 0xFFFFFFFFu;
    const bool composition_changed = w.draws_2d != last_draws;
    last_draws = w.draws_2d;
    if (w.trace != nullptr && w.written >= 30 && (traced_lists < 6 || composition_changed)) {
        ++traced_lists;
        std::fprintf(stderr, "[hud] ---- list %u: %u commands, %u 2D draws, %u class changes, anchors %s, race %s"
                             " (world-first %d, ortho %d, scissor %d,%d-%d,%d) ----\n%s",
                     traced_lists, w.written, w.draws_2d, w.class_changes, w.anchors ? "on" : "off",
                     w.race_test.race() ? "yes" : "no", w.race_test.world_first ? 1 : 0,
                     w.race_test.seen_ortho ? 1 : 0, w.scissor_left, w.scissor_top, w.scissor_right,
                     w.scissor_bottom, trace_text.c_str());
        std::fflush(stderr);
    }

    static uint32_t lists = 0;
    if (++lists == 1 || lists == 600) {
        std::fprintf(stderr, "[bar] display list rewritten: %u commands, %u class changes (from 0x%08X to 0x%08X)\n",
                     w.written, w.class_changes, list_vaddr, scratch);
        // Copy fidelity: the first commands of the original beside the first commands of the copy.
        // The copy should be the original shifted by the one enable command at its head.
        const uint32_t* src = w.words(list_vaddr & 0x00FFFFFFu);
        for (int i = 0; i < 6; i++) {
            std::fprintf(stderr, "[bar]   orig[%d]=%08X %08X   copy[%d]=%08X %08X\n",
                         i, src[i * 2], src[i * 2 + 1], i, w.out[i * 2], w.out[i * 2 + 1]);
        }
        std::fflush(stderr);
    }
    return scratch;
}

namespace {

// Sits between ultramodern and whichever renderer context is in use (RT64 directly in the
// auto-start build, RecompFrontend's in the frontend build) so every display list is rewritten
// before RT64 sees it. Everything else is forwarded untouched.
class RewritingContext final : public ultramodern::renderer::RendererContext {
public:
    RewritingContext(uint8_t* rdram, std::unique_ptr<ultramodern::renderer::RendererContext> inner)
        : rdram_(rdram), inner_(std::move(inner)) {
        setup_result = inner_->get_setup_result();
        chosen_api = inner_->get_chosen_api();
    }

    bool valid() override { return inner_->valid(); }
    ultramodern::renderer::SetupResult get_setup_result() const override { return inner_->get_setup_result(); }
    ultramodern::renderer::GraphicsApi get_chosen_api() const override { return inner_->get_chosen_api(); }
    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        return inner_->update_config(old_config, new_config);
    }
    void enable_instant_present() override { inner_->enable_instant_present(); }
    void send_dummy_workload(uint32_t fb_address) override { inner_->send_dummy_workload(fb_address); }
    void update_screen() override { inner_->update_screen(); }
    void shutdown() override { inner_->shutdown(); }
    uint32_t get_display_framerate() const override { return inner_->get_display_framerate(); }
    float get_resolution_scale() const override { return inner_->get_resolution_scale(); }

    void send_dl(const OSTask* task) override {
        const uint32_t rewritten = rewrite(rdram_, static_cast<uint32_t>(task->t.data_ptr));
        if (rewritten == 0) { inner_->send_dl(task); return; }
        OSTask copy = *task;
        copy.t.data_ptr = rewritten;
        inner_->send_dl(&copy);
    }

private:
    uint8_t* rdram_;
    std::unique_ptr<ultramodern::renderer::RendererContext> inner_;
};

}  // namespace

std::unique_ptr<ultramodern::renderer::RendererContext>
wrap(uint8_t* rdram, std::unique_ptr<ultramodern::renderer::RendererContext> inner) {
    if (!inner) return inner;
    return std::make_unique<RewritingContext>(rdram, std::move(inner));
}

}  // namespace bar::dlrewrite
