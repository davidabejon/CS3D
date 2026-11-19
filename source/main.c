/* CS3D: las radios del mundo en una Nintendo 3DS.
 *
 * Un globo terráqueo con las emisoras del directorio de radio-browser.info
 * (app/radiobrowser.h) como puntos (app/stationmap.h); la elegida se
 * reproduce con audio/radio3ds.h.
 *
 * Pantalla superior: el globo, con el mapa de OpenStreetMap (gfx/tiles.h)
 * en cuanto llegan sus teselas. Pantalla inferior: la emisora enfocada y lo
 * que suena.
 *
 * Controles: stick = girar el globo (al soltarlo, la emisora más cercana al
 * centro se centra); cruceta izquierda/derecha = otra emisora del mismo
 * sitio; cruceta arriba/abajo = volumen; L/R = zoom; A = escuchar;
 * B = parar; Y = buscar; X = volver al top mundial; START = salir.
 */
#include <3ds.h>
#include <citro2d.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio3ds.h"
#include "radiobrowser.h"
#include "globe.h"
#include "stationmap.h"
#include "tiles.h"
#include "tls3ds.h"
#include "ui.h"

/* Memoria para el servicio de sockets: alineada a 4 KB. 1 MB es holgado. */
#define SOC_BUFFER_SIZE 0x100000

/* Emisoras que se cargan para el globo (páginas de RB_PAGE, en segundo
 * plano). */
#define MAP_TARGET 400

/* ------------------------------------------------------------------ vista */

/* Geometría de la pantalla inferior (320 x 240). */
#define BAR_H      28                       /* barra superior */
#define FOOTER_Y   206

typedef struct { int x, y, w, h; } button;
static const button BTN_TOP    = { 182, 3, 58, 22 };
static const button BTN_SEARCH = { 246, 3, 70, 22 };

static struct {
	/* Botón táctil pulsado. */
	bool       touching;
	int        touch_x, touch_y;
	/* Lo que suena, copiado: la lista puede cambiar mientras tanto. */
	rb_station playing;
	bool       has_playing;
} V;

static bool inside(const button *b, int x, int y)
{
	return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static bool wifi_connected(void)
{
	u32 status = 0;
	return R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
}

/* ---------------------------------------------------------------- acciones */

/* Abre el teclado del sistema. Bloquea el hilo principal mientras está
 * abierto; la radio y la red siguen en los suyos. */
static void ask_search(const char *current)
{
	SwkbdState kb;
	char text[64];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof(text) - 1);
	swkbdSetHintText(&kb, "Nombre de la emisora (vacío: top mundial)");
	swkbdSetInitialText(&kb, current);
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancelar", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Buscar", true);
	if (swkbdInputText(&kb, text, sizeof(text)) == SWKBD_BUTTON_RIGHT)
		rb_search(text);
}

static void play_station(int index, bool radio_ready)
{
	rb_station st;
	if (!radio_ready || !rb_station_get(index, &st))
		return;
	V.playing = st;
	V.has_playing = true;
	radio_play(st.url);
	rb_click(st.uuid);
}

/* ------------------------------------------------------------------ dibujo */

static void draw_small_button(const button *b, const char *label, bool down)
{
	ui_rect(b->x, b->y, b->w, b->h, down ? COL_OK : COL_ACCENT);
	ui_text_centered(label, b->x + b->w / 2.0f, b->y + 4, 0.45f, COL_BG);
}

/* La emisora enfocada en el globo. */
static void draw_details(const rb_info *info, const char *init_error,
                         unsigned frame)
{
	const float x = 10, w = SCREEN_BOTTOM_W - 20;
	if (init_error) {
		ui_text("No se pudo iniciar la red:", x, 40, 0.5f, COL_ERROR, 0);
		ui_text(init_error, x, 60, 0.45f, COL_TEXT, w);
		return;
	}
	rb_station st;
	int focus = map_focused();
	if (focus < 0 || !rb_station_get(focus, &st)) {
		if (info->state == RB_FAILED) {
			ui_text("No se pudo cargar el directorio:", x, 40, 0.5f,
			        COL_ERROR, 0);
			ui_text(info->error, x, 60, 0.42f, COL_DIM, w);
		} else if (info->count == 0 && info->state != RB_READY) {
			ui_text("Buscando emisoras...", x, 40, 0.5f, COL_DIM, 0);
			ui_loader(SCREEN_BOTTOM_W / 2.0f, 80, frame, COL_ACCENT);
		} else if (map_point_count() == 0) {
			ui_text(info->count ? "Ninguna emisora tiene ubicación."
			        : "Sin resultados.", x, 40, 0.5f, COL_DIM, 0);
		} else {
			ui_text("Gira el globo con el stick y suéltalo cerca de un "
			        "punto.", x, 40, 0.5f, COL_DIM, w);
		}
		return;
	}

	ui_text_fit(st.name, x, 34, 0.6f, COL_TEXT, w);

	char line[200];
	if (st.countrycode[0])
		snprintf(line, sizeof(line), "%s (%s)",
		         st.country[0] ? st.country : "?", st.countrycode);
	else
		snprintf(line, sizeof(line), "%s",
		         st.country[0] ? st.country : "País desconocido");
	ui_text_fit(line, x, 56, 0.48f, COL_ACCENT, w);

	char kbps[24] = "bitrate ?";
	if (st.bitrate > 0)
		snprintf(kbps, sizeof(kbps), "%d kbps", st.bitrate);
	snprintf(line, sizeof(line), "MP3  ·  %s  ·  %d votos", kbps, st.votes);
	ui_text(line, x, 74, 0.42f, COL_DIM, 0);

	if (st.tags[0])
		ui_text_fit(st.tags, x, 90, 0.42f, COL_TEXT, w);

	if (st.has_geo)
		snprintf(line, sizeof(line), "%.2f° %c   %.2f° %c",
		         st.lat < 0 ? -st.lat : st.lat, st.lat < 0 ? 'S' : 'N',
		         st.lon < 0 ? -st.lon : st.lon, st.lon < 0 ? 'O' : 'E');
	else
		snprintf(line, sizeof(line), "Ubicación aproximada (país)");
	ui_text(line, x, 106, 0.4f, COL_DIM, 0);
}

/* Lo que suena, en la franja baja de la pantalla inferior. */
static void draw_now_playing(const char *radio_error, unsigned frame)
{
	const float x = 6, y = 124, w = SCREEN_BOTTOM_W - 12;
	const float tx = x + 6, tw = w - 12;
	ui_rect(x, y, w, FOOTER_Y - y - 4, COL_PANEL);
	if (radio_error) {
		ui_text("Audio no disponible:", tx, y + 4, 0.48f, COL_ERROR, 0);
		ui_text(radio_error, tx, y + 22, 0.42f, COL_TEXT, tw);
		return;
	}

	radio_status st;
	radio_status_get(&st);
	if (!V.has_playing) {
		ui_text("Centra una emisora y pulsa A.", tx, y + 28, 0.48f, COL_DIM,
		        0);
		return;
	}
	ui_text_fit(V.playing.name, tx, y + 3, 0.48f, COL_ACCENT, tw);
	ui_text_fit(st.title[0] ? st.title : "-", tx, y + 20, 0.5f, COL_TEXT, tw);

	char line[200];
	u32 color = COL_DIM;
	switch (st.state) {
	case RADIO_STOPPED:
		snprintf(line, sizeof(line), "Parada.");
		break;
	case RADIO_CONNECTING:
		snprintf(line, sizeof(line), "Conectando...");
		break;
	case RADIO_BUFFERING:
		snprintf(line, sizeof(line), "Cargando búfer...");
		break;
	case RADIO_PLAYING:
		snprintf(line, sizeof(line), "Sonando  ·  %d kbps  ·  %d Hz  ·  %s",
		         st.bitrate_kbps, st.sample_rate,
		         st.channels == 2 ? "estéreo" : "mono");
		color = COL_OK;
		break;
	case RADIO_FAILED:
		snprintf(line, sizeof(line), "Error: %s", st.error);
		color = COL_ERROR;
		break;
	}
	if (st.reconnecting && st.state != RADIO_FAILED) {
		snprintf(line, sizeof(line), "Reconectando (%u): %s", st.reconnects,
		         st.error);
		color = COL_ERROR;
	}
	ui_text_fit(line, tx, y + 39, 0.42f, color, tw - 36);
	if (st.state == RADIO_CONNECTING || st.state == RADIO_BUFFERING)
		ui_loader(x + w - 22, y + 46, frame, COL_ACCENT);

	/* Búfer (audio comprimido aún por decodificar) y volumen. */
	const float bar_w = 80, bar_y = y + 60;
	ui_rect(tx, bar_y, bar_w, 6, COL_BG);
	ui_rect(tx, bar_y, bar_w * st.fill, 6, COL_ACCENT);
	snprintf(line, sizeof(line), "búfer %.1f s · vol %d%% · cortes %u",
	         st.buffered_s, (int)(st.volume * 100 + 0.5f), st.underruns);
	ui_text_fit(line, tx + bar_w + 6, bar_y - 4, 0.38f, COL_DIM,
	            tw - bar_w - 6);
}

static void draw_bottom(const rb_info *info, const char *init_error,
                        const char *radio_error, unsigned frame)
{
	ui_scene(GFX_BOTTOM, COL_BG);

	ui_rect(0, 0, SCREEN_BOTTOM_W, BAR_H, COL_PANEL);
	char title[96];
	if (info->query[0])
		snprintf(title, sizeof(title), "\"%s\"", info->query);
	else
		snprintf(title, sizeof(title), "Top mundial");
	ui_text_fit(title, 8, 6, 0.5f, COL_TEXT, BTN_TOP.x - 14);
	draw_small_button(&BTN_TOP, "Top (X)",
	                  V.touching && inside(&BTN_TOP, V.touch_x, V.touch_y));
	draw_small_button(&BTN_SEARCH, "Buscar (Y)",
	                  V.touching && inside(&BTN_SEARCH, V.touch_x, V.touch_y));

	draw_details(info, init_error, frame);
	draw_now_playing(radio_error, frame);

	ui_rect(0, FOOTER_Y, SCREEN_BOTTOM_W, SCREEN_H - FOOTER_Y, COL_PANEL);
	ui_text("Stick girar · L/R zoom · Cruceta < > cambiar", 8,
	        FOOTER_Y + 3, 0.38f, COL_DIM, 0);
	ui_text("A escuchar · B parar · Cruceta arriba/abajo volumen", 8, FOOTER_Y + 18, 0.38f,
	        COL_DIM, 0);
	if (info->state == RB_LOADING && info->count > 0) {
		ui_loader(SCREEN_BOTTOM_W - 24, FOOTER_Y + 24, frame, COL_ACCENT);
	} else {
		char count[24];
		snprintf(count, sizeof(count), "%d", map_point_count());
		float cw = ui_text_width(count, 0.42f);
		ui_text(count, SCREEN_BOTTOM_W - 8 - cw, FOOTER_Y + 17, 0.42f,
		        COL_DIM, 0);
	}
}

static void draw_top(bool wifi, unsigned frame)
{
	ui_scene(GFX_TOP, COL_BG);
	map_draw(V.has_playing && radio_active() ? V.playing.uuid : NULL, frame);

	/* La cabecera va encima: con zoom, el globo pasa por debajo. */
	ui_rect(0, 0, SCREEN_TOP_W, MAP_HEADER_H, COL_PANEL);
	ui_text("CS3D", 10, 3, 0.7f, COL_ACCENT, 0);
	ui_text("radios del mundo", 70, 8, 0.5f, COL_DIM, 0);
	const char *net = wifi ? "Wi-Fi" : "sin Wi-Fi";
	ui_text(net, SCREEN_TOP_W - 10 - ui_text_width(net, 0.45f), 8, 0.45f,
	        wifi ? COL_OK : COL_ERROR, 0);

	/* La atribución que pide OpenStreetMap, siempre que se ve su mapa. */
	if (globe_has_map()) {
		const char *osm = "© OpenStreetMap contributors";
		float w = ui_text_width(osm, 0.35f);
		ui_rect(SCREEN_TOP_W - w - 8, SCREEN_H - 13, w + 8, 13, COL_PANEL);
		ui_text(osm, SCREEN_TOP_W - w - 4, SCREEN_H - 12, 0.35f, COL_DIM, 0);
	}
	tiles_info t;
	tiles_info_get(&t);
	if (t.waiting > 0)
		ui_loader(SCREEN_TOP_W - 18, MAP_HEADER_H + 10, frame, COL_ACCENT);
	if (t.failing)
		ui_text_fit(t.error, 6, SCREEN_H - 13, 0.35f, COL_ERROR, 200);
}

/* ---------------------------------------------------------------- entrada */

static void handle_touch(u32 down, u32 up, const rb_info *info)
{
	if (down & KEY_TOUCH) {
		touchPosition t;
		hidTouchRead(&t);
		V.touching = true;
		V.touch_x = t.px;
		V.touch_y = t.py;
	}
	if (!(up & KEY_TOUCH) || !V.touching)
		return;
	/* Al soltar: un toque donde empezó. (Tras soltar, hidTouchRead ya da
	 * 0,0.) */
	V.touching = false;
	if (inside(&BTN_TOP, V.touch_x, V.touch_y))
		rb_search("");
	else if (inside(&BTN_SEARCH, V.touch_x, V.touch_y))
		ask_search(info->query);
}

static void handle_buttons(u32 down, u32 held, const rb_info *info,
                           bool radio_ready)
{
	circlePosition c;
	hidCircleRead(&c);
	map_input in = {
		.stick_x = c.dx / 156.0f,
		.stick_y = c.dy / 156.0f,
		.cycle = (down & KEY_DRIGHT) ? 1 : (down & KEY_DLEFT) ? -1 : 0,
		.zoom = (held & KEY_R) ? 1 : (held & KEY_L) ? -1 : 0,
	};
	map_update(&in);

	if (down & KEY_A) {
		int focus = map_focused();
		if (focus >= 0)
			play_station(focus, radio_ready);
	}
	if (down & KEY_B)
		radio_stop();
	if (down & KEY_X)
		rb_search("");
	if (down & KEY_Y)
		ask_search(info->query);
	if (radio_ready && (down & (KEY_DUP | KEY_DDOWN))) {
		radio_status st;
		radio_status_get(&st);
		radio_set_volume(st.volume + ((down & KEY_DUP) ? 0.1f : -0.1f));
	}
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* 1. Gráficos. En una New 3DS se sube la CPU a 804 MHz: holgura para
	 *    decodificar MP3 mientras se dibuja. */
	osSetSpeedupEnable(true);
	gfxInitDefault();
	bool romfs_ready = R_SUCCEEDED(romfsInit());
	if (!ui_init()) {
		if (romfs_ready)
			romfsExit();
		gfxExit();
		return 1;
	}
	map_init();

	/* 2. Red: sockets -> ps (entropía para TLS) -> TLS. Un fallo aquí no
	 *    cierra la app: se muestra en pantalla. */
	const char *init_error = NULL;
	char tls_reason[128];
	bool ac_ready = R_SUCCEEDED(acInit());
	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
	bool soc_ready = soc_buffer &&
	                 R_SUCCEEDED(socInit(soc_buffer, SOC_BUFFER_SIZE));
	bool ps_ready = R_SUCCEEDED(psInit());
	bool tls_ready = false;
	if (!soc_ready) {
		init_error = "socInit() falló.";
	} else if (!ps_ready) {
		init_error = "psInit() falló (sin entropía no hay TLS).";
	} else {
		int rc = tls_global_init();
		if (rc != 0) {
			tls_strerror(rc, tls_reason, sizeof(tls_reason));
			init_error = tls_reason;
		} else {
			tls_ready = true;
		}
	}

	/* 3. Audio (DSP). Necesita sdmc:/3ds/dspfirm.cdc; sin él se puede
	 *    explorar el directorio, pero no escuchar. */
	char radio_reason[160];
	const char *radio_error = NULL;
	bool radio_ready = false;
	if (!tls_ready)
		radio_error = "sin red";
	else if (radio_init(radio_reason, sizeof(radio_reason)))
		radio_ready = true;
	else
		radio_error = radio_reason;

	/* 4. Las teselas del mapa y el directorio, que arranca con el top
	 *    mundial. */
	bool tiles_ready = tls_ready && tiles_init();
	bool rb_ready = tls_ready && rb_init();
	if (rb_ready)
		rb_search("");

	/* 5. Bucle principal. aptMainLoop() devuelve false cuando el sistema
	 *    pide cerrar la app (HOME > Cerrar). */
	unsigned frame = 0;
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
		if (down & KEY_START)
			break;

		rb_info info;
		rb_info_get(&info);
		if (rb_ready) {
			handle_touch(down, up, &info);
			handle_buttons(down, held, &info, radio_ready);
			rb_info_get(&info);     /* una búsqueda nueva la cambia */
			map_sync(&info);
			/* El globo quiere más emisoras que una página: se piden las
			 * siguientes, una a una, hasta MAP_TARGET. */
			if (info.more && info.state == RB_READY &&
			    info.count < MAP_TARGET)
				rb_load_more();
		}
		radio_update();

		ui_begin();
		draw_top(ac_ready && wifi_connected(), frame);
		draw_bottom(&info, init_error, radio_error, frame);
		ui_end();
		frame++;
	}

	/* 6. Apagado, en orden inverso. tls_set_abort() corta las esperas de
	 *    red para que los hilos terminen en ~100 ms en vez de agotar su
	 *    timeout. Directorio y radio se cierran antes que TLS. */
	tls_set_abort(true);
	if (rb_ready)
		rb_exit();
	if (tiles_ready)
		tiles_exit();
	if (radio_ready)
		radio_exit();
	tls_set_abort(false);
	if (tls_ready)
		tls_global_exit();
	if (ps_ready)
		psExit();
	if (soc_ready)
		socExit();
	free(soc_buffer);
	if (ac_ready)
		acExit();
	map_exit();
	ui_exit();
	if (romfs_ready)
		romfsExit();
	gfxExit();
	return 0;
}
