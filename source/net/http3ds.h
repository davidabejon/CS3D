/* Minimal HTTP/1.1 client over tls3ds (HTTPS only).
 *
 * GET and POST, Content-Length and chunked bodies, redirects on the streaming
 * reader, and optional keep-alive sessions. Every call blocks: run them on a
 * worker thread, never on the thread that draws (see main.c).
 *
 * Hosts may carry a port ("example.com:8443", or https://example.com:8443/
 * in URLs); without one, 443.
 *
 * Calls on different connections may run on different threads at once (for
 * instance an endless audio stream next to ordinary requests): tls3ds
 * serializes handshakes and the shared random generator. One connection or
 * one http_session must still be used by a single thread at a time.
 */
#ifndef HTTP3DS_H
#define HTTP3DS_H

#include <stdbool.h>
#include <stddef.h>

/* Sent with every request unless `extra` sets its own User-Agent. Override
 * it from the build (-DHTTP_USER_AGENT=\"myapp/1.0\") or here. */
#ifndef HTTP_USER_AGENT
#define HTTP_USER_AGENT "3ds-homebrew/0.1"
#endif

typedef struct {
	int    status;        /* HTTP status, or 0 if the request never completed */
	char  *body;          /* NUL-terminated; caller frees */
	size_t len;
	int    retry_after;   /* seconds, from a 429's Retry-After; 0 otherwise */
	char   err[160];
} http_reply;

/* All of these return 0 when a response was read (whatever its status: a
 * 404 or a 500 is still a successful read, check out->status) and -1 when
 * not, with out->err set. On 0 the caller frees with http_reply_free(). */

/* GET https://host/path. `bearer` may be NULL. */
int http_get(const char *host, const char *path, const char *bearer,
             http_reply *out);

/* POST `body` as application/x-www-form-urlencoded (see url_encode()). */
int http_post_form(const char *host, const char *path, const char *body,
                   http_reply *out);

/* POST a JSON document, asking for JSON back. `bearer` may be NULL. */
int http_post_json(const char *host, const char *path, const char *bearer,
                   const char *json, http_reply *out);

/* GET a full https:// URL; the query is passed through untouched. `limit`
 * caps the whole response including headers (0 = 512 KB). */
int http_get_url(const char *url, size_t limit, http_reply *out);

/* The general form the ones above are built from. `bearer`, `ctype` and
 * `accept` may be NULL. `limit` caps the complete response, including
 * headers; 0 uses the default (512 KB), sized for API replies. */
int http_request(const char *host, const char *path, const char *method,
                 const char *bearer, const char *ctype, const char *accept,
                 const void *body, size_t len, size_t limit, http_reply *out);

/* The same with `extra` header lines, each ending in "\r\n" (NULL: none).
 * A User-Agent among them replaces the default one. */
int http_request_ex(const char *host, const char *path, const char *method,
                    const char *bearer, const char *ctype, const char *accept,
                    const char *extra, const void *body, size_t len,
                    size_t limit, http_reply *out);

/* Streaming GET for large files: instead of buffering the whole response,
 * every piece of body goes to `cb` as it arrives; returning false from the
 * callback cancels the download.
 *
 * `offset` resumes from that byte (expects a 206); 0 asks for the whole file
 * (a 200). The file's total size goes to `total_out` before the first
 * callback, so the caller can reserve its buffer. Follows up to
 * HTTP_MAX_REDIRECTS redirects (https only). The body is read until the
 * server closes: a chunked body is not handled here, but an endless one
 * without Content-Length (live radio, *total_out = 0) is. Returns 0 or -1. */
int http_get_url_stream(const char *url, size_t offset,
                        bool (*cb)(void *ctx, const char *data, size_t n),
                        void *ctx, size_t *total_out, int *status_out,
                        char *err, size_t errlen);

/* Flags for http_get_url_stream_ex(). */
enum {
	/* Also http:// URLs (and redirects to them), over plain TCP. */
	HTTP_STREAM_ALLOW_HTTP = 1 << 0,
	/* https:// without requiring a verified chain (tls3ds TLS_NO_VERIFY).
	 * Only for public content, such as a radio's audio. */
	HTTP_STREAM_NO_VERIFY  = 1 << 1,
};

/* The same with `flags`, `extra` header lines, each ending in "\r\n" (NULL:
 * none), and `on_headers`, called once with the final 200/206 response's
 * status and header block before any body; returning false from it cancels.
 * Either may be NULL. Also accepts SHOUTcast's "ICY 200 OK" status line. */
int http_get_url_stream_ex(const char *url, size_t offset, unsigned flags,
                           const char *extra,
                           bool (*on_headers)(void *ctx, int status,
                                              const char *headers),
                           bool (*cb)(void *ctx, const char *data, size_t n),
                           void *ctx, size_t *total_out, int *status_out,
                           char *err, size_t errlen);

void http_reply_free(http_reply *r);

/* Keep-alive session: one TLS connection, many sequential requests to the
 * same host. A full handshake costs seconds on this console; over a session
 * only the first request pays it. A request that finds the server dropped an
 * idle connection reconnects and retries once, transparently. */
typedef struct http_session http_session;
http_session *http_session_open(const char *host);
int  http_session_request(http_session *s, const char *path,
                          const char *method, const char *bearer,
                          const char *ctype, const char *accept,
                          const void *body, size_t len, size_t limit,
                          http_reply *out);
/* With `extra` header lines, as http_request_ex. */
int  http_session_request_ex(http_session *s, const char *path,
                             const char *method, const char *bearer,
                             const char *ctype, const char *accept,
                             const char *extra, const void *body, size_t len,
                             size_t limit, http_reply *out);
void http_session_close(http_session *s);
/* 1 when the last request reused the open link, 0 when it reconnected. */
int  http_session_last_reused(const http_session *s);

/* Percent-encodes everything outside the unreserved set, for form bodies and
 * query strings. Returns the encoded length. */
size_t url_encode(const char *in, char *out, size_t outlen);

#endif
