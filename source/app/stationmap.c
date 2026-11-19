#include "stationmap.h"
#include "globe.h"
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Globe size: at zoom 1 it fills the height under the header. */
#define BASE_RADIUS  100.0f
/* Down to streets: about OSM zoom level 15. */
#define MAX_ZOOM     8192.0f
#define ZOOM_STEP    1.05f    /* per frame while L or R is held */
/* Degrees per frame at full deflection and zoom 1 (~100 degrees a second). */
#define TURN_SPEED   1.7f
#define STICK_DEAD   0.12f
/* A station this close to the centre (pixels) is caught when the stick is
 * let go; within CLUSTER_R the D-pad steps through the ones bunched there. */
#define SNAP_R       14.0f
#define CLUSTER_R    10.0f
/* Fraction of the remaining angle covered each frame when centring. */
#define EASE         0.16f
/* Spread (degrees) of stations placed at their country's label point. */
#define COUNTRY_SPREAD 1.6f

typedef struct {
	globe_vec p;
	float     lat, lon;
	int       index;           /* in the directory's list */
	char      uuid[40];
	char      name[96];
} map_point;

static struct {
	map_point *pts;
	int        count, cap;
	int        synced;         /* list entries already looked at */
	int        unplaced;
	char       query[64];
	bool       fly_to_first;   /* a new list: go to its first station */

	globe_view view;
	float      zoom;
	int        focus;          /* point index or -1 */
	bool       locked;         /* stick released: the globe centres `focus` */
	bool       centred;
} M;

void map_init(void)
{
	globe_init();
	memset(&M, 0, sizeof(M));
	M.view.cx = SCREEN_TOP_W / 2.0f;
	M.view.cy = MAP_HEADER_H + (SCREEN_H - MAP_HEADER_H) / 2.0f;
	M.view.lat0 = 30.0f;               /* Europe and Africa, to start */
	M.view.lon0 = 10.0f;
	M.zoom = 1.0f;
	M.view.radius = BASE_RADIUS;
	M.focus = -1;
}

void map_exit(void)
{
	globe_exit();
	free(M.pts);
	M.pts = NULL;
	M.count = M.cap = 0;
}

/* --------------------------------------------------------------- placing */

static unsigned hash(const char *s)
{
	unsigned h = 2166136261u;          /* FNV-1a */
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static bool place(const rb_station *st, float *lat, float *lon)
{
	if (st->has_geo && (st->lat != 0.0f || st->lon != 0.0f) &&
	    st->lat >= -90.0f && st->lat <= 90.0f) {
		*lat = st->lat;
		*lon = st->lon;
		return true;
	}
	if (!globe_country_centre(st->countrycode, lat, lon))
		return false;
	/* A fixed offset per station, so it stays put between frames. */
	unsigned h = hash(st->uuid);
	float a = (float)(h & 0xFFFF) / 65535.0f * 6.2831853f;
	float d = (float)(h >> 16) / 65535.0f;
	d = sqrtf(d) * COUNTRY_SPREAD;
	*lat += d * sinf(a);
	float c = cosf(*lat * 3.14159265f / 180.0f);
	*lon += d * cosf(a) / (c > 0.2f ? c : 0.2f);
	if (*lat > 89.0f)
		*lat = 89.0f;
	if (*lat < -89.0f)
		*lat = -89.0f;
	return true;
}

void map_sync(const rb_info *info)
{
	if (strcmp(M.query, info->query) != 0 || info->count < M.synced) {
		memcpy(M.query, info->query, sizeof(M.query));
		M.count = M.synced = M.unplaced = 0;
		M.focus = -1;
		M.locked = M.centred = false;
		M.fly_to_first = true;
	}
	for (; M.synced < info->count; M.synced++) {
		rb_station st;
		if (!rb_station_get(M.synced, &st))
			break;
		float lat, lon;
		if (!place(&st, &lat, &lon)) {
			M.unplaced++;
			continue;
		}
		if (M.count == M.cap) {
			int cap = M.cap ? M.cap * 2 : 128;
			map_point *bigger = realloc(M.pts, cap * sizeof(*bigger));
			if (!bigger)
				return;
			M.pts = bigger;
			M.cap = cap;
		}
		map_point *p = &M.pts[M.count++];
		p->p = globe_vec_from(lat, lon);
		p->lat = lat;
		p->lon = lon;
		p->index = M.synced;
		memcpy(p->uuid, st.uuid, sizeof(p->uuid));
		memcpy(p->name, st.name, sizeof(p->name));
	}
	if (M.fly_to_first && M.count > 0) {
		M.fly_to_first = false;
		M.focus = 0;                   /* the most popular */
		M.locked = true;
		M.centred = false;
	}
}

/* ----------------------------------------------------------------- input */

/* Squared distance (pixels) from the centre of the view, or -1 when the
 * point is on the far side. */
static float centre_dist2(int i)
{
	float sx, sy, depth;
	if (!globe_project(&M.view, M.pts[i].p, &sx, &sy, &depth))
		return -1.0f;
	float dx = sx - M.view.cx, dy = sy - M.view.cy;
	return dx * dx + dy * dy;
}

static int nearest(float radius)
{
	int best = -1;
	float best_d = radius * radius;
	for (int i = 0; i < M.count; i++) {
		float d = centre_dist2(i);
		if (d >= 0.0f && d <= best_d) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

/* Next (dir = 1) or previous station, in list order, among those bunched at
 * the centre. */
static int cycle(int dir)
{
	int cur = M.focus >= 0 ? M.pts[M.focus].index : -1;
	int pick = -1, wrap = -1;
	for (int i = 0; i < M.count; i++) {
		float d = centre_dist2(i);
		if (d < 0.0f || d > CLUSTER_R * CLUSTER_R || i == M.focus)
			continue;
		int idx = M.pts[i].index;
		if (dir > 0) {
			if (idx > cur && (pick < 0 || idx < M.pts[pick].index))
				pick = i;
			if (wrap < 0 || idx < M.pts[wrap].index)
				wrap = i;
		} else {
			if (idx < cur && (pick < 0 || idx > M.pts[pick].index))
				pick = i;
			if (wrap < 0 || idx > M.pts[wrap].index)
				wrap = i;
		}
	}
	return pick >= 0 ? pick : wrap;
}

static float clamp_lat(float lat)
{
	return lat > 85.0f ? 85.0f : lat < -85.0f ? -85.0f : lat;
}

void map_update(const map_input *in)
{
	if (in->zoom) {
		M.zoom *= in->zoom > 0 ? ZOOM_STEP : 1.0f / ZOOM_STEP;
		if (M.zoom > MAX_ZOOM)
			M.zoom = MAX_ZOOM;
		if (M.zoom < 1.0f)
			M.zoom = 1.0f;
		M.view.radius = BASE_RADIUS * M.zoom;
	}

	float sx = in->stick_x, sy = in->stick_y;
	bool moving = sx * sx + sy * sy > STICK_DEAD * STICK_DEAD;
	if (moving) {
		/* Squared response: fine control near the middle of the stick. */
		float speed = TURN_SPEED / M.zoom;
		M.view.lon0 = globe_wrap_deg(M.view.lon0 + sx * fabsf(sx) * speed);
		M.view.lat0 = clamp_lat(M.view.lat0 + sy * fabsf(sy) * speed);
		M.locked = M.centred = false;
		M.fly_to_first = false;
		/* What will be caught if the stick is let go now. */
		M.focus = nearest(SNAP_R);
		return;
	}

	if (in->cycle && M.focus >= 0) {
		int next = cycle(in->cycle);
		if (next >= 0) {
			M.focus = next;
			M.locked = true;
			M.centred = false;
		}
	}
	if (!M.locked) {
		M.focus = nearest(SNAP_R);
		M.locked = M.focus >= 0;
		M.centred = false;
	}
	if (M.locked && M.focus >= 0 && !M.centred) {
		const map_point *p = &M.pts[M.focus];
		float dlon = globe_wrap_deg(p->lon - M.view.lon0);
		float dlat = clamp_lat(p->lat) - M.view.lat0;
		/* Close enough: a third of a pixel at this zoom. */
		float eps = 0.3f / (M.view.radius * 3.14159265f / 180.0f);
		if (fabsf(dlon) < eps && fabsf(dlat) < eps) {
			M.view.lon0 = p->lon;
			M.view.lat0 = clamp_lat(p->lat);
			M.centred = true;
		} else {
			M.view.lon0 = globe_wrap_deg(M.view.lon0 + dlon * EASE);
			M.view.lat0 += dlat * EASE;
		}
	}
}

int map_focused(void)
{
	return M.focus >= 0 && M.focus < M.count ? M.pts[M.focus].index : -1;
}

bool map_centred(void)
{
	return M.focus >= 0 && M.centred;
}

int map_point_count(void)
{
	return M.count;
}

int map_unplaced(void)
{
	return M.unplaced;
}

/* ------------------------------------------------------------------ draw */

/* Station dots: a saturated colour with a dark edge, to stand out on the
 * light OSM map as well as on the dark offline globe. */
#define COL_STATION  C2D_Color32(0xE8, 0x3A, 0x6B, 0xFF)
#define COL_EDGE     C2D_Color32(0x10, 0x14, 0x1A, 0xFF)

static u32 fade(u32 c, float depth)
{
	/* Translucent near the rim. Colours are 0xAABBGGRR. */
	float a = 0.35f + 0.65f * depth;
	return (c & 0x00FFFFFFu) | ((u32)(a * 255.0f) << 24);
}

static void draw_reticle(float cx, float cy, u32 color)
{
	const float r = 12.0f, len = 5.0f, t = 1.5f;
	for (int sx = -1; sx <= 1; sx += 2) {
		for (int sy = -1; sy <= 1; sy += 2) {
			float x = cx + sx * r, y = cy + sy * r;
			ui_rect(sx < 0 ? x : x - len, y - t / 2, len, t, color);
			ui_rect(x - t / 2, sy < 0 ? y : y - len, t, len, color);
		}
	}
}

void map_draw(const char *playing_uuid, unsigned frame)
{
	globe_draw(&M.view);

	float dot = 2.5f + 0.4f * M.zoom;
	if (dot > 5.0f)
		dot = 5.0f;
	for (int i = 0; i < M.count; i++) {
		if (i == M.focus)
			continue;
		float sx, sy, depth;
		if (!globe_project(&M.view, M.pts[i].p, &sx, &sy, &depth))
			continue;
		if (sx < -4 || sx > SCREEN_TOP_W + 4 || sy < -4 || sy > SCREEN_H + 4)
			continue;
		bool on = playing_uuid && strcmp(playing_uuid, M.pts[i].uuid) == 0;
		float s = on ? dot + 2.0f : dot;
		ui_rect(sx - s / 2 - 1, sy - s / 2 - 1, s + 2, s + 2,
		        fade(COL_EDGE, depth));
		ui_rect(sx - s / 2, sy - s / 2, s, s,
		        fade(on ? COL_OK : COL_STATION, depth));
	}

	/* Dark on the light map, light on the dark globe. */
	bool map = globe_has_map();
	draw_reticle(M.view.cx, M.view.cy,
	             map_centred() ? (map ? COL_EDGE : COL_TEXT)
	                           : (map ? C2D_Color32(0x40, 0x48, 0x55, 0xFF)
	                                  : COL_DIM));

	if (M.focus < 0 || M.focus >= M.count)
		return;
	const map_point *p = &M.pts[M.focus];
	float sx, sy, depth;
	if (!globe_project(&M.view, p->p, &sx, &sy, &depth))
		return;
	bool on = playing_uuid && strcmp(playing_uuid, p->uuid) == 0;
	u32 col = on ? COL_OK : COL_STATION;
	/* A pulsing halo once it is centred. */
	float pulse = M.centred ? 1.5f * sinf((float)frame * 0.12f) : 0.0f;
	C2D_DrawCircleSolid(sx, sy, 0.0f, 7.5f + pulse, COL_EDGE);
	C2D_DrawCircleSolid(sx, sy, 0.0f, 6.5f + pulse, col);
	C2D_DrawCircleSolid(sx, sy, 0.0f, 4.5f + pulse, COL_BG);
	C2D_DrawCircleSolid(sx, sy, 0.0f, 3.0f, col);

	/* Its name beside it, on a plate so the land does not show through. */
	const float scale = 0.45f, max_w = 150.0f;
	float w = ui_text_width(p->name, scale);
	if (w > max_w)
		w = max_w;
	float lx = sx + 14.0f, ly = sy - 8.0f;
	if (lx + w + 8.0f > SCREEN_TOP_W)
		lx = sx - 14.0f - w - 8.0f;      /* no room on the right */
	ui_rect(lx, ly, w + 8.0f, 16.0f, COL_PANEL);
	ui_text_fit(p->name, lx + 4.0f, ly + 1.0f, scale, col, max_w);
}
