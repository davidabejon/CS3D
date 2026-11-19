/* Thin citro2d layer: both screens as render targets, a per-frame text
 * buffer for the system font, and a few drawing helpers.
 *
 * Main thread only. Usage, once per frame:
 *
 *     ui_begin();
 *     ui_scene(GFX_TOP, COL_BG);     ...draw the top screen...
 *     ui_scene(GFX_BOTTOM, COL_BG);  ...draw the bottom screen...
 *     ui_end();
 */
#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>

#include <stdbool.h>

#define SCREEN_TOP_W    400
#define SCREEN_BOTTOM_W 320
#define SCREEN_H        240

/* Colours as RGBA8 for citro2d. Make your palette here. */
#define COL_BG      C2D_Color32(0x14, 0x17, 0x1C, 0xFF)
#define COL_PANEL   C2D_Color32(0x22, 0x27, 0x30, 0xFF)
#define COL_TEXT    C2D_Color32(0xF2, 0xF4, 0xF5, 0xFF)
#define COL_DIM     C2D_Color32(0x9A, 0xA5, 0xB1, 0xFF)
#define COL_ACCENT  C2D_Color32(0x4F, 0xD1, 0xC5, 0xFF)
#define COL_OK      C2D_Color32(0x7E, 0xD9, 0x57, 0xFF)
#define COL_ERROR   C2D_Color32(0xF0, 0x71, 0x78, 0xFF)

/* Starts citro3d/citro2d and creates both screen targets. gfxInitDefault()
 * must already have run. False if the GPU could not be set up. */
bool ui_init(void);
void ui_exit(void);

/* Frame bracket: ui_begin() also clears the frame's text buffer. */
void ui_begin(void);
void ui_scene(gfxScreen_t screen, u32 clear_color);
void ui_end(void);

/* True once after the app came back from HOME, sleep or an applet: with
 * on-demand rendering that is the moment to repaint. */
bool ui_take_restored(void);

/* A filled rectangle. */
void ui_rect(float x, float y, float w, float h, u32 color);

/* Text with the system font, top-left at (x, y). `wrap_width` > 0 wraps at
 * word boundaries to that width. Returns the height drawn (an estimate
 * when wrapping). The string is
 * parsed into this frame's buffer, so call it every frame: simple and fast
 * enough for a few hundred glyphs. For big static texts, parse once into
 * your own C2D_TextBuf and draw with C2D_DrawText. */
float ui_text(const char *s, float x, float y, float scale, u32 color,
              float wrap_width);
/* The same, centred horizontally on `cx`. */
void ui_text_centered(const char *s, float cx, float y, float scale,
                      u32 color);
/* Width of `s` at `scale`, in pixels. */
float ui_text_width(const char *s, float scale);
/* One line at most `max_w` wide: a longer `s` is cut and ends in "...".
 * Measured from the system font's glyph widths, so it costs no more of the
 * frame's text buffer than drawing the visible part. */
void ui_text_fit(const char *s, float x, float y, float scale, u32 color,
                 float max_w);

/* A three-dot loading indicator centred on (cx, cy). `frame` animates it. */
void ui_loader(float cx, float cy, unsigned frame, u32 color);

#endif
