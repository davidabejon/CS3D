/* Internet radio: MP3 over HTTP(S) (Icecast / SHOUTcast) to the DSP.
 *
 *     network thread --> ring buffer --> decoder thread --> NDSP channel
 *       (http3ds)        (MP3 bytes)      (minimp3)          (PCM16 stereo)
 *
 * The network thread reads the endless HTTP body, strips the ICY metadata
 * blocks (the "now playing" title) and fills a ring of compressed audio. The
 * decoder waits until a few seconds are buffered, then decodes frame by frame
 * into a queue of DSP wave buffers. If the connection drops it reconnects with
 * backoff while what is buffered keeps playing.
 *
 * Needs:
 *   - tls_global_init() done (and socInit/psInit before it);
 *   - sdmc:/3ds/dspfirm.cdc, the DSP firmware, dumped once from the console
 *     with "DSP1". Without it ndspInit() fails and radio_init() says so.
 *
 * Every function here is for the main thread. MP3 only: AAC/Ogg streams are
 * refused with a readable error.
 */
#ifndef RADIO3DS_H
#define RADIO3DS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	RADIO_STOPPED,
	RADIO_CONNECTING,     /* first connection, nothing received yet */
	RADIO_BUFFERING,      /* filling the ring before (re)starting output */
	RADIO_PLAYING,
	RADIO_FAILED          /* gave up; see `error` */
} radio_state;

typedef struct {
	radio_state state;
	bool     reconnecting;     /* the link dropped; retrying in the background */
	unsigned reconnects;       /* how many times it had to reconnect */
	unsigned underruns;        /* times playback ran dry and rebuffered */
	char     station[64];      /* icy-name, if the server sends it */
	char     title[128];       /* StreamTitle from the ICY metadata, UTF-8 */
	int      bitrate_kbps;     /* of the last decoded frame */
	int      sample_rate;
	int      channels;
	float    buffered_s;       /* compressed audio waiting in the ring */
	float    fill;             /* ring occupancy, 0..1 */
	float    volume;           /* 0..1 */
	char     error[160];       /* last network error, or why it failed */
} radio_status;

/* Starts the DSP. False with `err` set if it could not (usually a missing
 * dspfirm.cdc). The other calls are harmless no-ops if this failed. */
bool radio_init(char *err, size_t errlen);
/* Stops the stream and waits for its threads. Call tls_set_abort(true) first
 * so a connection in progress gives up within ~100 ms. Before tls_global_exit. */
void radio_exit(void);

/* Starts playing an http:// or https:// URL (a port is allowed). https is
 * not required to verify: the audio is public (see net_worker()).
 * If a previous stream is still shutting down, it starts once it is gone. */
void radio_play(const char *url);
/* Asks the stream to stop; returns at once. The threads finish on their own
 * and radio_update() reaps them. */
void radio_stop(void);
/* True from radio_play() until it stops or fails. */
bool radio_active(void);

/* Once per frame: reaps finished threads and starts a pending radio_play(). */
void radio_update(void);

void radio_set_volume(float volume);   /* clamped to 0..1 */
void radio_status_get(radio_status *out);

#endif
