#include "radiobrowser.h"
#include "http3ds.h"
#include "json3ds.h"
#include "tls3ds.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The API's mirrors. all.api round-robins over whichever are up; the list
 * of live ones is at /json/servers (de1 and de2 when this was written).
 * Each must be in tools/hosts.txt so its root is in ca_bundle.c. */
static const char *const HOSTS[] = {
	"de1.api.radio-browser.info",
	"de2.api.radio-browser.info",
	"all.api.radio-browser.info",
};
#define HOST_COUNT (int)(sizeof(HOSTS) / sizeof(HOSTS[0]))

/* One page of stations is ~60 KB of JSON; this leaves ample room. */
#define REPLY_LIMIT  (1024 * 1024)
#define WORKER_STACK (64 * 1024)      /* TLS handshake, as in main.c */

/* ---------------------------------------------------------------- state */

typedef struct {
	bool     pending;
	char     query[64];
	int      offset;
	unsigned gen;
} search_job;

static struct {
	LightLock   lock;          /* everything below except the thread */
	LightEvent  wake;
	Thread      thread;
	volatile bool quit;

	search_job  job;
	char        click[40];     /* a uuid to report, "" when none */
	unsigned    gen;           /* bumped by rb_search: stale pages drop */
	int         next_offset;   /* API entries consumed, unplayable included */

	rb_info     info;
	rb_station *list;
	int         cap;
} B;

/* ---------------------------------------------------------- JSON -> station */

/* Cuts `s` to at most `cap - 1` bytes without splitting a UTF-8 character. */
static void utf8_cut(char *s, size_t cap)
{
	size_t n = strlen(s);
	if (n < cap)
		return;
	n = cap - 1;
	while (n && ((unsigned char)s[n] & 0xC0) == 0x80)
		n--;
	s[n] = '\0';
}

static void trim(char *s)
{
	size_t start = strspn(s, " \t\r\n");
	size_t n = strlen(s + start);
	memmove(s, s + start, n + 1);
	while (n && strchr(" \t\r\n", s[n - 1]))
		s[--n] = '\0';
}

/* A string member, decoded and cut to fit `out` instead of being refused. */
static bool member_text(const char *obj, size_t len, const char *key,
                        char *out, size_t outlen)
{
	static char big[1024];         /* the worker is the only caller */
	const char *v;
	size_t vlen;
	out[0] = '\0';
	if (!json_member(obj, len, key, &v, &vlen) ||
	    !json_slice_str(v, vlen, big, sizeof(big)))
		return false;
	trim(big);
	utf8_cut(big, outlen);
	memcpy(out, big, strlen(big) + 1);
	return true;
}

static bool member_float(const char *obj, size_t len, const char *key,
                         float *out)
{
	const char *v;
	size_t vlen;
	if (!json_member(obj, len, key, &v, &vlen))
		return false;
	while (vlen && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r'))
		v++, vlen--;
	if (!vlen || !(*v == '-' || (*v >= '0' && *v <= '9')))
		return false;              /* null */
	*out = strtof(v, NULL);
	return true;
}

static bool playable_url(const char *u)
{
	return strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0;
}

/* False for entries the console cannot play or that are malformed. */
static bool parse_station(const char *obj, size_t len, rb_station *st)
{
	memset(st, 0, sizeof(*st));
	long hls = 0;
	if (json_member_int(obj, len, "hls", &hls) && hls)
		return false;
	if (!member_text(obj, len, "stationuuid", st->uuid, sizeof(st->uuid)) ||
	    !st->uuid[0])
		return false;

	/* url_resolved has playlists (.m3u/.pls) already followed; a URL that
	 * does not fit is dropped rather than cut into a wrong one. */
	static char url[1024];
	const char *v;
	size_t vlen;
	bool have = (json_member(obj, len, "url_resolved", &v, &vlen) &&
	             json_slice_str(v, vlen, url, sizeof(url)) && url[0]) ||
	            (json_member(obj, len, "url", &v, &vlen) &&
	             json_slice_str(v, vlen, url, sizeof(url)) && url[0]);
	if (!have)
		return false;
	trim(url);
	if (!playable_url(url) || strlen(url) >= sizeof(st->url))
		return false;
	memcpy(st->url, url, strlen(url) + 1);

	member_text(obj, len, "name", st->name, sizeof(st->name));
	if (!st->name[0])
		snprintf(st->name, sizeof(st->name), "(sin nombre)");
	member_text(obj, len, "country", st->country, sizeof(st->country));
	member_text(obj, len, "countrycode", st->countrycode,
	            sizeof(st->countrycode));
	member_text(obj, len, "tags", st->tags, sizeof(st->tags));
	long n;
	if (json_member_int(obj, len, "bitrate", &n))
		st->bitrate = (int)n;
	if (json_member_int(obj, len, "votes", &n))
		st->votes = (int)n;
	st->has_geo = member_float(obj, len, "geo_lat", &st->lat) &&
	              member_float(obj, len, "geo_long", &st->lon);
	return true;
}

/* ----------------------------------------------------------------- worker */

/* GET `path` on the current mirror, failing over through the others. On 0
 * the reply is a 2xx and the caller frees it. */
static int api_get(http_session **s, int *host, const char *path,
                   http_reply *reply, char *err, size_t errlen)
{
	for (int tries = 0; tries < HOST_COUNT && !B.quit; tries++) {
		if (!*s) {
			*s = http_session_open(HOSTS[*host]);
			if (!*s) {
				snprintf(err, errlen, "sin memoria");
				return -1;
			}
			LightLock_Lock(&B.lock);
			snprintf(B.info.server, sizeof(B.info.server), "%s",
			         HOSTS[*host]);
			LightLock_Unlock(&B.lock);
		}
		int rc = http_session_request(*s, path, "GET", NULL, NULL,
		                              "application/json", NULL, 0,
		                              REPLY_LIMIT, reply);
		if (rc == 0 && reply->status >= 200 && reply->status < 300)
			return 0;
		if (rc == 0) {
			snprintf(err, errlen, "%s: HTTP %d", HOSTS[*host], reply->status);
			http_reply_free(reply);
			/* A 4xx is our request's fault: another mirror would agree. */
			if (reply->status < 500)
				return -1;
		} else {
			snprintf(err, errlen, "%s: %.120s", HOSTS[*host], reply->err);
		}
		http_session_close(*s);
		*s = NULL;
		*host = (*host + 1) % HOST_COUNT;
	}
	return -1;
}

static void run_search(http_session **s, int *host, const search_job *job)
{
	char name[3 * sizeof(job->query) + 1];
	url_encode(job->query, name, sizeof(name));
	char path[512];
	snprintf(path, sizeof(path),
	         "/json/stations/search?%s%s%scodec=MP3&hidebroken=true"
	         "&order=clickcount&reverse=true&limit=%d&offset=%d",
	         name[0] ? "name=" : "", name, name[0] ? "&" : "",
	         RB_PAGE, job->offset);

	http_reply reply;
	char err[160] = "";
	int rc = api_get(s, host, path, &reply, err, sizeof(err));

	rb_station *page = NULL;
	int got = 0, raw = 0;
	if (rc == 0) {
		page = malloc(RB_PAGE * sizeof(*page));
		size_t cursor = 0;
		const char *obj;
		size_t objlen;
		while (page && got < RB_PAGE &&
		       json_item_next(reply.body, reply.len, &cursor, &obj, &objlen)) {
			raw++;
			if (parse_station(obj, objlen, &page[got]))
				got++;
		}
		if (!page) {
			rc = -1;
			snprintf(err, sizeof(err), "sin memoria");
		}
		http_reply_free(&reply);
	}

	LightLock_Lock(&B.lock);
	if (job->gen == B.gen) {          /* not overtaken by a newer search */
		if (rc != 0) {
			B.info.state = RB_FAILED;
			snprintf(B.info.error, sizeof(B.info.error), "%s", err);
		} else {
			if (B.info.count + got > B.cap) {
				int cap = B.cap ? B.cap * 2 : 2 * RB_PAGE;
				while (cap < B.info.count + got)
					cap *= 2;
				rb_station *bigger = realloc(B.list, cap * sizeof(*bigger));
				if (bigger) {
					B.list = bigger;
					B.cap = cap;
				} else {
					got = 0;
				}
			}
			memcpy(B.list + B.info.count, page, got * sizeof(*page));
			B.info.count += got;
			B.next_offset = job->offset + raw;
			/* A full page (before dropping unplayable ones) means there
			 * may be another. */
			B.info.more = raw == RB_PAGE;
			B.info.state = RB_READY;
			B.info.error[0] = '\0';
		}
	}
	LightLock_Unlock(&B.lock);
	free(page);
}

static void worker(void *arg)
{
	(void)arg;
	http_session *s = NULL;
	/* Spread clients over the mirrors, as the API asks. */
	int host = (int)(svcGetSystemTick() % (HOST_COUNT - 1));

	while (!B.quit) {
		LightEvent_Wait(&B.wake);
		while (!B.quit) {
			LightLock_Lock(&B.lock);
			search_job job = B.job;
			B.job.pending = false;
			char click[sizeof(B.click)];
			memcpy(click, B.click, sizeof(click));
			B.click[0] = '\0';
			LightLock_Unlock(&B.lock);

			if (job.pending) {
				run_search(&s, &host, &job);
			} else if (click[0]) {
				char path[96], err[160];
				http_reply reply;
				snprintf(path, sizeof(path), "/json/url/%s", click);
				if (api_get(&s, &host, path, &reply, err, sizeof(err)) == 0)
					http_reply_free(&reply);
			} else {
				break;
			}
		}
	}
	http_session_close(s);
}

/* ------------------------------------------------------------- public API */

bool rb_init(void)
{
	LightLock_Init(&B.lock);
	LightEvent_Init(&B.wake, RESET_ONESHOT);
	B.quit = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.thread = threadCreate(worker, NULL, WORKER_STACK,
	                        prio < 0x3F ? prio + 1 : 0x3F, -2, false);
	if (!B.thread) {
		B.info.state = RB_FAILED;
		snprintf(B.info.error, sizeof(B.info.error),
		         "no se pudo crear el hilo");
		return false;
	}
	return true;
}

void rb_exit(void)
{
	if (B.thread) {
		B.quit = true;
		LightEvent_Signal(&B.wake);
		threadJoin(B.thread, U64_MAX);
		threadFree(B.thread);
		B.thread = NULL;
	}
	free(B.list);
	B.list = NULL;
	B.cap = 0;
	B.info.count = 0;
}

void rb_search(const char *name)
{
	if (!B.thread)
		return;
	LightLock_Lock(&B.lock);
	B.gen++;
	B.job.pending = true;
	B.job.offset = 0;
	B.job.gen = B.gen;
	snprintf(B.job.query, sizeof(B.job.query), "%s", name);
	trim(B.job.query);
	memcpy(B.info.query, B.job.query, sizeof(B.info.query));
	B.info.count = 0;
	B.next_offset = 0;
	B.info.more = false;
	B.info.state = RB_LOADING;
	B.info.error[0] = '\0';
	LightLock_Unlock(&B.lock);
	LightEvent_Signal(&B.wake);
}

void rb_load_more(void)
{
	if (!B.thread)
		return;
	LightLock_Lock(&B.lock);
	bool go = B.info.state == RB_READY && B.info.more;
	if (go) {
		B.job.pending = true;
		/* Offsets count every entry the API returned, including the ones
		 * dropped here, or the next page would repeat stations. */
		B.job.offset = B.next_offset;
		B.job.gen = B.gen;
		memcpy(B.job.query, B.info.query, sizeof(B.job.query));
		B.info.state = RB_LOADING;
	}
	LightLock_Unlock(&B.lock);
	if (go)
		LightEvent_Signal(&B.wake);
}

void rb_click(const char *uuid)
{
	if (!B.thread)
		return;
	LightLock_Lock(&B.lock);
	snprintf(B.click, sizeof(B.click), "%s", uuid);
	LightLock_Unlock(&B.lock);
	LightEvent_Signal(&B.wake);
}

void rb_info_get(rb_info *out)
{
	LightLock_Lock(&B.lock);
	*out = B.info;
	LightLock_Unlock(&B.lock);
}

bool rb_station_get(int index, rb_station *out)
{
	LightLock_Lock(&B.lock);
	bool ok = index >= 0 && index < B.info.count;
	if (ok)
		*out = B.list[index];
	LightLock_Unlock(&B.lock);
	return ok;
}
