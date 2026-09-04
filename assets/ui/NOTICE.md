# UI asset provenance and licensing

These assets back the RecompFrontend (`recompui`) launcher and menus. The build stages this whole
directory next to the executable as `assets/`, because recompui resolves every asset as
`<exe dir>/assets/<name>`.

## The shape of this directory is deliberate

It mirrors `danielgomesvieira2000/wave-race-64-recomp`, the reference N64: Recompiled port for this
project: a near-empty `recomp.rcss`, `icons/`, and `promptfont/`.

**`recomp.rcss` is intentionally tiny.** recompui's elements style themselves in code from a named
theme palette (Background1..3, Text, Primary, Secondary, Warning, Danger, Success, Border), which is
why every one of these ports looks alike. A stylesheet that sets its own colours does not restyle
those elements, it competes with them. The one thing the library genuinely cannot know is the font,
so that is all this file supplies. To restyle the menus, call `recompui::theme::set_theme_color`
from C++ rather than adding rules here.

An earlier attempt imported Zelda 64: Recompiled's 62 KB `recomp.rcss`. That was wrong: that sheet
belongs to Zelda64Recomp's older in-repo UI rather than to RecompFrontend, and it sets
`body { font-family: chiaro }` — a Nintendo-associated typeface that is not redistributable here.
Do not reintroduce it.

## recomp.rcss, icons/

From `danielgomesvieira2000/wave-race-64-recomp`, this project's own reference port.

## LatoLatin-Regular.ttf

Lato, by Łukasz Dziedzic — **SIL Open Font License 1.1**.

The font's internal family name is **`LatoLatin`**, not `Lato`. RmlUi matches on the internal name,
and registering the wrong one fails silently in a way that wastes hours: every element lays out and
draws in the right place, with no text in any of them. It must stay in sync with
`kPrimaryFontFamily` in `src/frontend/bar_frontend.cpp` and with `body` in `recomp.rcss`.

## promptfont/

PromptFont, by Yukari "Shinmera" Hafner — controller and key glyphs used by `recompinput` for button
prompts. **SIL Open Font License 1.1**; `promptfont/LICENSE.txt` ships alongside as the licence
requires.
