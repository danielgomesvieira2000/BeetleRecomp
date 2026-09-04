# UI asset provenance and licensing

These assets back the RecompFrontend (`recompui`) launcher and menus. They are staged next to the
executable as `assets/` at build time, because recompui resolves every asset as
`<exe dir>/assets/<name>`.

The goal is deliberate: this port should wear **Zelda 64: Recompiled's frontend look**, not a
bespoke theme, so its stylesheet is used as-is wherever possible.

## recomp.rcss, rml.rcss

From [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) (`assets/`), which is
licensed **GPL-3.0**. This project is AGPL-3.0, and AGPLv3 §13 explicitly permits combining with
GPLv3 works, so the combination is fine — the obligation is to keep the source public and credit
the origin, which this file and the project credits do.

`recomp.rcss` is kept byte-identical to upstream except for a clearly delimited
`BeetleRecomp overrides` block appended at the end. Keep local changes inside that block so the
upstream sheet can be refreshed by overwriting everything above it.

The one override that is not cosmetic: upstream sets `body { font-family: chiaro }`, and **Chiaro
is a Nintendo-associated typeface that is not redistributable here**. It is deliberately not
bundled, so the override re-points the body font at Lato. Do not add Chiaro to this directory.

## LatoLatin-Regular.ttf

Lato, by Łukasz Dziedzic, under the **SIL Open Font License 1.1**. Note the font's internal family
name is `LatoLatin`, not `Lato` — RmlUi matches on the internal name, so registering the wrong one
silently renders no text at all.

## promptfont/

PromptFont, by Yukari "Shinmera" Hafner — controller and key glyphs used by `recompinput` to draw
button prompts. **SIL Open Font License 1.1**; `promptfont/LICENSE.txt` is included as the licence
requires.

## icons/

`Arrow.svg`, `Plus.svg`, `Quit.svg`, `RecordBorder.svg`, `Reset.svg`, `Trash.svg`, `X.svg` come from
Zelda 64: Recompiled (GPL-3.0), as above.

`Caret.svg`, `Cont.svg`, `Keyboard.svg`, `PlusKeyboard.svg`, `Question.svg` and `RecordSpinner.svg`
were written for this project: `recompui` is a newer extraction than the Zelda64Recomp tree and
references icons that predate it, so those had no upstream counterpart. They are simple monochrome
glyphs drawn with `currentColor` so they theme with the rest of the UI. Replace them with upstream
artwork if RecompFrontend ever ships its own.
