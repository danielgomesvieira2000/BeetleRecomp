// src/main/bar_frustum.cpp — draw distance, and the frustum BAR culls against.
//
// BAR builds every perspective projection in exactly one function, uvfmtx_rom's glFrustum:
//
//     func_uvfmtx_rom_00401F74(Mtx4F *dst, float left, float right, float top, float bottom,
//                              float near, float far)
//
// and its caller, uvchannel_rom's case 4, stores those same six values in the camera channel
// (unkDC..unkF0) and then calls func_uvchannel_rom_00401658, which derives the SIX CULLING PLANES
// from them. func_uvchannel_rom_004014E8 is the sphere-vs-frustum test the game asks before
// submitting anything. So one set of six numbers decides both what the projection draws and what the
// game bothers to submit — changing the matrix alone would move the far plane while the game went on
// culling everything past the old one, and nothing would appear.
//
// Measured in a race with BAR_DBG_FRUSTUM=1, two projections are built per frame:
//
//   dst=0x80099E1C  l/r = +-0.7673  t/b = +-0.5711  near = 1  far = 300     the racing camera
//   dst=0x80025920  same extents                    near = 1  far = 27000   a second, static matrix
//
// So the racing camera's draw distance is `far = 300`, and 0.7673/0.5711 = 1.343, i.e. the game's
// own frustum is 4:3 — which is the other half of this file's job, because in widescreen RT64 draws
// a wider view than that while the game keeps culling to it.
//
// Reaching the channel without a module data symbol. The channel struct is allocated on the heap by
// the relocatable uvchannel module, so its address cannot be baked in (this is the same constraint
// documented in patches/viewport_patch.c). It does not have to be: `dst` IS `&channel->unk4`, and
// unk4 is at offset 4, so the channel is `dst - 4`. Before writing anything there, all six stored
// fields are checked against the six arguments -- if the memory does not hold exactly what we were
// passed, this is not a channel and it is left alone. That guard is what keeps the second, non-channel
// projection at 0x80025920 safe.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"

namespace {

// Offsets into the channel struct (uvchannel_rom.c). unk4 is the projection matrix the caller hands
// us as `dst`; unkDC..unkF0 are left, right, top, bottom, near, far.
constexpr int32_t kChanUnk4 = 0x04;
constexpr int32_t kChanLeft = 0xDC;
constexpr int32_t kChanRight = 0xE0;
constexpr int32_t kChanTop = 0xE4;
constexpr int32_t kChanBottom = 0xE8;
constexpr int32_t kChanNear = 0xEC;
constexpr int32_t kChanFar = 0xF0;

// A projection this deep is not the racing camera -- the measured second matrix is at far = 27000 --
// and multiplying it would only cost depth precision. Anything at or beyond this is left alone.
constexpr float kSkyFarMin = 5000.0f;

float asFloat(unsigned bits) {
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

unsigned asBits(float v) {
    unsigned bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

float envScale(const char *name, float fallback, float maxValue) {
    const char *e = std::getenv(name);
    float parsed = 0.0f;
    if ((e != nullptr) && (std::sscanf(e, "%f", &parsed) == 1) && (parsed > 0.0f) && (parsed <= maxValue)) {
        return parsed;
    }
    return fallback;
}

// How far to push the far plane, as a multiple of the game's own. BAR_DRAW_DIST overrides it.
float drawDistanceScale() {
    static const float scale = envScale("BAR_DRAW_DIST", 4.0f, 64.0f);
    return scale;
}

// How much wider than its own 4:3 frustum the game should be willing to CULL against.
//
// This is the widescreen half. RT64's Expand draws a wider view than the game's frustum describes,
// but the game goes on testing objects against its own 4:3 planes, so anything that is only visible
// in the widened margins is never submitted -- it pops in as the camera turns towards it. Widening
// the planes fixes that at the source, in the game's own culling.
//
// It is deliberately NOT applied to the projection matrix, only to the planes: RT64 already widens
// the drawn view, and widening it here as well would stack and double-widen it. So the game draws
// the same 4:3 projection it always did (which RT64 then expands) while agreeing to submit what
// falls in the wider view.
//
// The default covers 21:9 (2.333 / 1.333 = 1.75) rather than just 16:9, because the error is not
// symmetric: culling slightly too wide costs a few extra draws that are then clipped away, whereas
// culling too narrow is the visible defect this exists to remove. BAR_CULL_WIDEN=1 disables it.
float cullWidenScale() {
    static const float scale = envScale("BAR_CULL_WIDEN", 1.75f, 4.0f);
    return scale;
}

bool traceEnabled() {
    static const bool dbg = std::getenv("BAR_DBG_FRUSTUM") != nullptr;
    return dbg;
}

// Widening the channel's stored left/right is not confined to the culling planes: the game builds a
// SECOND projection from the same fields on a later frame (measured at dst=0x80025920, near=1,
// far=27000 -- the distant/sky matrix), and it picked up the widened sides, rendering that layer at
// aspect 2.35 against the main view's 1.34.
//
// So remember the exact bit patterns written, and whenever a projection is built from them, hand the
// originals back for the matrix. Matching on the bits is exact -- a value we did not write cannot
// collide with one we did -- so no projection is narrowed by accident. The culling planes still see
// the widened values, which is the whole point.
struct WidenedPair {
    unsigned widenedLeft, widenedRight, origLeft, origRight;
};
constexpr int kWidenedSlots = 8;
WidenedPair gWidened[kWidenedSlots] = {};
int gWidenedNext = 0;

void rememberWidened(unsigned wl, unsigned wr, unsigned ol, unsigned orr) {
    for (int i = 0; i < kWidenedSlots; i++) {
        if ((gWidened[i].widenedLeft == wl) && (gWidened[i].widenedRight == wr)) {
            return;
        }
    }
    gWidened[gWidenedNext] = { wl, wr, ol, orr };
    gWidenedNext = (gWidenedNext + 1) % kWidenedSlots;
}

bool restoreWidened(unsigned *l, unsigned *r) {
    for (int i = 0; i < kWidenedSlots; i++) {
        if ((gWidened[i].widenedLeft == *l) && (gWidened[i].widenedRight == *r) &&
            (gWidened[i].widenedLeft != gWidened[i].origLeft)) {
            *l = gWidened[i].origLeft;
            *r = gWidened[i].origRight;
            return true;
        }
    }
    return false;
}

} // namespace

// Called from the top of the recompiled func_uvfmtx_rom_00401F74, before it reads its arguments.
// The six values are passed by pointer so they can be rewritten in place; the caller writes them
// back into the registers and stack slots the function is about to load them from.
extern "C" void bar_frustum_adjust(uint8_t *rdram, unsigned dst, unsigned *l, unsigned *r,
                                   unsigned *t, unsigned *b, unsigned *n, unsigned *f) {
    // If this projection is being built from sides we widened for culling, build it from the
    // originals instead -- see the note on WidenedPair.
    restoreWidened(l, r);

    const float left = asFloat(*l);
    const float right = asFloat(*r);
    const float nearZ = asFloat(*n);
    const float farZ = asFloat(*f);

    if (traceEnabled()) {
        static unsigned last[7] = { 1, 1, 1, 1, 1, 1, 1 };
        const unsigned cur[7] = { dst, *l, *r, *t, *b, *n, *f };
        if (std::memcmp(last, cur, sizeof(cur)) != 0) {
            std::memcpy(last, cur, sizeof(cur));
            std::fprintf(stderr,
                         "[frustum] dst=%08X l=%.4f r=%.4f t=%.4f b=%.4f near=%.4f far=%.4f (aspect=%.3f)\n",
                         dst, left, right, asFloat(*t), asFloat(*b), nearZ, farZ,
                         (asFloat(*b) != asFloat(*t)) ? ((right - left) / (asFloat(*b) - asFloat(*t))) : 0.0f);
            std::fflush(stderr);
        }
    }

    const float distScale = drawDistanceScale();
    const float widenScale = cullWidenScale();
    const bool wantDistance = (distScale != 1.0f) && (farZ > 0.0f) && (farZ < kSkyFarMin) && (nearZ > 0.0f);
    const bool wantWiden = (widenScale != 1.0f) && (right > left);
    if (!wantDistance && !wantWiden) {
        return;
    }

    const float newFar = wantDistance ? (farZ * distScale) : farZ;

    // The far plane goes into the projection matrix this call is about to build, so the extra
    // distance is actually drawn. The widened sides deliberately do not -- see cullWidenScale().
    if (wantDistance) {
        *f = asBits(newFar);
    }

    // Both go into the channel, which is what func_uvchannel_rom_00401658 derives the culling planes
    // from a moment later. Only when the memory really is a channel holding exactly these six values.
    const int64_t chan = (int64_t)(int32_t)(dst - kChanUnk4);
    const bool isChannel =
        ((unsigned)MEM_W(kChanLeft, chan) == *l) && ((unsigned)MEM_W(kChanRight, chan) == *r) &&
        ((unsigned)MEM_W(kChanTop, chan) == *t) && ((unsigned)MEM_W(kChanBottom, chan) == *b) &&
        ((unsigned)MEM_W(kChanNear, chan) == *n) && ((unsigned)MEM_W(kChanFar, chan) == asBits(farZ));
    if (isChannel) {
        if (wantDistance) {
            MEM_W(kChanFar, chan) = (int32_t)asBits(newFar);
        }
        if (wantWiden) {
            const float centre = (left + right) * 0.5f;
            const float halfWidth = (right - left) * 0.5f * widenScale;
            const unsigned widenedLeft = asBits(centre - halfWidth);
            const unsigned widenedRight = asBits(centre + halfWidth);
            MEM_W(kChanLeft, chan) = (int32_t)widenedLeft;
            MEM_W(kChanRight, chan) = (int32_t)widenedRight;
            rememberWidened(widenedLeft, widenedRight, *l, *r);
        }
    }

    if (traceEnabled()) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(stderr,
                         "[frustum] draw distance x%.2f (far %.1f -> %.1f), cull widen x%.2f; channel %s\n",
                         distScale, farZ, newFar, widenScale,
                         isChannel ? "found, planes follow" : "NOT found, planes unchanged");
            std::fflush(stderr);
        }
    }
}
