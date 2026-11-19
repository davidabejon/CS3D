/* Client for the radio-browser.info directory (https://api.radio-browser.info).
 *
 * A worker thread owns one keep-alive HTTPS session to one of the API mirrors
 * and fails over to the next when it stops answering. The main thread asks
 * for things (rb_search, rb_load_more, rb_click) and reads the list back
 * (rb_info_get, rb_station_get); nothing here blocks it.
 *
 * Only stations the console can play are listed: MP3, not marked broken by
 * the directory's own checks, not HLS. Sorted by popularity (clicks).
 *
 * Start after tls_global_init(); stop with tls_set_abort(true) set, so a
 * request in flight gives up at once.
 */
#ifndef RADIOBROWSER_H
#define RADIOBROWSER_H

#include <stdbool.h>

/* Stations per request; the list grows a page at a time. */
#define RB_PAGE 50

typedef struct {
	char  uuid[40];          /* stationuuid: stable id, used for clicks */
	char  name[96];          /* UTF-8, cut at a character boundary */
	char  url[384];          /* url_resolved (playlists already expanded) */
	char  country[48];
	char  countrycode[4];    /* ISO 3166-1 alpha-2, may be empty */
	char  tags[96];          /* comma-separated, cut */
	int   bitrate;           /* kbps, 0 = unknown */
	int   votes;
	bool  has_geo;           /* lat/lon present: for the future globe */
	float lat, lon;
} rb_station;

typedef enum {
	RB_IDLE,                 /* nothing asked yet */
	RB_LOADING,
	RB_READY,
	RB_FAILED                /* see rb_info.error; the list keeps what it had */
} rb_state;

typedef struct {
	rb_state state;
	int      count;          /* stations in the list */
	bool     more;           /* the last page was full: rb_load_more() helps */
	char     query[64];      /* "" = the global top */
	char     server[48];     /* mirror in use */
	char     error[160];
} rb_info;

bool rb_init(void);
void rb_exit(void);

/* Replaces the list with the first page for `name` ("" = most popular). */
void rb_search(const char *name);
/* Appends the next page of the current query, if there is one and nothing
 * is loading. */
void rb_load_more(void);
/* Tells the directory a station was played: it asks clients to, and it
 * feeds the popularity order. Fire and forget. */
void rb_click(const char *uuid);

void rb_info_get(rb_info *out);
/* Copies station `index`; false when out of range. */
bool rb_station_get(int index, rb_station *out);

#endif
