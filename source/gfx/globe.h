/* An orthographic globe: OpenStreetMap tiles (gfx/tiles.h) wrapped on a
 * sphere with citro3d, at the tile zoom level that matches the globe's size
 * on screen. Until the first tile arrives (or with no network) it falls back
 * to an offline globe: the land as a grid of dots (gfx/geodata.c).
 *
 * The CPU projects each tile's grid of vertices (finer for big, curved
 * tiles; a single quad when zoomed in on a city) and the GPU only textures
 * them (gfx/globe.v.pica), between citro2d draws.
 *
 * The view looks straight at (lat0, lon0); `radius` is the globe's radius in
 * pixels, so zooming is just a bigger radius. Points are kept as unit
 * vectors, so turning the globe costs a few multiplications per point.
 */
#ifndef GLOBE_H
#define GLOBE_H

#include <stdbool.h>

typedef struct {
	float lat0, lon0;        /* degrees: the point at the centre of the view */
	float cx, cy;            /* centre on screen */
	float radius;            /* pixels */
} globe_view;

/* A point on the sphere as a unit vector (x towards lon 0, y towards lon 90 E,
 * z towards the north pole). */
typedef struct {
	float x, y, z;
} globe_vec;

/* Precomputes the land dots and sets up the shader. Call once, after
 * ui_init(). */
void globe_init(void);
void globe_exit(void);

globe_vec globe_vec_from(float lat, float lon);

/* Screen position of `p`. `depth` is 1 at the centre of the visible face,
 * 0 at the rim and negative behind the globe. Returns depth > 0. */
bool globe_project(const globe_view *v, globe_vec p, float *sx, float *sy,
                   float *depth);

/* Ocean, glow and land, into the current (top screen) scene. Also moves
 * newly decoded tiles into their textures: call it once every frame. */
void globe_draw(const globe_view *v);
/* True when the map tiles are showing (not the offline globe). */
bool globe_has_map(void);

/* Label point of a country by ISO 3166-1 alpha-2 code (any case); false when
 * unknown. */
bool globe_country_centre(const char *code, float *lat, float *lon);

/* Wraps an angle into [-180, 180). */
float globe_wrap_deg(float deg);

#endif
