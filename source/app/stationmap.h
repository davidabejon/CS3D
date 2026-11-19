/* The directory's stations as points on the globe (gfx/globe.h), and the
 * globe's controls: the circle pad turns it, and when it is let go the point
 * nearest the centre of the screen is pulled to the centre and focused.
 *
 * Stations with coordinates go where they say; the rest (about three in
 * four) go to their country's label point, spread a little so they do not
 * all sit on one pixel. Stations with neither are left off the map.
 *
 * Main thread only.
 */
#ifndef STATIONMAP_H
#define STATIONMAP_H

#include <stdbool.h>

#include "radiobrowser.h"

typedef struct {
	float stick_x, stick_y;  /* circle pad, -1..1, up and right positive */
	int   cycle;             /* -1 / +1: previous / next station at the centre */
	int   zoom;              /* -1 / +1 while zooming out / in */
} map_input;

void map_init(void);
void map_exit(void);

/* Picks up stations added to the list (or starts over after a new search).
 * Call every frame after the list may have changed. */
void map_sync(const rb_info *info);
void map_update(const map_input *in);

/* List index (for rb_station_get) of the focused station, or -1. */
int  map_focused(void);
/* True once the globe has settled on the focused station. */
bool map_centred(void);
int  map_point_count(void);
/* Stations of the list that could not be placed. */
int  map_unplaced(void);

/* The globe on the top screen, centred below a header MAP_HEADER_H pixels
 * tall (draw the header after it). `playing_uuid` (or NULL) is drawn in
 * another colour. */
#define MAP_HEADER_H 28
void map_draw(const char *playing_uuid, unsigned frame);

#endif
