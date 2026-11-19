#include "ui.h"

#include <citro3d.h>

#include <string.h>

/* Glyphs that fit in one frame's text buffer. Raise it if text goes missing
 * at the end of a long screen. */
#define FRAME_GLYPHS 4096

static C3D_RenderTarget *top_target;
static C3D_RenderTarget *bottom_target;
static C2D_TextBuf frame_text;
static bool c3d_ready;
static bool c2d_ready;
static aptHookCookie apt_cookie;
static bool apt_hooked;
static volatile bool restored = true;

/* Back from the HOME menu, a closed lid or an applet the screens may show
 * something else; an app that only redraws on change needs to know. */
static void on_apt_event(APT_HookType hook, void *param)
{
	(void)param;
	if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
		restored = true;
}

bool ui_init(void)
{
	c3d_ready = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	if (!c3d_ready)
		return false;
	c2d_ready = C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	if (!c2d_ready) {
		ui_exit();
		return false;
	}
	C2D_Prepare();
	fontEnsureMapped();        /* ui_text_fit reads glyph widths directly */
	top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	frame_text = C2D_TextBufNew(FRAME_GLYPHS);
	if (!top_target || !bottom_target || !frame_text) {
		ui_exit();
		return false;
	}
	aptHook(&apt_cookie, on_apt_event, NULL);
	apt_hooked = true;
	return true;
}

void ui_exit(void)
{
	if (apt_hooked)
		aptUnhook(&apt_cookie);
	apt_hooked = false;
	if (frame_text)
		C2D_TextBufDelete(frame_text);
	frame_text = NULL;
	if (top_target)
		C3D_RenderTargetDelete(top_target);
	if (bottom_target)
		C3D_RenderTargetDelete(bottom_target);
	top_target = bottom_target = NULL;
	if (c2d_ready)
		C2D_Fini();
	if (c3d_ready)
		C3D_Fini();
	c2d_ready = c3d_ready = false;
}

void ui_begin(void)
{
	C2D_TextBufClear(frame_text);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void ui_scene(gfxScreen_t screen, u32 clear_color)
{
	C3D_RenderTarget *target = screen == GFX_TOP ? top_target : bottom_target;
	C2D_TargetClear(target, clear_color);
	C2D_SceneBegin(target);
}

void ui_end(void)
{
	C3D_FrameEnd(0);
}

bool ui_take_restored(void)
{
	bool was = restored;
	restored = false;
	return was;
}

void ui_rect(float x, float y, float w, float h, u32 color)
{
	C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

/* Parses into the frame buffer; false when the buffer is full. */
static bool parse(C2D_Text *text, const char *s)
{
	C2D_TextParse(text, frame_text, s);
	if (text->end == text->begin && s[0])
		return false;
	C2D_TextOptimize(text);
	return true;
}

float ui_text(const char *s, float x, float y, float scale, u32 color,
              float wrap_width)
{
	C2D_Text text;
	if (!parse(&text, s))
		return 0.0f;
	float h = 0.0f;
	if (wrap_width > 0.0f) {
		C2D_DrawText(&text, C2D_WithColor | C2D_WordWrap, x, y, 0.0f,
		             scale, scale, color, wrap_width);
		/* citro2d does not report the wrapped height; this estimate
		 * (unwrapped width / wrap width) can fall a line short when
		 * long words wrap early. */
		float w = 0.0f;
		C2D_TextGetDimensions(&text, scale, scale, &w, &h);
		int lines = (int)(w / wrap_width) + 1;
		h *= lines;
	} else {
		C2D_DrawText(&text, C2D_WithColor, x, y, 0.0f, scale, scale, color);
		C2D_TextGetDimensions(&text, scale, scale, NULL, &h);
	}
	return h;
}

void ui_text_centered(const char *s, float cx, float y, float scale,
                      u32 color)
{
	C2D_Text text;
	if (!parse(&text, s))
		return;
	C2D_DrawText(&text, C2D_WithColor | C2D_AlignCenter, cx, y, 0.0f,
	             scale, scale, color);
}

float ui_text_width(const char *s, float scale)
{
	C2D_Text text;
	if (!parse(&text, s))
		return 0.0f;
	float w = 0.0f;
	C2D_TextGetDimensions(&text, scale, scale, &w, NULL);
	return w;
}

static float glyph_width(u32 code_point, float scale)
{
	int index = fontGlyphIndexFromCodePoint(fontGetSystemFont(), code_point);
	charWidthInfo_s *info = fontGetCharWidthInfo(fontGetSystemFont(), index);
	return info ? info->charWidth * scale : 0.0f;
}

void ui_text_fit(const char *s, float x, float y, float scale, u32 color,
                 float max_w)
{
	char buf[256];
	const float dots = 3 * glyph_width('.', scale);
	float w = 0.0f;
	size_t fits = 0;           /* bytes that fit with room for the dots */
	const u8 *p = (const u8 *)s;
	while (*p) {
		u32 cp;
		ssize_t n = decode_utf8(&cp, p);
		if (n <= 0)
			break;
		if (cp == '\n')
			cp = ' ';
		w += glyph_width(cp, scale);
		if (w > max_w)
			break;
		p += n;
		if (w + dots <= max_w)
			fits = (size_t)(p - (const u8 *)s);
	}
	if (!*p) {
		ui_text(s, x, y, scale, color, 0);     /* all of it fits */
		return;
	}
	if (fits > sizeof(buf) - 4)
		fits = sizeof(buf) - 4;
	while (fits && ((u8)s[fits] & 0xC0) == 0x80)
		fits--;
	memcpy(buf, s, fits);
	memcpy(buf + fits, "...", 4);
	ui_text(buf, x, y, scale, color, 0);
}

void ui_loader(float cx, float cy, unsigned frame, u32 color)
{
	const float size = 6.0f, gap = 6.0f;
	const float left = cx - (3 * size + 2 * gap) / 2;
	unsigned lit = (frame / 12) % 3;
	for (unsigned i = 0; i < 3; i++) {
		float lift = i == lit ? 3.0f : 0.0f;
		ui_rect(left + i * (size + gap), cy - size / 2 - lift, size, size,
		        i == lit ? color : COL_DIM);
	}
}
