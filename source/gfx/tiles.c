#include "tiles.h"
#include "http3ds.h"
#include "tls3ds.h"

#include <3ds.h>
#include <png.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TILE_HOST    "tile.openstreetmap.org"
#define CACHE_DIR    "sdmc:/3ds/CS3D/tiles"
#define REPLY_LIMIT  (512 * 1024)
#define WORKER_STACK (64 * 1024)
/* A request nobody asked for again in this many frames is not fetched. */
#define STALE_FRAMES 3
/* Frames before a failed tile is tried again (60 per second). */
#define RETRY_FRAMES   (10 * 60)
#define MISSING_FRAMES (10 * 60 * 60)
/* Decoded tiles copied into textures per frame (128 KB each). */
#define UPLOADS_PER_FRAME 3

typedef enum {
	SLOT_FREE,
	SLOT_QUEUED,
	SLOT_LOADING,              /* the worker has it */
	SLOT_DECODED,              /* pixels ready, texture not yet updated */
	SLOT_READY,
	SLOT_FAILED,
} slot_state;

typedef struct {
	int        z, x, y;
	slot_state state;
	u32        used;           /* last frame tiles_get() asked for it */
	u32        order;          /* request order within that frame */
	u32        retry;          /* SLOT_FAILED: frame to try again */
	u16       *pixels;         /* SLOT_DECODED: RGB565, tiled */
	C3D_Tex    tex;
	bool       has_tex;
} slot;

static struct {
	LightLock     lock;        /* the slots' state, key and pixels */
	LightEvent    wake;
	Thread        thread;
	volatile bool quit;
	slot          slots[TILES_SLOTS];
	u32           frame;
	u32           order;
	int           fails;       /* downloads failed in a row */
	char          error[96];
} T;

/* --------------------------------------------------------------- decoding */

/* Offset of texel (x, y) in the GPU's layout: 8x8 tiles in rows, Morton
 * order inside each. Row 0 of the image (its top) goes first, which the GPU
 * samples at t = 1. */
static inline u32 tiled_index(u32 x, u32 y)
{
	u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
	        ((x & 4) << 2) | ((y & 4) << 3);
	return (((y >> 3) * (TILE_PX >> 3) + (x >> 3)) << 6) + m;
}

static u16 *decode(const void *png, size_t len, char *err, size_t errlen)
{
	png_image img;
	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&img, png, len)) {
		snprintf(err, errlen, "PNG: %s", img.message);
		return NULL;
	}
	if (img.width != TILE_PX || img.height != TILE_PX) {
		png_image_free(&img);
		snprintf(err, errlen, "PNG de %ux%u", (unsigned)img.width,
		         (unsigned)img.height);
		return NULL;
	}
	img.format = PNG_FORMAT_RGB;
	u8 *rgb = malloc(TILE_PX * TILE_PX * 3);
	u16 *out = malloc(TILE_PX * TILE_PX * sizeof(u16));
	if (!rgb || !out ||
	    !png_image_finish_read(&img, NULL, rgb, 0, NULL)) {
		snprintf(err, errlen, rgb && out ? "PNG: %s" : "sin memoria",
		         img.message);
		png_image_free(&img);
		free(rgb);
		free(out);
		return NULL;
	}
	const u8 *p = rgb;
	for (u32 y = 0; y < TILE_PX; y++)
		for (u32 x = 0; x < TILE_PX; x++, p += 3)
			out[tiled_index(x, y)] =
				(u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
	free(rgb);
	return out;
}

/* ------------------------------------------------------------------ cache */

static void cache_path(char *out, size_t len, int z, int x, int y)
{
	snprintf(out, len, CACHE_DIR "/%d/%d/%d.png", z, x, y);
}

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	void *data = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long n = ftell(f);
		if (n > 0 && n <= REPLY_LIMIT && fseek(f, 0, SEEK_SET) == 0) {
			data = malloc((size_t)n);
			if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
				free(data);
				data = NULL;
			}
			*len = (size_t)n;
		}
	}
	fclose(f);
	return data;
}

/* Creates every directory of `path` up to its last '/'. */
static void make_dirs(const char *path)
{
	char dir[96];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *s = dir + strlen("sdmc:/"); *s; s++) {
		if (*s != '/')
			continue;
		*s = '\0';
		mkdir(dir, 0777);
		*s = '/';
	}
}

/* Written to a temporary name first: a half-written file never counts. */
static void write_file(const char *path, const void *data, size_t len)
{
	char tmp[100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	make_dirs(path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fclose(f) == 0 && ok;
	if (ok)
		ok = rename(tmp, path) == 0;
	if (!ok)
		remove(tmp);
}

/* ----------------------------------------------------------------- worker */

/* The PNG of a tile, from the card or the network. On failure, NULL with
 * `*missing` when the server says the tile does not exist. */
static void *fetch(http_session **s, int z, int x, int y, size_t *len,
                   bool *missing, char *err, size_t errlen)
{
	char path[64];
	cache_path(path, sizeof(path), z, x, y);
	void *data = read_file(path, len);
	if (data)
		return data;

	if (!*s && !(*s = http_session_open(TILE_HOST))) {
		snprintf(err, errlen, "sin memoria");
		return NULL;
	}
	char url[48];
	snprintf(url, sizeof(url), "/%d/%d/%d.png", z, x, y);
	http_reply r;
	if (http_session_request(*s, url, "GET", NULL, NULL, "image/png", NULL, 0,
	                         REPLY_LIMIT, &r) != 0) {
		snprintf(err, errlen, "%.90s", r.err);
		http_session_close(*s);
		*s = NULL;
		return NULL;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "HTTP %d", r.status);
		*missing = r.status == 404;
		http_reply_free(&r);
		return NULL;
	}
	write_file(path, r.body, r.len);
	*len = r.len;
	return r.body;                 /* malloc'd by http3ds: the caller frees */
}

/* The most wanted queued slot: asked for in the latest frames, earliest in
 * that frame (the globe asks from the centre out). -1 when none. */
static int next_job(void)
{
	int best = -1;
	for (int i = 0; i < TILES_SLOTS; i++) {
		const slot *s = &T.slots[i];
		if (s->state != SLOT_QUEUED ||
		    (s->z > TILES_PINNED_Z && T.frame - s->used > STALE_FRAMES))
			continue;
		if (best < 0 || s->used > T.slots[best].used ||
		    (s->used == T.slots[best].used &&
		     s->order < T.slots[best].order))
			best = i;
	}
	return best;
}

static void worker(void *arg)
{
	(void)arg;
	http_session *session = NULL;
	while (!T.quit) {
		LightLock_Lock(&T.lock);
		int i = next_job();
		int z = 0, x = 0, y = 0;
		if (i >= 0) {
			T.slots[i].state = SLOT_LOADING;
			z = T.slots[i].z;
			x = T.slots[i].x;
			y = T.slots[i].y;
		}
		LightLock_Unlock(&T.lock);
		if (i < 0) {
			/* Woken by new requests; the timeout picks up stale ones that
			 * became wanted again. */
			LightEvent_WaitTimeout(&T.wake, 200 * 1000 * 1000LL);
			continue;
		}

		char err[96] = "";
		size_t len = 0;
		bool missing = false;
		void *png = fetch(&session, z, x, y, &len, &missing, err, sizeof(err));
		u16 *pixels = png ? decode(png, len, err, sizeof(err)) : NULL;
		if (png && !pixels) {
			/* A corrupt file on the card: drop it, download it next time. */
			char path[64];
			cache_path(path, sizeof(path), z, x, y);
			remove(path);
		}
		free(png);

		LightLock_Lock(&T.lock);
		slot *s = &T.slots[i];
		bool same = s->state == SLOT_LOADING && s->z == z && s->x == x &&
		            s->y == y;
		if (pixels) {
			T.fails = 0;
			T.error[0] = '\0';
		} else if (!missing) {
			T.fails++;
			snprintf(T.error, sizeof(T.error), "%s", err);
		}
		if (same && pixels) {
			s->pixels = pixels;
			s->state = SLOT_DECODED;
			pixels = NULL;
		} else if (same) {
			s->state = SLOT_FAILED;
			s->retry = T.frame + (missing ? MISSING_FRAMES : RETRY_FRAMES);
		}
		LightLock_Unlock(&T.lock);
		free(pixels);              /* the slot was given away meanwhile */
	}
	http_session_close(session);
}

/* ------------------------------------------------------------- main thread */

bool tiles_init(void)
{
	LightLock_Init(&T.lock);
	LightEvent_Init(&T.wake, RESET_ONESHOT);
	T.quit = false;
	/* Coarse levels first, so the globe has something to show. */
	for (int z = 0; z <= TILES_PINNED_Z; z++)
		for (int y = 0; y < (1 << z); y++)
			for (int x = 0; x < (1 << z); x++)
				tiles_get(z, x, y, true);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	/* Below the directory's worker: decoding PNGs must not delay the UI or
	 * the audio. */
	T.thread = threadCreate(worker, NULL, WORKER_STACK,
	                        prio < 0x3E ? prio + 2 : 0x3F, -2, false);
	return T.thread != NULL;
}

void tiles_exit(void)
{
	if (T.thread) {
		T.quit = true;
		LightEvent_Signal(&T.wake);
		threadJoin(T.thread, U64_MAX);
		threadFree(T.thread);
		T.thread = NULL;
	}
	for (int i = 0; i < TILES_SLOTS; i++) {
		slot *s = &T.slots[i];
		free(s->pixels);
		s->pixels = NULL;
		if (s->has_tex)
			C3D_TexDelete(&s->tex);
		s->has_tex = false;
		s->state = SLOT_FREE;
	}
}

void tiles_frame(void)
{
	LightLock_Lock(&T.lock);
	T.frame++;
	T.order = 0;
	int uploads = 0;
	for (int i = 0; i < TILES_SLOTS && uploads < UPLOADS_PER_FRAME; i++) {
		slot *s = &T.slots[i];
		if (s->state != SLOT_DECODED)
			continue;
		if (!s->has_tex) {
			s->has_tex = C3D_TexInit(&s->tex, TILE_PX, TILE_PX, GPU_RGB565);
			if (s->has_tex) {
				C3D_TexSetFilter(&s->tex, GPU_LINEAR, GPU_LINEAR);
				C3D_TexSetWrap(&s->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			}
		}
		if (s->has_tex) {
			memcpy(s->tex.data, s->pixels, TILE_PX * TILE_PX * sizeof(u16));
			C3D_TexFlush(&s->tex);
			s->state = SLOT_READY;
		} else {
			/* Out of linear memory: this slot cannot hold a texture. */
			snprintf(T.error, sizeof(T.error), "sin memoria para texturas");
			s->state = SLOT_FAILED;
			s->retry = T.frame + MISSING_FRAMES;
		}
		free(s->pixels);
		s->pixels = NULL;
		uploads++;
	}
	LightLock_Unlock(&T.lock);
}

static int find(int z, int x, int y)
{
	for (int i = 0; i < TILES_SLOTS; i++) {
		const slot *s = &T.slots[i];
		if (s->state != SLOT_FREE && s->z == z && s->x == x && s->y == y)
			return i;
	}
	return -1;
}

/* A slot for a new tile: a free one, or the one unused the longest that is
 * not pinned, not used this frame and not in the worker's hands. */
static int claim(void)
{
	int best = -1;
	for (int i = 0; i < TILES_SLOTS; i++) {
		const slot *s = &T.slots[i];
		if (s->state == SLOT_FREE)
			return i;
		if (s->z <= TILES_PINNED_Z || s->used == T.frame ||
		    s->state == SLOT_LOADING || s->state == SLOT_DECODED)
			continue;
		if (best < 0 || s->used < T.slots[best].used)
			best = i;
	}
	return best;
}

C3D_Tex *tiles_get(int z, int x, int y, bool want)
{
	C3D_Tex *tex = NULL;
	bool signal = false;
	LightLock_Lock(&T.lock);
	int i = find(z, x, y);
	if (i >= 0) {
		slot *s = &T.slots[i];
		if (want || s->state == SLOT_READY)
			s->used = T.frame;
		if (s->state == SLOT_READY) {
			tex = &s->tex;
		} else if (want && s->state == SLOT_FAILED &&
		           (s32)(T.frame - s->retry) >= 0) {
			s->state = SLOT_QUEUED;
			signal = true;
		} else if (want && s->state == SLOT_QUEUED) {
			s->order = ++T.order;
			signal = true;
		}
	} else if (want && (i = claim()) >= 0) {
		slot *s = &T.slots[i];
		s->z = z;
		s->x = x;
		s->y = y;
		s->state = SLOT_QUEUED;
		s->used = T.frame;
		s->order = ++T.order;
		signal = true;
	}
	LightLock_Unlock(&T.lock);
	if (signal)
		LightEvent_Signal(&T.wake);
	return tex;
}

void tiles_info_get(tiles_info *out)
{
	memset(out, 0, sizeof(*out));
	LightLock_Lock(&T.lock);
	for (int i = 0; i < TILES_SLOTS; i++) {
		const slot *s = &T.slots[i];
		if (s->state == SLOT_READY)
			out->loaded++;
		else if ((s->state == SLOT_QUEUED &&
		          T.frame - s->used <= STALE_FRAMES) ||
		         s->state == SLOT_LOADING || s->state == SLOT_DECODED)
			out->waiting++;
	}
	out->failing = T.fails >= 3;
	snprintf(out->error, sizeof(out->error), "%s", T.error);
	LightLock_Unlock(&T.lock);
}
