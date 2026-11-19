#ifndef HTTP_LOGIC_H
#define HTTP_LOGIC_H

/* Pure HTTP helpers: no console headers, so they can be unit-tested on the
 * host PC with a plain C compiler. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* How many Location hops one streaming request follows before giving up. */
#define HTTP_MAX_REDIRECTS 4

static inline bool http_status_is_redirect(int status)
{
	return status == 301 || status == 302 || status == 303 ||
	       status == 307 || status == 308;
}

/* Exponential backoff for retries: 1 s, 2 s, 4 s, then 8 s each. */
static inline unsigned http_retry_delay_ms(int failures)
{
	if (failures <= 1)
		return 1000;
	if (failures >= 4)
		return 8000;
	return 1000u << (failures - 1);
}

/* Splits an authority ("host" or "host:port") into the host name and the
 * port, `default_port` when none is given. False for an empty host, a host
 * that does not fit, or a port outside 1-65535. IPv6 literals are not
 * handled. */
static inline bool http_split_authority(const char *authority, size_t len,
                                        unsigned default_port,
                                        char *host, size_t hostlen,
                                        unsigned *port)
{
	if (!authority || !host || !hostlen || !port)
		return false;
	const char *colon = memchr(authority, ':', len);
	size_t hlen = colon ? (size_t)(colon - authority) : len;
	if (hlen == 0 || hlen >= hostlen)
		return false;
	*port = default_port;
	if (colon) {
		const char *p = colon + 1, *end = authority + len;
		if (p == end)
			return false;
		unsigned value = 0;
		for (; p < end; p++) {
			if (*p < '0' || *p > '9')
				return false;
			value = value * 10 + (unsigned)(*p - '0');
			if (value > 65535)
				return false;
		}
		if (value == 0)
			return false;
		*port = value;
	}
	memcpy(host, authority, hlen);
	host[hlen] = '\0';
	return true;
}

/* Length of the scheme prefix of `url`: 8 for https://, 7 for http:// when
 * `allow_plain`, 0 for anything else. */
static inline size_t http_scheme_len(const char *url, bool allow_plain)
{
	if (strncmp(url, "https://", 8) == 0)
		return 8;
	if (allow_plain && strncmp(url, "http://", 7) == 0)
		return 7;
	return 0;
}

/* Resolves a Location header value against the URL that answered it, into
 * an absolute URL. `location` may run on to the end of its header line.
 * http:// (either side) only when `allow_plain`. */
static inline bool http_redirect_url(const char *from_url,
                                     const char *location, bool allow_plain,
                                     char *out, size_t outlen)
{
	if (!from_url || !location || !out || !outlen)
		return false;
	while (*location == ' ' || *location == '\t')
		location++;
	size_t loc_len = strcspn(location, "\r\n");
	while (loc_len && (location[loc_len - 1] == ' ' ||
	                   location[loc_len - 1] == '\t'))
		loc_len--;
	if (!loc_len)
		return false;

	const char *prefix;
	size_t prefix_len;
	size_t from_scheme = http_scheme_len(from_url, allow_plain);
	size_t loc_scheme = http_scheme_len(location, allow_plain);
	if (loc_scheme && loc_len > loc_scheme) {
		prefix = "";
		prefix_len = 0;
	} else if (loc_len > 2 && location[0] == '/' && location[1] == '/') {
		/* Scheme-relative: keep the scheme of the URL that answered. */
		if (!from_scheme)
			return false;
		prefix = from_url;
		prefix_len = from_scheme - 2;
	} else if (location[0] == '/') {
		if (!from_scheme)
			return false;
		const char *slash = strchr(from_url + from_scheme, '/');
		prefix = from_url;
		prefix_len = slash ? (size_t)(slash - from_url) : strlen(from_url);
		if (prefix_len <= from_scheme)
			return false;
	} else {
		return false;
	}

	if (prefix_len + loc_len >= outlen)
		return false;
	memcpy(out, prefix, prefix_len);
	memcpy(out + prefix_len, location, loc_len);
	out[prefix_len + loc_len] = '\0';
	return true;
}

#endif
