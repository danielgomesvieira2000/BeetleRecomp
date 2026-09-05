# Known Issues

Open defects with what has already been ruled out, so nobody re-runs a dead experiment.
Add the negative results, not just the leads — they are the expensive part.

---

## Widescreen: the picture sits in a 275x207 island even with a full-frame scissor and viewport

**What is landed.** Framebuffer pairs whose scissor is full-width but not 4:3-shaped -- the intro's
letterboxed `320x147` pair, and a pair drawn with no scissor (RT64's unbounded `2048x2048`) -- used to
fail RT64's per-pair aspect test and were drawn unwidened and centred. They now qualify by
horizontal coverage (`BAR_ASPECT_FB_COVER`, default 90%). Measured: `adjust=1 scale=1.3333` on those
pairs afterwards.

**What is not.** In plain 4:3, with `patches/viewport_patch.c` making the game's scissor and viewport
cover the whole 320x240 (confirmed with `BAR_DBG_SCISSOR`: the only rects emitted are `(0,0)-(320,240)`
and the intro's `(0,47)-(320,194)`), the picture STILL occupies a 275x207 island: content spans
225..1050 of the 160..1120 4:3 region in a 1280x720 window, i.e. exactly 22px and ~17px of the
framebuffer on each side. In Expand the same island appears with widened slivers of world outside it
and black columns between -- the "glitched widescreen" look. The 2D HUD also sits inside the island,
but that is layout (it is authored inside the safe area), not a transform.

**Ruled out, with evidence:**

- *The RDP scissor and RSP viewport.* Full-frame after the viewport patch; `BAR_DBG_PROJ` reports the
  projection's scissor-and-viewport intersection as 319-320 wide.
- *A CPU/RDRAM copy being presented instead of the widened render.* `BAR_DBG_VI` shows the VI
  presenting `0x1DA800`/`0x200000`, and `BAR_DBG_ASPECT` shows those same buffers receiving widened
  passes (`adjust=1 scale=1.3333`).
- *The projection matrix alone.* The inset was assumed to be baked into the perspective matrix
  (`BAR_DBG_PROJ` shows BAR's 3D projections with `[3][3]=0`, `[2][3]=-0.2955`, tiny `[0][0]`).
  `barCompensateProjectionInset` (rt64, opt-in `BAR_PROJ_INSET=1`) scales every such matrix by
  320/275 x 240/207 and the log confirms it runs on all of them -- yet the island's extent does not
  change at all. A game-side patch of `func_uvimtx_rom_004004D8(Mtx)` was also tried; passing `Mtx`
  by value through a recompiled patch corrupted the matrix (the world vanished) and was dropped.
- *The last-clip-rect globals* (`D_uvgfxmgr_rom_00402408..0E`): zero-initialised `.bss`, only read
  by two fill routines; cannot shape the 3D.

**Remaining candidates, untested:** the game's own frustum culling and sky/terrain construction
(`uvterra`, `uvdyn`, `motion` -- all still assembly) building geometry to the inset frustum, so that
nothing exists to draw outside it; or a second transform between the projection and the RSP. The
next diagnostic is to log, per draw call, the NDC extent of submitted vertices rather than the
matrix, which tells whether geometry outside the island is ever submitted.

**Note:** in the meantime `BAR_CONTENT_INSET=auto` (renderer-side crop to the game's drawn region)
still removes the island's margins for presentation, at the cost described in that code.

---

## Intermittent crash in the audio thread

Seen twice while capturing automated runs, roughly 50 seconds into the attract sequence, on builds
both before and after the presentation changes -- so it is not caused by them.

```
*** CRASH code=0xc0000005 addr=... ***
  # 0 func_8000DA6C +0x2ac
  # 1 alAudioFrame +0x452
  # 2 uvAudioMgrHandleFrameMesg +0x43a
  # 3 uvAudioMgrThreadFunc +0x535
```

An access violation inside libultra's audio library (`alAudioFrame`), reached from Paradigm's audio
manager thread. `func_8000DA6C` is the unnamed leaf, and is the place to start. Not yet
investigated; recorded so the stack trace is not lost, and so a future crash here is recognised as a
recurrence rather than a new regression.

---

## RESOLVED — Intermittent permanent black screen (game boots, audio plays, no picture)

Fixed in the N64ModernRuntime fork, branch `controller-pak`, commit `9503a9b`. Recorded here
because the symptom is misleading and the mechanism is worth knowing before touching VI code again.

**Symptom.** The game boots and audio plays normally, but nothing is ever drawn. Crucially it was
*intermittent*: the same binary would run correctly once and then stay black on the next launch,
which makes it look like a configuration or GPU problem rather than a code defect.

**Cause.** `osViGetCurrentFramebuffer()` returns the current `ViState`'s `framebuffer` field
verbatim. BAR boots with `osViBlack(TRUE)` and un-blacks *only* once its gfx manager observes that
value equal to `0x100000` (`uvgfxmgr_rom.c`); until then RT64 has nothing to present, because
`VI_STATE_BLACK` forces `hStart == 0`.

An earlier change (`0c1bf21`) had made the pre-game dummy VI alternate that framebuffer field
between `0x100000` and `0x125800`, so RT64 would see a changing origin and keep presenting while the
launcher is up — without which recompui's `draw_hook` never runs and the launcher paints once and
then ignores all input. But `set_dummy_vi` only runs while `!is_game_started()`, so whichever value
it wrote *last* is what the game reads back. Whether BAR ever un-blacked therefore came down to the
parity of the final dummy frame — a coin toss, hence the intermittency.

**Fix.** `update_vi()` computes `VI_ORIGIN_REG` as `framebuffer + fldRegs[field].origin`, so the
alternation was moved into the mode's field origin (a second dummy mode, `dummy_mode_alt`). The
scanout address still changes every frame, so the launcher keeps presenting; the address the game
reads back is pinned at `0x100000`, so BAR always un-blacks.

**If you touch this again:** the two requirements genuinely conflict, and satisfying only one of
them produces a different bug — pin the framebuffer and the launcher goes inert; alternate it and
the picture dies at random. Test *both* paths: the no-frontend auto-start build must reach gameplay,
and the frontend build's launcher must still respond to input.

**Ruled out along the way** (do not re-run these):

- `RecompiledPatches/patches.c` / `PatchesLib` — disabling it entirely still produced a black screen.
- The graphics config (`ar_option`, `hr_option`, `rr_option`, `wm_option`) — the fault reproduces on
  conservative windowed values and the fix holds on fullscreen `Expand`.
- The `scripts/fix-recompiled.sh` rewrite — its end-state verification passes and rendering is
  correct with it in place.

---

## Intro / boot sequence runs too fast

**Status:** open, deferred. **Severity:** cosmetic but immediately visible.

The boot sequence plays noticeably faster than a reference emulator running the same USA ROM —
"seconds too fast", verified by side-by-side comparison. Races feel correct.

### Ruled out (with evidence)

| Hypothesis | Verdict |
|---|---|
| RT64 frame interpolation speeding the simulation | **Refuted.** `rr_option` `Display` vs `Original` both hold 60–61 SI polls/sec. |
| Game loop periodically tripling (~180 polls/sec bursts) | **Refuted.** The bursts are Controller Pak I/O inflating the counter — every spike coincides with ~60 pak transactions, every quiet second has zero. The loop is a steady 60. |
| Emulated CPU counter running fast | **Refuted.** `counter_per_ms = 46'875` is exactly the N64's 46.875 MHz count rate, and `speed_multiplier == 1`. |
| Fixed delta-time override (`D_uvgfxmgr_rom_00402480`) | **Refuted.** Probed at both return sites of `func_uvgfxmgr_rom_00401004`: the override branch is **never** taken. |
| Measured delta-time being wrong | **Refuted.** Measured delta averages **0.0163 s ≈ 1/61**, i.e. correct. ~1020 calls/sec ≈ 17 active sequences × 60 fps. |
| Whole-frame divider (`D_8001F7C0`) | **Refuted.** Reads 0 in both the menu (state 14) and a race (state 2). |
| SI completion pacing (`requeue_si`) | **Refuted.** `BAR_REQUEUE_SI=1` restores stock re-queueing: the menu becomes choppy but the animation speed is unchanged — the signature of correct delta-timing. |
| PAL/NTSC confusion (PAL runs ~17% slower) | **Refuted** by the user's side-by-side against the same USA ROM. |

### The strongest remaining lead

The UV timeline (`uvtseq_rom.c:243`) advances as `step -= rate * func_uvgfxmgr_rom_00401004()`,
which is genuinely delta-timed and measured correct. A temporary scale factor on that delta
(`BAR_TIME_SCALE`) **visibly changes the attract cinematics** — two captures at the same input
frame differ between scale 1.0 and 0.4 — but **does not change the boot logo screens at all**
(frame 1700 was byte-identical at both scales), and the user reported no perceptible change.

That splits the boot sequence in two:

- **Attract cinematics** — timeline-driven, delta-timed, responds to the scale knob.
- **Legal / logo screens** — *not* driven by this time base. Whatever paces them is elsewhere.

So if the "too fast" part is the logo/legal screens, every measurement above is consistent and the
cause has simply not been located yet. This also lines up with the pre-existing note that the
legal screens are driven by un-decompiled boot assembly whose per-frame handler was never pinned
down (see the legal-screen-skip item inherited from upstream).

### Next steps

1. Determine precisely *which* part is fast — logo/legal screens or attract — by timing each
   segment against the reference emulator with a stopwatch, rather than judging the whole boot.
2. If it is the logo screens, find their pacing source. They are likely on a `uvClkGetSec` timer
   in boot assembly; instrument that rather than the gfx-manager time base.
3. Only then decide on a fix. Do **not** reach for `speed_multiplier` or the VI clock — the
   simulation rate is measured correct and changing it would break lap times.

### Temporary scaffolding to remove

- `bar_timebase()` / `bar_dbg_timebase()` in `src/main/bar_config.cpp` (`BAR_TIME_SCALE`,
  `BAR_DBG_TIME`).
- Probe calls patched into `RecompiledFuncs/funcs_38.c` at both return sites of
  `func_uvgfxmgr_rom_00401004`. These live in **generated** code and disappear on the next
  `N64Recomp` run — re-apply manually if the investigation resumes.
- `BAR_DBG_DIV` frame-divider logging in `src/main/os_unimpl_stubs.cpp`.

`BAR_DBG_PAK` in the same file is worth keeping — it is what diagnosed the Controller Pak.
