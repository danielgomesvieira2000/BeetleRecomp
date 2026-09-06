# Racing draw map — which draw makes which thing on screen

A running record of "what means what" on screen: each entry ties a class of draw call (measured in
the renderer) to the element the player actually sees. Built by experiment — a switch is flipped, the
frame is captured, **and Daniel says what he saw change in the live window**, because a single
captured frame does not show everything (a captured race frame will never tell you that the
track-select screen lost its text).

Add to this rather than rediscovering it. Every experiment should leave a fact behind.

## How the measurements are taken

* `BAR_DBG_RECT=1` (RT64 fork, `RDP::drawRect`) logs every **distinct** 2D rectangle the game draws,
  in framebuffer pixels, with the cycle type, whether texturing is on, the current fill colour, the
  scissor in force and the framebuffer width.
* `BAR_DBG_PROJ=1` logs each projection with its type (1 = perspective, 2 = orthographic,
  3 = rectangle), its width, and whether the widescreen adjustment was applied to it.
* `BAR_SKIP_WIDE=<px>` drops textured rectangles at least that wide — a probe for "which element is
  made of wide texture rectangles".
* Reaching a real race headlessly (verified recipe, ~110 s):

  ```
  BAR_SKIP_LAUNCHER=1 BAR_WINDOW_SIZE=1280x720 BAR_NO_AUDIO=1 \
    BAR_SHOTS="6100:race.png" \
    BAR_AUTOPLAY="250:0 30:8000 1120:0 20:1000 1480:0 20:8000 280:0 20:8000 280:0 20:8000 \
                  280:0 20:8000 280:0 20:8000 280:0 20:8000 280:0 20:8000 200:0 20:8000 1200:0" \
    ./BeetleRecomp.exe
  ```

  That is: A on the Controller Pak prompt, START to skip the intro movie, then six A presses through
  Race Type (Single Race) → Opponents (Full Grid) → Difficulty (Easy) → Track (Coventry Cove) →
  Car → Transmission, then one more A for the Rumble Pak prompt. Frame ~5700 is "GO!", ~6100 is
  racing. **Give `BAR_SHOTS` Windows-style paths** (`C:/...`); an MSYS `/c/...` path is written
  somewhere else and the files never appear.

## The map

| What is drawn | How it is drawn | Where it lands in widescreen |
|---|---|---|
| **Overscan mask** (the black frame around the picture) | Four opaque black (`fill=0x00010001`) fill rectangles, in fill cycle, that are the complement of the drawn area: `(0,0)-(320,17)`, `(0,224)-(320,240)`, `(0,17)-(21,223)`, `(296,17)-(320,223)` | Top and bottom span the whole scissor width, so RT64 stretches them across the widened frame; the side ones do not, so they stay at their 4:3 positions and appear as **black columns inside the picture**. Removed (see below). |
| **Intro / attract letterbox** | The same four-rectangle shape, but sized to the cinematic rect: `(0,0)-(320,47)`, `(0,194)-(320,240)`, `(0,0)-(21,240)`, `(296,0)-(320,240)` | Kept deliberately — the top/bottom bars are ~20% of the height, well outside the "overscan margin" test. |
| **Depth clear** | Fill rectangle with `fill=0xFFFCFFFC` over the clip rect (`func_uvgfxmgr_rom_00401DC4`) | Follows the clip rect, which `patches/viewport_patch.c` already makes full-frame during a race. |
| **Framebuffer clear** | Full-screen fill rectangle, black, drawn under RT64's *unbounded* `2048x2048` scissor (`func_uvgfxmgr_rom_00401914`) | Spans the scissor, so it is stretched — the whole widened frame is cleared. |
| **Track-select course name / text** | Wide texture rectangles (≥150 px) | Confirmed by `BAR_SKIP_WIDE=150`: the course map's text disappears. Daniel observed this in the live window; it is not visible in a race capture. |
| **Racing HUD** (speedometer dial, timer, lap, position, map) | Small texture rectangles — measured shapes `43x42` (dial), `42x42`, `100x20`, `14x19`, `7x10` (digits) | Rectangle projections, inverse-scaled, so they sit at their 4:3 positions. Untouched so far. |
| **Speedometer needle** | Triangles under an **orthographic** projection | Confirmed by an experiment that left orthographic projections unscaled: the needle moved out of the dial while nothing else changed. So BAR's HUD is *not* purely texture rectangles — at least the needle is ortho geometry. |
| **Sky / clouds above the horizon** | Geometry under a perspective projection whose viewport is one pixel narrower than the framebuffer scissor (319 against 320, because `uvGfxClipRect` clamps the viewport to `sScreenWidth - 1`). | It used to end exactly at the edges of the *un-widened* frame, at px 160..1116 of 1280 — fixed on screen whatever the camera did. Cause: RT64 had **two different coverage tests**. The projection processor widens the projection matrix at >= 80% coverage (`BAR_ASPECT_COVER`), but the framebuffer renderer's `useWideViewport` demanded 100%, so this projection had its matrix widened while its viewport and scissor stayed 4:3 — the wider view squeezed back into the old rectangle and clipped there. Fixed by giving the framebuffer renderer the same test. |

## Negative results worth keeping

* The sky is **not** wide texture rectangles: `BAR_SKIP_WIDE=150` removes the track-select screen's
  course-name text and leaves the racing sky untouched.
* The sky is **not** an orthographic layer. Leaving orthographic projections unwidened moved the
  speedometer needle out of its dial and did nothing to the sky — and note that this test is
  worthless unless the viewport and scissor are widened together with the matrix, because otherwise
  the layer is drawn wider and then clipped straight back, which looks identical to no change.
* Stretching every rectangle at least N% of the framebuffer width did nothing: BAR's backdrop
  strips (`320x3`, `320x6`, `160x6`, `160x12`) are not the sky.

## Open items

* **A 6-pixel band of flat background colour along the very top of a race frame** (framebuffer rows
  0-1): the sky geometry starts two rows down. This is not new — it was there before, hidden under
  the overscan mask's 17-row top bar — but it is visible now that the mask is gone.
* **Culling at the new margins** has not been measured. Now that the world reaches the frame's
  edges, drive a lap past trackside objects and watch whether anything pops in or out at the left
  and right margins.
