#include "radio3ds.h"
#include "http3ds.h"
#include "http_logic.h"
#include "tls3ds.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

/* --------------------------------------------------------------- tuning */

/* Compressed audio between the network and the decoder: ~12 s at 128 kbps.
 * It rides out Wi-Fi hiccups and the seconds a reconnect takes. */
#define RING_SIZE        (192 * 1024)
/* How much to gather before (re)starting output, in seconds of audio at the
 * stream's announced bitrate. More = fewer dropouts, slower start. */
#define PREBUFFER_S      2.5f
#define PREBUFFER_MIN    (16 * 1024)
/* DSP queue: WB_COUNT buffers of WB_FRAMES stereo frames each. 4608 is four
 * MPEG-1 frames (or eight MPEG-2 ones); 6 x 4608 at 48 kHz is ~0.6 s. */
#define WB_FRAMES        4608
#define WB_COUNT         6
#define CHANNEL          0
/* Decoder input window; a frame is at most 1441 bytes (320 kbps, 32 kHz). */
#define INBUF            8192
/* Consecutive failed connections before giving up (data received resets). */
#define MAX_RETRIES      6
#define NET_STACK        (64 * 1024)   /* TLS handshake: see main.c */
#define DEC_STACK        (32 * 1024)   /* minimp3 keeps ~16 KB of scratch */

/* ---------------------------------------------------------------- state */

static struct {
	bool         ready;           /* ndspInit() succeeded, buffers exist */
	LightLock    lock;            /* everything below except the threads */
	radio_status st;
	volatile bool stop;
	bool         net_done;        /* the producer finished for good */
	bool         net_running, dec_running;
	Thread       net_thread, dec_thread;
	char         url[512];
	char         pending[512];    /* radio_play() while the last one ends */
	bool         has_pending;
	size_t       prebuffer;       /* bytes, set from icy-br */

	u8          *ring;
	size_t       ring_head;       /* next byte to write */
	size_t       ring_count;

	s16         *pcm;             /* linear memory, WB_COUNT * WB_FRAMES * 2 */
	ndspWaveBuf  wbuf[WB_COUNT];
} R;

/* Write while there is room; returns what fit. */
static size_t ring_write(const u8 *p, size_t n)
{
	LightLock_Lock(&R.lock);
	size_t room = RING_SIZE - R.ring_count;
	if (n > room)
		n = room;
	size_t first = RING_SIZE - R.ring_head;
	if (first > n)
		first = n;
	memcpy(R.ring + R.ring_head, p, first);
	memcpy(R.ring, p + first, n - first);
	R.ring_head = (R.ring_head + n) % RING_SIZE;
	R.ring_count += n;
	LightLock_Unlock(&R.lock);
	return n;
}

static size_t ring_read(u8 *out, size_t max)
{
	LightLock_Lock(&R.lock);
	size_t n = R.ring_count < max ? R.ring_count : max;
	size_t tail = (R.ring_head + RING_SIZE - R.ring_count) % RING_SIZE;
	size_t first = RING_SIZE - tail;
	if (first > n)
		first = n;
	memcpy(out, R.ring + tail, first);
	memcpy(out + first, R.ring, n - first);
	R.ring_count -= n;
	LightLock_Unlock(&R.lock);
	return n;
}

/* FAILED sticks until the next radio_play(), whatever the threads report
 * while they wind down. Call with the lock held. */
static void set_state_locked(radio_state s)
{
	if (R.st.state != RADIO_FAILED)
		R.st.state = s;
}

static void set_state(radio_state s)
{
	LightLock_Lock(&R.lock);
	set_state_locked(s);
	LightLock_Unlock(&R.lock);
}

/* ------------------------------------------------------------ text helpers */

/* A header's value, anchored to a line start, case-insensitive. */
static bool header_value(const char *headers, const char *name,
                         char *out, size_t outlen)
{
	size_t n = strlen(name);
	for (const char *p = headers; *p; p++) {
		if ((p != headers && p[-1] != '\n') || strncasecmp(p, name, n) ||
		    p[n] != ':')
			continue;
		p += n + 1;
		while (*p == ' ' || *p == '\t')
			p++;
		size_t len = strcspn(p, "\r\n");
		while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
			len--;
		if (len >= outlen)
			len = outlen - 1;
		memcpy(out, p, len);
		out[len] = '\0';
		return true;
	}
	return false;
}

/* newlib hides strcasestr behind _GNU_SOURCE; `needle` is lowercase. */
static bool contains_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp(hay, needle, n) == 0)
			return true;
	return false;
}

static bool utf8_valid(const unsigned char *s)
{
	while (*s) {
		int extra = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 :
		            (*s & 0xF0) == 0xE0 ? 2 : (*s & 0xF8) == 0xF0 ? 3 : -1;
		if (extra < 0)
			return false;
		s++;
		for (; extra; extra--, s++)
			if ((*s & 0xC0) != 0x80)
				return false;
	}
	return true;
}

/* Stations send titles as UTF-8 or, quite often, Latin-1: the system font
 * wants UTF-8, so convert when it is not valid UTF-8 already. */
static void to_utf8(const char *in, size_t len, char *out, size_t outlen)
{
	char raw[256];
	if (len >= sizeof(raw))
		len = sizeof(raw) - 1;
	memcpy(raw, in, len);
	raw[len] = '\0';
	size_t o = 0;
	if (utf8_valid((const unsigned char *)raw)) {
		/* Whole characters only, so a cut never splits one. */
		for (const char *p = raw; *p;) {
			size_t n = 1;
			while ((p[n] & 0xC0) == 0x80)
				n++;
			if (o + n >= outlen)
				break;
			memcpy(out + o, p, n);
			o += n;
			p += n;
		}
		out[o] = '\0';
		return;
	}
	for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
		if (*p < 0x80) {
			if (o + 1 >= outlen)
				break;
			out[o++] = (char)*p;
		} else {
			if (o + 2 >= outlen)
				break;
			out[o++] = (char)(0xC0 | (*p >> 6));
			out[o++] = (char)(0x80 | (*p & 0x3F));
		}
	}
	out[o] = '\0';
}

/* ------------------------------------------------------- network (thread) */

/* One connection's worth of ICY demultiplexing: every `metaint` bytes of
 * audio the server slips in one length byte (x16) and that much metadata. */
typedef struct {
	int    metaint;          /* 0: the server sends no metadata */
	int    until_meta;
	enum { ICY_AUDIO, ICY_LENGTH, ICY_META } part;
	int    meta_left;
	int    meta_have;
	char   meta[255 * 16 + 1];
	size_t audio_bytes;      /* this connection, to tell a working link */
	bool   fatal;            /* do not retry: wrong format, 4xx... */
	char   err[160];
} net_ctx;

static void icy_title(net_ctx *n)
{
	n->meta[n->meta_have] = '\0';
	const char *start = strstr(n->meta, "StreamTitle='");
	if (!start)
		return;
	start += 13;
	const char *end = strstr(start, "';");
	size_t len = end ? (size_t)(end - start) : strlen(start);
	char title[sizeof(R.st.title)];
	to_utf8(start, len, title, sizeof(title));
	LightLock_Lock(&R.lock);
	memcpy(R.st.title, title, sizeof(title));
	LightLock_Unlock(&R.lock);
}

/* Blocks while the ring is full: the TCP window then fills and the server
 * waits for us, which is how a live stream is paced. */
static bool push_audio(net_ctx *n, const char *data, size_t len)
{
	while (len) {
		if (R.stop)
			return false;
		size_t put = ring_write((const u8 *)data, len);
		if (!put) {
			svcSleepThread(20 * 1000000LL);
			continue;
		}
		n->audio_bytes += put;
		data += put;
		len -= put;
	}
	return true;
}

static bool on_headers(void *ctx, int status, const char *headers)
{
	net_ctx *n = ctx;
	(void)status;
	char value[96];

	if (header_value(headers, "content-type", value, sizeof(value))) {
		bool mp3 = contains_ci(value, "mpeg") || contains_ci(value, "mp3") ||
		           contains_ci(value, "octet-stream");
		if (!mp3) {
			snprintf(n->err, sizeof(n->err),
			         "formato no soportado: %s (solo MP3)", value);
			n->fatal = true;
			return false;
		}
	}
	n->metaint = header_value(headers, "icy-metaint", value, sizeof(value))
	             ? atoi(value) : 0;
	if (n->metaint < 0)
		n->metaint = 0;
	n->until_meta = n->metaint;
	n->part = ICY_AUDIO;

	int kbps = header_value(headers, "icy-br", value, sizeof(value))
	           ? atoi(value) : 0;
	if (kbps <= 0 || kbps > 320)
		kbps = 128;
	size_t pre = (size_t)(kbps * 1000 / 8 * PREBUFFER_S);

	char station[sizeof(R.st.station)] = "";
	/* "no name" is Icecast's default when the source sets none. */
	if (header_value(headers, "icy-name", value, sizeof(value)) &&
	    strcasecmp(value, "no name") != 0)
		to_utf8(value, strlen(value), station, sizeof(station));

	LightLock_Lock(&R.lock);
	R.prebuffer = pre < PREBUFFER_MIN ? PREBUFFER_MIN : pre;
	if (station[0])
		memcpy(R.st.station, station, sizeof(station));
	R.st.reconnecting = false;
	R.st.error[0] = '\0';
	if (R.st.state == RADIO_CONNECTING)
		set_state_locked(RADIO_BUFFERING);
	LightLock_Unlock(&R.lock);
	return true;
}

static bool on_body(void *ctx, const char *data, size_t len)
{
	net_ctx *n = ctx;
	if (!n->metaint)
		return push_audio(n, data, len);
	while (len) {
		switch (n->part) {
		case ICY_AUDIO: {
			size_t take = len < (size_t)n->until_meta ? len
			                                          : (size_t)n->until_meta;
			if (!push_audio(n, data, take))
				return false;
			data += take;
			len -= take;
			n->until_meta -= (int)take;
			if (!n->until_meta)
				n->part = ICY_LENGTH;
			break;
		}
		case ICY_LENGTH:
			n->meta_left = (unsigned char)*data * 16;
			n->meta_have = 0;
			data++;
			len--;
			n->until_meta = n->metaint;
			n->part = n->meta_left ? ICY_META : ICY_AUDIO;
			break;
		case ICY_META: {
			size_t take = len < (size_t)n->meta_left ? len
			                                         : (size_t)n->meta_left;
			memcpy(n->meta + n->meta_have, data, take);
			n->meta_have += (int)take;
			n->meta_left -= (int)take;
			data += take;
			len -= take;
			if (!n->meta_left) {
				icy_title(n);
				n->part = ICY_AUDIO;
			}
			break;
		}
		}
	}
	return true;
}

static void net_worker(void *arg)
{
	(void)arg;
	/* The ICY state is ~4 KB: off the stack, which TLS needs. */
	net_ctx *n = malloc(sizeof(*n));
	int failures = 0;
	bool ever_played = false;
	char fail_reason[160] = "sin memoria";

	while (n && !R.stop) {
		memset(n, 0, sizeof(*n));
		char err[160] = "";
		int status = 0;
		/* Radios stream over http:// as often as https://, and their
		 * certificates come from every CA there is (or chains with
		 * missing pieces). The audio is public and plain http is taken
		 * anyway, so a chain that does not verify is no reason to refuse.
		 * The API calls that matter stay verified. */
		int rc = http_get_url_stream_ex(
			R.url, 0, HTTP_STREAM_ALLOW_HTTP | HTTP_STREAM_NO_VERIFY,
			"Icy-MetaData: 1\r\nAccept: audio/mpeg, audio/*;q=0.8\r\n",
			on_headers, on_body, n, NULL, &status, err, sizeof(err));
		if (R.stop)
			break;
		if (n->audio_bytes)
			ever_played = true;

		if (n->err[0])
			snprintf(err, sizeof(err), "%s", n->err);
		else if (rc == 0)
			snprintf(err, sizeof(err), "el servidor cerró el stream");
		/* A client error will not fix itself by asking again. */
		if (n->fatal || (status >= 400 && status < 500)) {
			snprintf(fail_reason, sizeof(fail_reason), "%s", err);
			goto failed;
		}
		/* A station that never played gets one retry, not six: in a
		 * browser of thousands, many entries are simply dead. */
		failures = n->audio_bytes ? 1 : failures + 1;
		if (failures > (ever_played ? MAX_RETRIES : 1)) {
			snprintf(fail_reason, sizeof(fail_reason), "%s", err);
			goto failed;
		}

		LightLock_Lock(&R.lock);
		R.st.reconnecting = true;
		R.st.reconnects++;
		snprintf(R.st.error, sizeof(R.st.error), "%s", err);
		LightLock_Unlock(&R.lock);

		/* Back off in slices, so a stop is noticed quickly. */
		for (unsigned waited = 0, delay = http_retry_delay_ms(failures);
		     waited < delay && !R.stop; waited += 100)
			svcSleepThread(100 * 1000000LL);
	}
	if (!n && !R.stop)
		goto failed;
	goto out;

failed:
	LightLock_Lock(&R.lock);
	R.st.reconnecting = false;
	snprintf(R.st.error, sizeof(R.st.error), "%s", fail_reason);
	R.st.state = RADIO_FAILED;
	LightLock_Unlock(&R.lock);
out:
	free(n);
	LightLock_Lock(&R.lock);
	R.net_done = true;
	R.net_running = false;
	LightLock_Unlock(&R.lock);
}

/* ------------------------------------------------------- decoder (thread) */

static bool output_drained(void)
{
	for (int i = 0; i < WB_COUNT; i++)
		if (R.wbuf[i].status == NDSP_WBUF_QUEUED ||
		    R.wbuf[i].status == NDSP_WBUF_PLAYING)
			return false;
	return true;
}

static void dec_worker(void *arg)
{
	(void)arg;
	static mp3dec_t dec;
	static u8 in[INBUF];
	static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	static s16 stereo[MINIMP3_MAX_SAMPLES_PER_FRAME];   /* 1152 frames x 2 */
	size_t in_pos = 0, in_len = 0;
	int wb = 0;                 /* the buffer being filled */
	size_t wb_fill = 0;         /* frames in it so far */
	int rate = 0;
	bool buffering = true, started = false;

	mp3dec_init(&dec);
	for (int i = 0; i < WB_COUNT; i++) {
		memset(&R.wbuf[i], 0, sizeof(R.wbuf[i]));
		R.wbuf[i].data_vaddr = R.pcm + (size_t)i * WB_FRAMES * 2;
		R.wbuf[i].status = NDSP_WBUF_FREE;
	}

	while (!R.stop) {
		if (buffering) {
			LightLock_Lock(&R.lock);
			size_t have = R.ring_count, want = R.prebuffer;
			bool done = R.net_done;
			LightLock_Unlock(&R.lock);
			if (done && !have && in_pos == in_len)
				break;                     /* nothing more will come */
			if (!done && have < want) {
				svcSleepThread(20 * 1000000LL);
				continue;
			}
			buffering = false;
		}

		/* Keep the window topped up; compact once half of it is spent. */
		if (in_pos > INBUF / 2) {
			memmove(in, in + in_pos, in_len - in_pos);
			in_len -= in_pos;
			in_pos = 0;
		}
		if (in_len < INBUF)
			in_len += ring_read(in + in_len, INBUF - in_len);

		mp3dec_frame_info_t info = {0};
		int samples = 0;
		if (in_len > in_pos)
			samples = mp3dec_decode_frame(&dec, in + in_pos,
			                              (int)(in_len - in_pos), pcm, &info);
		if (!info.frame_bytes) {
			/* No whole frame. A full window (at least INBUF/2 unread
			 * bytes, several frames' worth) without one is not MP3: drop
			 * it and resync. Otherwise wait for more bytes. */
			if (in_len == INBUF) {
				in_pos = in_len = 0;
				continue;
			}
			LightLock_Lock(&R.lock);
			bool more = R.ring_count > 0, done = R.net_done;
			LightLock_Unlock(&R.lock);
			if (more)
				continue;
			if (!output_drained()) {
				svcSleepThread(10 * 1000000LL);
				continue;
			}
			if (done)
				break;
			/* Ran dry: rebuffer before starting again, rather than
			 * stuttering frame by frame. */
			LightLock_Lock(&R.lock);
			if (started)
				R.st.underruns++;
			set_state_locked(RADIO_BUFFERING);
			LightLock_Unlock(&R.lock);
			buffering = true;
			continue;
		}
		in_pos += info.frame_bytes;
		if (!samples)
			continue;              /* an ID3 tag or a skipped frame */

		if (info.hz != rate) {
			rate = info.hz;
			ndspChnSetRate(CHANNEL, (float)rate);
		}
		LightLock_Lock(&R.lock);
		R.st.bitrate_kbps = info.bitrate_kbps;
		R.st.sample_rate = info.hz;
		R.st.channels = info.channels;
		LightLock_Unlock(&R.lock);

		/* The channel is always stereo: duplicate mono. */
		const s16 *src = pcm;
		if (info.channels == 1) {
			for (int i = 0; i < samples; i++)
				stereo[2 * i] = stereo[2 * i + 1] = pcm[i];
			src = stereo;
		}

		size_t left = (size_t)samples;
		while (left && !R.stop) {
			ndspWaveBuf *b = &R.wbuf[wb];
			if (wb_fill == 0 && (b->status == NDSP_WBUF_QUEUED ||
			                     b->status == NDSP_WBUF_PLAYING)) {
				/* Every buffer is queued: the DSP is ahead of us. */
				svcSleepThread(5 * 1000000LL);
				continue;
			}
			size_t take = WB_FRAMES - wb_fill;
			if (take > left)
				take = left;
			memcpy(b->data_pcm16 + wb_fill * 2, src, take * 2 * sizeof(s16));
			src += take * 2;
			left -= take;
			wb_fill += take;
			if (wb_fill == WB_FRAMES) {
				b->nsamples = WB_FRAMES;
				DSP_FlushDataCache(b->data_pcm16, WB_FRAMES * 2 * sizeof(s16));
				ndspChnWaveBufAdd(CHANNEL, b);
				wb = (wb + 1) % WB_COUNT;
				wb_fill = 0;
				if (!started)
					started = true;
				set_state(RADIO_PLAYING);
			}
		}
	}

	ndspChnWaveBufClear(CHANNEL);
	LightLock_Lock(&R.lock);
	set_state_locked(RADIO_STOPPED);
	R.dec_running = false;
	LightLock_Unlock(&R.lock);
}

/* ------------------------------------------------------------- public API */

static void apply_volume(float v)
{
	float mix[12] = {0};
	mix[0] = mix[1] = v;
	ndspChnSetMix(CHANNEL, mix);
}

bool radio_init(char *err, size_t errlen)
{
	LightLock_Init(&R.lock);
	R.st.volume = 0.8f;
	if (R.ready)
		return true;
	if (R_FAILED(ndspInit())) {
		snprintf(err, errlen, "ndspInit() falló: falta sdmc:/3ds/dspfirm.cdc "
		         "(vuélcalo una vez con la app DSP1)");
		return false;
	}
	R.ring = malloc(RING_SIZE);
	R.pcm = linearAlloc((size_t)WB_COUNT * WB_FRAMES * 2 * sizeof(s16));
	if (!R.ring || !R.pcm) {
		free(R.ring);
		if (R.pcm)
			linearFree(R.pcm);
		R.ring = NULL;
		R.pcm = NULL;
		ndspExit();
		snprintf(err, errlen, "sin memoria para los búferes de audio");
		return false;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(CHANNEL);
	ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetRate(CHANNEL, 44100.0f);
	apply_volume(R.st.volume);
	R.ready = true;
	return true;
}

static void reap(bool wait)
{
	if (!R.net_thread && !R.dec_thread)
		return;
	LightLock_Lock(&R.lock);
	bool running = R.net_running || R.dec_running;
	LightLock_Unlock(&R.lock);
	if (running && !wait)
		return;
	if (R.net_thread) {
		threadJoin(R.net_thread, U64_MAX);
		threadFree(R.net_thread);
		R.net_thread = NULL;
	}
	if (R.dec_thread) {
		threadJoin(R.dec_thread, U64_MAX);
		threadFree(R.dec_thread);
		R.dec_thread = NULL;
	}
}

static void start_session(const char *url)
{
	LightLock_Lock(&R.lock);
	snprintf(R.url, sizeof(R.url), "%s", url);
	R.stop = false;
	R.net_done = false;
	R.ring_head = R.ring_count = 0;
	R.prebuffer = PREBUFFER_MIN;
	float volume = R.st.volume;
	memset(&R.st, 0, sizeof(R.st));
	R.st.volume = volume;
	R.st.state = RADIO_CONNECTING;
	R.net_running = R.dec_running = true;
	LightLock_Unlock(&R.lock);

	/* Decoder one step above the main thread (lower number = higher
	 * priority): a late frame is an audible click, a late UI frame is not.
	 * The network one step below, as in main.c. */
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	R.dec_thread = threadCreate(dec_worker, NULL, DEC_STACK,
	                            prio > 0x18 ? prio - 1 : 0x18, -2, false);
	R.net_thread = R.dec_thread
		? threadCreate(net_worker, NULL, NET_STACK,
		               prio < 0x3F ? prio + 1 : 0x3F, -2, false)
		: NULL;
	if (!R.net_thread) {
		LightLock_Lock(&R.lock);
		R.net_running = false;
		R.net_done = true;
		if (!R.dec_thread)
			R.dec_running = false;
		R.st.state = RADIO_FAILED;
		snprintf(R.st.error, sizeof(R.st.error), "no se pudo crear el hilo");
		LightLock_Unlock(&R.lock);
		R.stop = true;
	}
}

void radio_play(const char *url)
{
	if (!R.ready)
		return;
	snprintf(R.pending, sizeof(R.pending), "%s", url);
	R.has_pending = true;
	radio_stop();
	radio_update();
}

void radio_stop(void)
{
	R.stop = true;
}

bool radio_active(void)
{
	if (R.has_pending)
		return true;
	LightLock_Lock(&R.lock);
	bool active = !R.stop && R.st.state != RADIO_STOPPED &&
	              R.st.state != RADIO_FAILED;
	LightLock_Unlock(&R.lock);
	return active;
}

void radio_update(void)
{
	if (!R.ready)
		return;
	reap(false);
	if (R.has_pending && !R.net_thread && !R.dec_thread) {
		R.has_pending = false;
		start_session(R.pending);
	}
}

void radio_exit(void)
{
	if (!R.ready)
		return;
	R.has_pending = false;
	radio_stop();
	reap(true);
	ndspChnWaveBufClear(CHANNEL);
	ndspExit();
	free(R.ring);
	linearFree(R.pcm);
	R.ring = NULL;
	R.pcm = NULL;
	R.ready = false;
}

void radio_set_volume(float volume)
{
	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;
	LightLock_Lock(&R.lock);
	R.st.volume = volume;
	LightLock_Unlock(&R.lock);
	if (R.ready)
		apply_volume(volume);
}

void radio_status_get(radio_status *out)
{
	LightLock_Lock(&R.lock);
	*out = R.st;
	size_t bytes = R.ring_count;
	LightLock_Unlock(&R.lock);
	int kbps = out->bitrate_kbps > 0 ? out->bitrate_kbps : 128;
	out->buffered_s = (float)bytes * 8.0f / (kbps * 1000.0f);
	out->fill = (float)bytes / RING_SIZE;
}
