# The VMU LCD — what it should show

**Status: DESIGN ONLY. Nothing is implemented.** The mockup is the user's, dated
2026-08-05, and is checked in at `dc/assets/vmu/vmu-lcd-mockup.webp`. This file
records what it specifies so the image does not have to be re-interpreted later.

This is the *secondary display* on the VMU's own LCD while the game runs. It is
unrelated to `kb/save-plan.md`, which is about the 100 KB of flash behind it.
The two share a device and nothing else — see §5.

---

## 1. The panel

The Dreamcast VMU LCD is **48 x 32 pixels, 1 bit per pixel, monochrome**, no
grey and no backlight. That is the entire budget: 1,536 pixels, 192 bytes.

KOS drives it with `vmu_draw_lcd(maple_device_t *dev, void *bitmap)`, which
takes a 192-byte bitmap. There is no text layer and no font — **every glyph in
the mockup has to be drawn by us, as pixels.**

---

## 2. The layout

```
+------------------------------------------------+
| DAVID                          A-3             |   name          acre coord
| 999996                        +--+--+--+--+--+ |   bells         town grid
|                               +--+--+--+--+--+ |
| 12:17PM.                      +--+--+--+--+--+ |   clock
|                               +--+--+--+--+--+ |
|                               +--+--+--+--+--+ |
+------------------------------------------------+
```

Left column, top to bottom:

| element | source |
|---|---|
| **player name** | `Save_Get(player[n].name)` — the game already has it as the name-entry keyboard's output |
| **bells** | a **6-digit count**, not a bar or a coarse indicator. The mockup reads `999996`, i.e. the field is sized for the wallet cap. No thousands separator — 48 px will not carry one |
| **clock** | **12-hour** `h:mm` + `AM`/`PM`, with the trailing period the mockup shows. The game's own time, not the RTC directly: `Common_Get(time)` |

Note the name and the bell count sit on adjacent rows in the same left column and
are the same glyph height, so one font serves both. The clock is the same font
again — **there is exactly one typeface on this panel.**

Right column:

| element | source |
|---|---|
| **acre coordinate** | `A-3` style. The town is a grid of acres and the player's block index is `(bx, bz)` — the same pair `mFI_GetBGDisplayListRom(bx, bz)` takes |
| **town grid** | 5 x 5 of the acre map, with the player's acre presumably marked. The layout lives in `Save_Get(combi_table)` (`include/m_common_data.h:96`), written by `mFM_SetCombiTable` at `m_field_make.c:1471` |

⚠️ The grid's exact dimensions are **read off the mockup, not measured** — count
the cells in the image before hardcoding them. The town is 5 acres wide in the
playable area but the full `combi_table` is `[10][7]`, so which subrange the
grid shows is an open question the mockup does not settle.

---

## 3. Why this is cheap, and the one reason it might not be

Cheap: every field above is a global the port can already read, the panel
updates at human speed (the clock ticks once a minute; the acre changes when the
player walks a block boundary), and 192 bytes of bitmap is nothing.

The one real cost is **the Maple bus write**. `vmu_draw_lcd` is a Maple frame to
the controller's expansion port, and the port's input path already goes through
Maple every frame. Blitting the LCD on a schedule (once a second, or only on
change) is the obvious policy; blitting it every frame is not, and nobody has
measured what a Maple LCD write costs on this machine.

**Do not implement this before the frame rate work lands.** The town is at
11-15 FPS and this is a cosmetic feature; it is written down here so it is not
forgotten, not because it is next.

---

## 4. What has to be built

1. **A 1-bpp glyph set.** Digits, `A`-`Z`, `:`, `-`, `.`, `AM`/`PM`. Probably a
   3x5 or 4x6 font, hand-drawn, in `dc/`. This is the bulk of the work and it is
   pure data. One face covers the whole panel — the mockup uses the same glyph
   height for the name, the bell count and the clock.
   ⚠️ Size it against the **worst case, not the mockup**: the mockup's name is
   `DAVID` (5), but the game's name entry allows longer, and the bell field is
   6 digits wide. Left column has ~26 px before it runs into the acre grid.
2. **A 192-byte framebuffer + blit** in `dc/src/` — set/clear pixel, draw glyph,
   draw box.
3. **A change-detect tick** so the panel is only pushed when a field actually
   differs.
4. **A kill switch**, per `CLAUDE.md` §1. `DC_VMU_LCD=0`.

## 5. It shares the device with the save, and that is a conflict to design for

`kb/save-plan.md` measures a VMU block write at **84.6 ms**, and a 150-block
save at **13.4 s**. The LCD and the flash are the same physical unit on the same
Maple port. A save in progress and an LCD blit must not interleave, and the
panel is the natural place to *show* save progress — which is a better use of it
during those 13.4 s than the clock.

Related: `kb/save-plan.md`, `kb/save-layout.md`, `kb/platform-api-save-card.md`,
and `dc/src/dc_card.c` (the working `vmufs`/`vmu_pkg` backend that the game does
not yet call).
