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
| **Racing HUD** (speedometer dial, timer, lap, position, map) | Small texture rectangles. Measured extents in game pixels: speedometer `(25,21)-(77,53)` with its gear digits at `(60,36)-(74,53)`; timer `(208,21)-(293,42)` with digits along `y 27..51`; lap `(236,51)-(292,68)`; course map `(235,149)-(291,205)` with its blip at `(260,186)-(266,192)`; position `(232,205)-(290,222)` | **Anchored** (see "Anchoring the HUD" below): the speedometer to the left edge, the timer, lap, map and position to the right edge, so the layout opens out with the aspect ratio instead of sitting in the middle 4:3 region. |
| **Speedometer needle** | Triangles under an **orthographic** projection — the only one in a race frame, 320 wide | Confirmed by an experiment that left orthographic projections unscaled: the needle moved out of the dial while nothing else changed. So BAR's HUD is *not* purely texture rectangles — at least the needle is ortho geometry. Anchored with the dial by giving that projection a left viewport origin; nothing else in a race moved when it did, which is the second confirmation that the needle is all this layer draws. |
| **Sky / clouds above the horizon** | Geometry under a perspective projection whose viewport is one pixel narrower than the framebuffer scissor (319 against 320, because `uvGfxClipRect` clamps the viewport to `sScreenWidth - 1`). | It used to end exactly at the edges of the *un-widened* frame, at px 160..1116 of 1280 — fixed on screen whatever the camera did. Cause: RT64 had **two different coverage tests**. The projection processor widens the projection matrix at >= 80% coverage (`BAR_ASPECT_COVER`), but the framebuffer renderer's `useWideViewport` demanded 100%, so this projection had its matrix widened while its viewport and scissor stayed 4:3 — the wider view squeezed back into the old rectangle and clipped there. Fixed by giving the framebuffer renderer the same test. |

## Anchoring the HUD to the widened frame

Landed 6 Sep 2026 (`lib/rt64/src/hle/rt64_bar_hud.cpp`). BAR emits no extended-GBI origins, so by
default RT64 places every 2D element by scaling it about the frame's centre: correct proportions,
but the whole HUD huddles in the middle 4:3 region with empty picture either side. RT64 now
classifies BAR's 2D draws itself and supplies the origins a game written for the extended GBI would
have emitted. What each measurement bought:

* **The HUD lives in two horizontal bands, and the race messages live between them.** In game
  pixels: speedometer and timer `y 21..53`, lap `y 51..70`, the centred "GET READY / GO!" messages
  `y 79..154`, map `y 149..205`, position `y 202..222`. So an element is only anchored if it lies
  entirely in the top 33% or the bottom 40% of the screen. Position alone is not enough to separate
  them — the timer's leading digit is at `x 205..212` and a message glyph at `x 218..230`, the same
  part of the screen.
* **Anchoring is a translation, not a stretch.** With both edges of an element given the same origin,
  a left-anchored element maps game `x` to `x * mult` and a right-anchored one to `(x + 106.67) * mult`
  — pure shifts of 53⅓ game pixels at 16:9. Every element on a side moves by the same amount, so the
  map keeps its blip and the timer keeps its digits.
* **The scissor has to travel with the element.** BAR scissors its HUD to the whole 320x240 screen,
  which converts to the centred 4:3 region, so an element moved to an edge is drawn outside its own
  scissor and vanishes. This is exactly how the needle disappeared while its dial anchored correctly;
  anchored draws now get the two edge origins on their scissor as well.
* **`currentGameState == 5` is not "a race".** It also covers the track-loading screen — the
  "Coventry Cove" card with its wipe tiles and its record-time text — which is a full 4:3 composition
  and came apart when its elements were pulled to the edges. `raceState` (`gGameSettings + 0x88`) is
  `0x10000` and then `0x30000` while the race is being set up and `0` once it is running, so the gate
  is `currentGameState == 5 && raceState == 0`.
* **The pause menu needs no special case.** It is drawn in the middle band and so classifies as
  centred; pausing mid-race leaves the menu centred and the HUD anchored, which is what it should
  look like.

Switches: `BAR_HUD_ANCHOR=0` turns the whole thing off, `BAR_HUD_ORTHO=0` keeps the rectangles
anchored but leaves the needle's layer centred (that is how the two halves are told apart on screen),
and `BAR_HUD_TRACE=1` prints the game state, the racing flag and every rectangle with the class it was
given. The user-facing control is the **HUD Ratio** setting: *Full* anchors to the frame's edges,
*Clamp16x9* anchors only as far as 16:9 (so an ultrawide keeps the HUD at 16:9 positions), and
*Original* restores the centred placement.

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
* **The results screen has not been checked** with anchoring on. Finishing a race takes minutes, so
  the state and `raceState` it runs in were never measured; if it turns out to be `state 5` with
  `raceState 0` its 2D layout will be anchored like the HUD, which is probably wrong for it.
* **Split screen has not been checked.** Classification measures every rectangle against the whole
  320-wide screen, so a two-player HUD drawn inside a half-screen viewport will be classified against
  the wrong reference. The bands and the per-viewport reference both need revisiting before 2P.
* **The pause menu's dimming panel covers only the centred 4:3 region**, leaving the widened margins
  undimmed. It is a 2D rectangle in the middle band, so anchoring does not touch it; this predates
  the anchoring work and is a separate widescreen item.
