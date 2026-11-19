/* OpenStreetMap raster tiles (https://tile.openstreetmap.org) as GPU
 * textures, for the globe (gfx/globe.c).
 *
 * A worker thread fetches what the globe asks for, newest request first, over
 * one keep-alive HTTPS connection, and keeps every tile it downloads on the SD
 * card (sdmc:/3ds/CS3D/tiles/z/x/y.png), so a place seen once loads from the
 * card next time and costs the tile servers nothing. PNGs are decoded on the
 * worker into RGB565 in the GPU's tiled layout; the main thread only copies
 * them into textures, a few per frame.
 *
 * TILES_SLOTS textures stay in memory (128 KB each, linear heap); the oldest
 * unused one makes room for a new tile. Zoom levels 0 to TILES_PINNED_Z are
 * asked for at start and never evicted, so there is always a coarser tile to
 * stretch while a sharper one loads.
 *
 * OSM's tile usage policy applies: a real User-Agent (HTTP_USER_AGENT), no
 * bulk downloads (only what is on screen is requested), the attribution on
 * screen, and the local cache.
 *
 * Start after tls_global_init(); stop with tls_set_abort(true). Everything
 * but the worker runs on the main thread, inside the frame (after
 * C3D_FrameBegin), since textures are replaced in place.
 */
#ifndef TILES_H
#define TILES_H

#include <citro3d.h>
#include <stdbool.h>

#define TILE_PX        256
#define TILES_MAX_Z    17
#define TILES_PINNED_Z 2
#define TILES_SLOTS    80

typedef struct {
	int  loaded;             /* tiles in memory */
	int  waiting;            /* asked for, not here yet */
	bool failing;            /* the last downloads failed */
	char error[96];
} tiles_info;

bool tiles_init(void);
void tiles_exit(void);

/* Once per frame, inside the frame and before any tiles_get(): moves a few
 * decoded tiles into their textures. */
void tiles_frame(void);

/* The texture of tile (z, x, y) if it is in memory. Otherwise NULL, and with
 * `want` it is asked for (dropped again if no frame wants it any more). */
C3D_Tex *tiles_get(int z, int x, int y, bool want);

void tiles_info_get(tiles_info *out);

#endif
