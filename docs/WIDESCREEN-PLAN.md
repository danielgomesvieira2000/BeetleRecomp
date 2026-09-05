# Widescreen plan

Goal: BAR renders a genuinely **wider view** in 16:9 and 21:9 — more of the world at the sides, not
a stretched or cropped 4:3 image — with the HUD correctly placed and no geometry popping at the new
edges.

This plan is grounded in the decomp rather than in generic advice, because BAR does not use
libultra's `guPerspective` at all. What follows names the actual functions.

## What the game actually does

Searching the decomp for `guPerspective`, `guFrustum` or `guOrtho` returns **nothing**. BAR builds
its own matrices in Paradigm's UV middleware, which changes the whole shape of this job.

| Concern | Where it lives | Why it matters |
|---|---|---|
| **Projection matrix** | `func_uvimtx_rom_004004D8(Mtx mtx)` — `uvIMtxPush(mtx, G_MTX_PROJECTION \| G_MTX_LOAD)` | A caller-supplied, already-built matrix pushed through **one** function. A single choke point for FOV. |
| **Viewport + scissor** | `uvGfxClipRect(vp, x0,y0,x1,y1)` then `func_uvgfxmgr_rom_00401C5C(vp_id)` — emits `gSPViewport` + `gDPSetScissor` | Scissor is computed against `sScreenHeight`; viewports live in an array of up to 11 (`D_uvgfxmgr_rom_004022C8`), which is also how split-screen works. |
| **Screen dimensions** | `uvSetScreenWidth` / `uvSetScreenHeight`, `uvGetScreenWidth` / `uvGetScreenHeight` | The game's own notion of screen size, distinct from RT64's output size. |
| **Perspective normalisation** | `func_uvgfxmgr_rom_00401D94(s16 persp)` — `gSPPerspNormalize` | Must stay consistent with any projection change or W-buffering/fog goes wrong. |
| **2D / HUD** | `uvsprt_rom`, `uvfont_rom`, `uvblit_rom` | Screen-space, authored against 320×240. |

That the projection funnels through a single function is the most useful fact here: widening the
frustum is potentially a **one-function patch**, not a hunt through call sites.

## The decision that comes first

There are two independent ways to widen the view, and **they must not both be applied** — stacking
them double-widens and looks wrong.

1. **RT64's `ar_option = Expand`** (already enabled). The renderer widens the frustum at the HLE
   level, with no game code involved. Free, already on, and does not know anything about BAR's
   culling.
2. **Game-side projection patch** at `func_uvimtx_rom_004004D8`. Scales the projection's horizontal
   term by `(4/3) / target_aspect` before it is pushed. More invasive, but it is *the game's own*
   frustum, so culling, LOD and any other frustum-derived logic see the wider view too.

Option 1 is already in place, so the plan starts by finding out whether it is sufficient rather
than by writing code.

## Phase W1 — Assess what Expand already gives us

Cheap, and it determines whether W2 is needed at all. Play a race and answer three questions:

1. **Is the view genuinely wider?** Compare a 16:9 capture against a 4:3 one at the same spot: is
   there *more scenery* at the sides, or the same scene stretched/zoomed?
2. **Does geometry pop at the left/right edges?** Drive past trackside objects and watch the
   margins. This is the risk the whole plan hinges on: if the game culls against its own 4:3
   frustum while RT64 draws a wider one, objects vanish exactly where they become visible.
3. **Is the HUD right?** Speedometer circular not oval, map unsquashed, timer not stretched.
   `hr_option` is now `Original` rather than `Expand`, which should already fix the stretching seen
   on BAR's own film-strip menu border.

Capture note: **use a DPI-aware process**. A non-aware one sees a virtualised 1536×864 view of a
1920×1080 window and produces images that look shifted and clipped — that artefact has already been
mistaken once for a rendering bug.

**Outcome A — all three pass.** Widescreen is essentially done; skip to W4 (HUD polish) and W5.
**Outcome B — popping at the edges.** Go to W2 and W3; this is the expected case.

## Phase W2 — Widen the game's own frustum

Only if W1 shows popping, or the view is not genuinely wider.

Patch `func_uvimtx_rom_004004D8` with `RECOMP_PATCH`/`RECOMP_HOOK` — the toolchain for this is set
up and proven (see "Building MIPS patches" in `docs/PORT-STATUS.md`). Scale the horizontal term of
the incoming projection matrix by `(4/3) / aspect`, preserving vertical FOV, so the change is Hor+
— players see *more* at the sides rather than *less* at top and bottom, which is what a racing game
wants.

Two things to get right:

- **`Mtx` is fixed-point**, split into integer and fractional halves, not floats. Scaling has to be
  done on the fixed-point representation or reconstructed through it.
- **Turn RT64's `Expand` off** at the same time, or the two stack. This is the choke point where the
  two mechanisms meet, and the reason the decision above comes first.

Take the target aspect from the host rather than hardcoding 16:9, so 21:9 works and windowed
resizing behaves.

## Phase W3 — Viewport, scissor and culling

**Viewport and scissor** widen with the projection or a correct image is simply clipped back to 4:3.
Both come from `uvGfxClipRect` + `func_uvgfxmgr_rom_00401C5C`, with the scissor derived from
`sScreenHeight`. The viewport array holds up to 11 entries, so **each split-screen viewport must be
handled independently** — a single global fix will be wrong for two-player.

**Culling** is the schedule risk and cannot be read statically with confidence, because the relevant
track and object code is still largely `GLOBAL_ASM`. The approach is empirical:

1. Reproduce popping reliably (a specific track and corner).
2. Find the test by instrumenting rather than reading — the recompiled functions can be traced, and
   `BAR_DBG_*` diagnostics already exist for this style of work.
3. Widen the extent it compares against, ideally derived from the same aspect value as W2.

**Fallback if culling proves intractable:** widen the frustum but clamp it to what the game's own
culling already accepts. That yields a modestly wider view with no popping — worse than full
widescreen, better than a broken one. Decide by time spent, not by principle.

## Phase W4 — HUD and 2D (built on `wip/hud-anchoring`; parked because it glitches races)

**Status (6 Sep 2026).** The rewriter described below was built and anchoring was verified in a race,
but with it enabled the car and background flicker and jitter. The main branch is back on the
pre-anchoring build; the code, the rt64 change it needs, the verified result, the likely causes and
the steps to bring it back are all in `docs/KNOWN_ISSUES.md`, "HUD anchoring in widescreen".


Measured facts that fix the design:

- Without extended-GBI origins (BAR emits none), RT64 places a 2D rectangle by scaling it about the
  framebuffer centre by the aspect scale (`convertFixedRect`, origin `NONE`), i.e. 2D rect draws are
  **stretched** in Expand; orthographic triangle draws get the inverse scale on their projection and
  stay 4:3-centred. So what the HUD looks like today depends on which primitive each element uses.
- `hr_option` (RT64 `extAspectRatio`) only moves elements that carry an extended-GBI origin, so for
  BAR it is currently inert.

The approach is the one `wave-race-64-recomp` uses in `src/dlrewrite.cpp`: a **host-side display-list
rewriter** that classifies each 2D draw by where it sits on the 320-wide screen -- left third anchored
to the left edge, right third to the right, full-frame elements stretched, the rest centred -- and
injects `gEXSetViewportAlign` / projection-group commands so RT64 anchors them, with a per-texture
override table (`hud.json`) for the exceptions and a HUD Placement setting to turn anchoring on. For
BAR: speedometer and lap counter to the left edge, timer, map and position to the right, the "GO!"
and race messages centred. No game patching needed for this step.

## Phase W5 — Verification

- Every track, both split-screen viewports, at 16:9 **and** 21:9.
- No popping at the margins while driving.
- HUD proportions correct at both ratios.
- Lap times unchanged — this phase must not touch anything time-based.

## Explicitly out of scope

Frame interpolation (that is the high-FPS phase) and the intro timing issue, which is documented
with its refuted hypotheses in `docs/KNOWN_ISSUES.md`.
