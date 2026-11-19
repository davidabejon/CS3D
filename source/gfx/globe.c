#include "globe.h"
#include "geodata.h"
#include "tiles.h"
#include "ui.h"

#include <citro3d.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI      3.14159265f
#define DEG2RAD (PI / 180.0f)
/* Web Mercator stops here; beyond, the ocean disc shows. */
#define MERC_MAX_LAT 85.0511f

/* Colours of the globe itself (the palette of the rest is in ui.h). */
#define COL_GLOW     C2D_Color32(0x1C, 0x3A, 0x4A, 0xFF)
#define COL_OCEAN    C2D_Color32(0x0F, 0x22, 0x33, 0xFF)
#define COL_OSM_SEA  C2D_Color32(0xAA, 0xD3, 0xDF, 0xFF)   /* OSM's water */
#define COL_OSM_GLOW C2D_Color32(0x3C, 0x6E, 0x88, 0xFF)

/* Tiles and vertices drawn per frame. */
#define MAX_TILES 64
#define WANT_TILES 40            /* asked for; the rest only use coarser ones */
#define MAX_VERTS 32768
/* Wanted size of a tile on screen, in pixels: picks the zoom level. */
#define TILE_TARGET_PX 300.0f

/* Land shades of the offline globe, from the rim (0) to the centre. */
#define LAND_SHADES 6
static u32 land_shade[LAND_SHADES];
static globe_vec *land;
static int land_count;

/* GPU side: gfx/globe.v.pica, assembled by build.py. */
extern const unsigned char globe_shbin[];
extern const unsigned int globe_shbin_size;

typedef struct {
	float x, y, z;
	float u, v;
	u8    shade[4];
} vertex;

static DVLB_s *dvlb;
static shaderProgram_s program;
static int loc_projection;
static vertex *vbo;
static bool gpu_ready;

static u32 mix(u32 a, u32 b, float t)
{
	/* citro2d colours are 0xAABBGGRR. */
	u32 out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
		out |= (u32)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

globe_vec globe_vec_from(float lat, float lon)
{
	float cl = cosf(lat * DEG2RAD);
	globe_vec p = {cl * cosf(lon * DEG2RAD), cl * sinf(lon * DEG2RAD),
	               sinf(lat * DEG2RAD)};
	return p;
}

void globe_init(void)
{
	if (land)
		return;
	int n = 0;
	for (int k = 0; k < GEO_COLS * GEO_ROWS; k++)
		n += (GEO_LAND[k >> 3] >> (k & 7)) & 1;
	land = malloc(n * sizeof(*land));
	if (land) {
		for (int k = 0; k < GEO_COLS * GEO_ROWS; k++) {
			if (!((GEO_LAND[k >> 3] >> (k & 7)) & 1))
				continue;
			int row = k / GEO_COLS, col = k % GEO_COLS;
			float lat = 90.0f - (row + 0.5f) * GEO_GRID_DEG;
			float lon = -180.0f + (col + 0.5f) * GEO_GRID_DEG;
			land[land_count++] = globe_vec_from(lat, lon);
		}
	}
	u32 dark = C2D_Color32(0x24, 0x4A, 0x3E, 0xFF);
	u32 light = C2D_Color32(0x5E, 0xA8, 0x7A, 0xFF);
	for (int i = 0; i < LAND_SHADES; i++)
		land_shade[i] = mix(dark, light, (float)i / (LAND_SHADES - 1));

	dvlb = DVLB_ParseFile((u32 *)globe_shbin, globe_shbin_size);
	vbo = linearAlloc(MAX_VERTS * sizeof(vertex));
	if (dvlb && vbo) {
		shaderProgramInit(&program);
		shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
		loc_projection = shaderInstanceGetUniformLocation(program.vertexShader,
		                                                  "projection");
		gpu_ready = true;
	}
}

void globe_exit(void)
{
	if (gpu_ready)
		shaderProgramFree(&program);
	if (dvlb)
		DVLB_Free(dvlb);
	if (vbo)
		linearFree(vbo);
	dvlb = NULL;
	vbo = NULL;
	gpu_ready = false;
	free(land);
	land = NULL;
	land_count = 0;
}

/* ---------------------------------------------------------------- rotation */

/* Per-frame rotation: the sines and cosines of the view centre. */
typedef struct {
	float slon, clon, slat, clat;
} rotation;

static rotation rotation_of(const globe_view *v)
{
	rotation r = {sinf(v->lon0 * DEG2RAD), cosf(v->lon0 * DEG2RAD),
	              sinf(v->lat0 * DEG2RAD), cosf(v->lat0 * DEG2RAD)};
	return r;
}

static inline void rotate(const rotation *r, globe_vec p, float *x, float *y,
                          float *z)
{
	float a = p.x * r->clon + p.y * r->slon;    /* cos(lat) cos(dlon) */
	float b = p.y * r->clon - p.x * r->slon;    /* cos(lat) sin(dlon) */
	*x = b;
	*y = r->clat * p.z - r->slat * a;
	*z = r->slat * p.z + r->clat * a;
}

bool globe_project(const globe_view *v, globe_vec p, float *sx, float *sy,
                   float *depth)
{
	rotation r = rotation_of(v);
	float x, y, z;
	rotate(&r, p, &x, &y, &z);
	*sx = v->cx + x * v->radius;
	*sy = v->cy - y * v->radius;
	*depth = z;
	return z > 0.0f;
}

/* The point of the globe under screen pixel (sx, sy); false off the globe. */
static bool unproject(const globe_view *v, const rotation *r, float sx,
                      float sy, float *lat, float *lon)
{
	float x = (sx - v->cx) / v->radius, y = (v->cy - sy) / v->radius;
	float r2 = x * x + y * y;
	if (r2 >= 1.0f)
		return false;
	float z = sqrtf(1.0f - r2);
	/* The inverse of rotate(): its (y, z) step is its own inverse. */
	float a = -r->slat * y + r->clat * z;
	float pz = r->clat * y + r->slat * z;
	float px = a * r->clon - x * r->slon;
	float py = a * r->slon + x * r->clon;
	*lat = asinf(pz > 1.0f ? 1.0f : pz < -1.0f ? -1.0f : pz) / DEG2RAD;
	*lon = atan2f(py, px) / DEG2RAD;
	return true;
}

/* --------------------------------------------------------- offline globe */

static void draw_dots(const globe_view *v)
{
	C2D_DrawCircleSolid(v->cx, v->cy, 0.0f, v->radius + 4.0f, COL_GLOW);
	C2D_DrawCircleSolid(v->cx, v->cy, 0.0f, v->radius, COL_OCEAN);

	rotation r = rotation_of(v);
	/* A cell spans GEO_GRID_DEG degrees; the dots leave a gap between them. */
	float cell = v->radius * GEO_GRID_DEG * DEG2RAD * 0.78f;
	if (cell < 1.5f)
		cell = 1.5f;
	for (int i = 0; i < land_count; i++) {
		float x, y, z;
		rotate(&r, land[i], &x, &y, &z);
		if (z <= 0.05f)
			continue;
		float sx = v->cx + x * v->radius, sy = v->cy - y * v->radius;
		if (sx < -cell || sx > SCREEN_TOP_W + cell || sy < -cell ||
		    sy > SCREEN_H + cell)
			continue;
		/* Near the rim the cells are seen edge-on: smaller and darker. */
		float s = cell * (0.45f + 0.55f * z);
		int shade = (int)(z * LAND_SHADES);
		if (shade >= LAND_SHADES)
			shade = LAND_SHADES - 1;
		ui_rect(sx - s / 2, sy - s / 2, s, s, land_shade[shade]);
	}
}

/* ------------------------------------------------------------ map tiles */

typedef struct {
	int   x, y;
	float dist;              /* from the centre of the screen, to order them */
} tile_ref;

static int tile_level(float radius)
{
	int z = (int)ceilf(log2f(radius * 2.0f * PI / TILE_TARGET_PX));
	if (z < TILES_PINNED_Z)
		z = TILES_PINNED_Z;
	if (z > TILES_MAX_Z)
		z = TILES_MAX_Z;
	return z;
}

static void tile_of(float lat, float lon, int z, int *tx, int *ty)
{
	int n = 1 << z;
	if (lat > MERC_MAX_LAT)
		lat = MERC_MAX_LAT;
	if (lat < -MERC_MAX_LAT)
		lat = -MERC_MAX_LAT;
	float l = lat * DEG2RAD;
	float fy = (1.0f - logf(tanf(l) + 1.0f / cosf(l)) / PI) / 2.0f;
	int x = (int)((lon + 180.0f) / 360.0f * n);
	int y = (int)(fy * n);
	*tx = x < 0 ? 0 : x >= n ? n - 1 : x;
	*ty = y < 0 ? 0 : y >= n ? n - 1 : y;
}

static int add_tile(tile_ref *list, int count, int x, int y, float dist)
{
	for (int i = 0; i < count; i++) {
		if (list[i].x == x && list[i].y == y) {
			if (dist < list[i].dist)
				list[i].dist = dist;
			return count;
		}
	}
	if (count < MAX_TILES)
		list[count++] = (tile_ref){x, y, dist};
	return count;
}

static int by_dist(const void *a, const void *b)
{
	float da = ((const tile_ref *)a)->dist, db = ((const tile_ref *)b)->dist;
	return (da > db) - (da < db);
}

/* The tiles of level z that show on screen, nearest the centre first. */
static int visible_tiles(const globe_view *v, const rotation *r, int z,
                         tile_ref *list)
{
	int count = 0;
	if (z <= TILES_PINNED_Z) {
		/* Few and large: all of them; the back ones cost nothing. */
		for (int y = 0; y < (1 << z); y++)
			for (int x = 0; x < (1 << z); x++)
				list[count++] = (tile_ref){x, y, 0.0f};
		return count;
	}
	/* Sample the screen where the globe covers it, plus its rim, where
	 * tiles are squeezed thinner than the grid. */
	float x0 = fmaxf(0.0f, v->cx - v->radius), x1 = fminf(SCREEN_TOP_W, v->cx + v->radius);
	float y0 = fmaxf(0.0f, v->cy - v->radius), y1 = fminf(SCREEN_H, v->cy + v->radius);
	const float step = 12.0f;
	for (float sy = y0; sy <= y1 + step / 2; sy += step) {
		for (float sx = x0; sx <= x1 + step / 2; sx += step) {
			float lat, lon;
			if (!unproject(v, r, sx, sy, &lat, &lon))
				continue;
			int tx, ty;
			tile_of(lat, lon, z, &tx, &ty);
			float dx = sx - v->cx, dy = sy - v->cy;
			count = add_tile(list, count, tx, ty, dx * dx + dy * dy);
		}
	}
	if (v->radius < 2.0f * SCREEN_TOP_W) {
		for (int i = 0; i < 128; i++) {
			float a = (float)i * (2.0f * PI / 128.0f);
			float sx = v->cx + cosf(a) * v->radius * 0.995f;
			float sy = v->cy + sinf(a) * v->radius * 0.995f;
			float lat, lon;
			if (sx < 0 || sx > SCREEN_TOP_W || sy < 0 || sy > SCREEN_H ||
			    !unproject(v, r, sx, sy, &lat, &lon))
				continue;
			int tx, ty;
			tile_of(lat, lon, z, &tx, &ty);
			count = add_tile(list, count, tx, ty, v->radius * v->radius);
		}
	}
	qsort(list, count, sizeof(*list), by_dist);
	return count;
}

/* Grid lines per tile side: enough that the flat cells bend with the globe
 * to within half a pixel. */
static int subdivisions(float radius, int z)
{
	float span = 2.0f * PI / (float)(1 << z);
	float cell = sqrtf(4.0f / radius);      /* sagitta R*c^2/8 <= 0.5 px */
	int n = (int)ceilf(span / cell);
	return n < 1 ? 1 : n > 12 ? 12 : n;
}

#define GRID_MAX 13
typedef struct {
	float sx, sy, depth;
} grid_point;

/* Appends tile (z, x, y)'s mesh to vbo at *used, texturing it with the
 * sub-square (u0, v0, size) of its texture. Returns the vertices added. */
static int build_tile(const globe_view *v, const rotation *r, int z, int x,
                      int y, float u0, float v0, float size, int *used)
{
	int n = subdivisions(v->radius, z);
	float tiles = (float)(1 << z);
	grid_point g[GRID_MAX][GRID_MAX];
	float lons_c[GRID_MAX], lons_s[GRID_MAX];
	for (int i = 0; i <= n; i++) {
		float lon = ((float)x + (float)i / n) / tiles * 2.0f * PI - PI;
		lons_c[i] = cosf(lon);
		lons_s[i] = sinf(lon);
	}
	for (int j = 0; j <= n; j++) {
		float fy = ((float)y + (float)j / n) / tiles;
		float lat = atanf(sinhf(PI * (1.0f - 2.0f * fy)));
		float cl = cosf(lat), sl = sinf(lat);
		for (int i = 0; i <= n; i++) {
			globe_vec p = {cl * lons_c[i], cl * lons_s[i], sl};
			float px, py, pz;
			rotate(r, p, &px, &py, &pz);
			if (pz < 0.0f) {
				/* Behind: pin it to the rim, so cells that straddle it
				 * still close the outline. */
				float len = sqrtf(px * px + py * py);
				if (len > 0.0f) {
					px /= len;
					py /= len;
				}
			}
			g[j][i] = (grid_point){v->cx + px * v->radius,
			                       v->cy - py * v->radius, pz};
		}
	}

	int start = *used;
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			const grid_point *c[4] = {&g[j][i], &g[j][i + 1],
			                          &g[j + 1][i + 1], &g[j + 1][i]};
			if (c[0]->depth < 0 && c[1]->depth < 0 && c[2]->depth < 0 &&
			    c[3]->depth < 0)
				continue;
			bool left = true, right = true, above = true, below = true;
			for (int k = 0; k < 4; k++) {
				left &= c[k]->sx < 0;
				right &= c[k]->sx > SCREEN_TOP_W;
				above &= c[k]->sy < 0;
				below &= c[k]->sy > SCREEN_H;
			}
			if (left || right || above || below)
				continue;
			if (*used + 6 > MAX_VERTS)
				return *used - start;
			static const int order[6] = {0, 1, 2, 0, 2, 3};
			static const int du[4] = {0, 1, 1, 0}, dv[4] = {0, 0, 1, 1};
			for (int k = 0; k < 6; k++) {
				int q = order[k];
				float d = c[q]->depth;
				u8 shade = (u8)(255.0f * (0.62f + 0.38f * (d > 0 ? d : 0)));
				vbo[(*used)++] = (vertex){
					c[q]->sx, c[q]->sy, 0.0f,
					u0 + size * (float)(i + du[q]) / n,
					/* t = 1 is the first row in memory: the tile's top. */
					1.0f - (v0 + size * (float)(j + dv[q]) / n),
					{shade, shade, shade, 0xFF}};
			}
		}
	}
	return *used - start;
}

static void begin_gpu(void)
{
	C2D_Flush();
	C3D_BindProgram(&program);
	C3D_AttrInfo *attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);
	C3D_BufInfo *buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, vbo, sizeof(vertex), 3, 0x210);

	C3D_Mtx projection;
	Mtx_OrthoTilt(&projection, 0.0f, SCREEN_TOP_W, SCREEN_H, 0.0f, -1.0f,
	              1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, loc_projection, &projection);

	/* Texture times shade; the other stages pass it through. */
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	for (int i = 1; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
}

static void draw_tiles(const globe_view *v)
{
	C2D_DrawCircleSolid(v->cx, v->cy, 0.0f, v->radius + 4.0f, COL_OSM_GLOW);
	C2D_DrawCircleSolid(v->cx, v->cy, 0.0f, v->radius, COL_OSM_SEA);

	rotation r = rotation_of(v);
	int z = tile_level(v->radius);
	tile_ref list[MAX_TILES];
	int count = visible_tiles(v, &r, z, list);

	begin_gpu();
	int used = 0;
	for (int t = 0; t < count; t++) {
		int x = list[t].x, y = list[t].y;
		/* This tile, or the closest coarser one already in memory, of
		 * which it is a sub-square. */
		C3D_Tex *tex = tiles_get(z, x, y, t < WANT_TILES);
		int d = 0;
		while (!tex && d < z) {
			d++;
			tex = tiles_get(z - d, x >> d, y >> d, false);
		}
		if (!tex)
			continue;
		float size = 1.0f / (float)(1 << d);
		float u0 = (float)(x - ((x >> d) << d)) * size;
		float v0 = (float)(y - ((y >> d) << d)) * size;
		int first = used;
		int added = build_tile(v, &r, z, x, y, u0, v0, size, &used);
		if (added > 0) {
			C3D_TexBind(0, tex);
			C3D_DrawArrays(GPU_TRIANGLES, first, added);
		}
		if (used + 6 > MAX_VERTS)
			break;
	}
	C2D_Prepare();
}

void globe_draw(const globe_view *v)
{
	tiles_frame();
	/* The map once its first tile is here; until then (or without a
	 * network), the offline globe. */
	if (gpu_ready && tiles_get(0, 0, 0, false))
		draw_tiles(v);
	else
		draw_dots(v);
}

bool globe_has_map(void)
{
	return gpu_ready && tiles_get(0, 0, 0, false);
}

/* ---------------------------------------------------------------- helpers */

static int compare_code(const void *key, const void *item)
{
	return strcmp((const char *)key, ((const geo_country *)item)->code);
}

bool globe_country_centre(const char *code, float *lat, float *lon)
{
	if (!code || !code[0] || !code[1] || code[2])
		return false;
	char key[3] = {(char)toupper((unsigned char)code[0]),
	               (char)toupper((unsigned char)code[1]), '\0'};
	const geo_country *c = bsearch(key, GEO_COUNTRIES, GEO_COUNTRY_COUNT,
	                               sizeof(GEO_COUNTRIES[0]), compare_code);
	if (!c)
		return false;
	*lat = c->lat;
	*lon = c->lon;
	return true;
}

float globe_wrap_deg(float deg)
{
	deg = fmodf(deg + 180.0f, 360.0f);
	if (deg < 0)
		deg += 360.0f;
	return deg - 180.0f;
}
