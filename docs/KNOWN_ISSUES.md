# Known Issues

Open defects with what has already been ruled out, so nobody re-runs a dead experiment.
Add the negative results, not just the leads — they are the expensive part.

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
