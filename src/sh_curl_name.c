/* URL checks and untrusted file names for curl; see include/sh_curl_name.h. */
#include "sh_curl_name.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int host_char_ok(unsigned char c)
{
    return isalnum(c) || c == '.' || c == '-' || c == '_';
}

/* Length of the scheme prefix ("http://" or "https://"), or 0. */
static size_t scheme_len(const char *url)
{
    if (strncasecmp(url, "http://", 7) == 0)
        return 7;
    if (strncasecmp(url, "https://", 8) == 0)
        return 8;
    return 0;
}

int sh_curl_url_check(const char *url, const char **why)
{
    const char *p, *auth, *end, *colon;
    size_t n = strlen(url);

    if (n > SH_CURL_URL_MAX) {
        *why = "URL is too long";
        return -1;
    }
    for (p = url; *p; p++) {
        if ((unsigned char)*p <= ' ' || *p == 0x7f) {
            *why = "URL contains a space or control character";
            return -1;
        }
    }
    auth = url + scheme_len(url);
    if (auth == url) {
        *why = strstr(url, "://") ? "scheme is not http or https" : "missing scheme (use http:// or https://)";
        return -1;
    }
    end = auth + strcspn(auth, "/?#");
    if (end == auth) {
        *why = "missing host";
        return -1;
    }
    if (memchr(auth, '@', (size_t)(end - auth))) {
        *why = "credentials in the URL are not supported";
        return -1;
    }
    if (*auth == '[') {
        *why = "IPv6 hosts are not supported";
        return -1;
    }
    colon = memchr(auth, ':', (size_t)(end - auth));
    if (colon) {
        long port = 0;
        if (colon + 1 == end) {
            *why = "bad port";
            return -1;
        }
        for (p = colon + 1; p < end; p++) {
            if (!isdigit((unsigned char)*p) || (port = port * 10 + (*p - '0')) > 65535) {
                *why = "bad port";
                return -1;
            }
        }
        if (port == 0) {
            *why = "bad port";
            return -1;
        }
        if (colon == auth) {
            *why = "missing host";
            return -1;
        }
        end = colon;
    }
    for (p = auth; p < end; p++) {
        if (!host_char_ok((unsigned char)*p)) {
            *why = "bad character in host";
            return -1;
        }
    }
    return 0;
}

int sh_curl_url_host(const char *url, char *out, size_t cap)
{
    const char *auth = url + scheme_len(url);
    size_t n = strcspn(auth, "/?#:");

    if (n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, auth, n);
    out[n] = '\0';
    return 0;
}

static int fat_illegal(unsigned char c)
{
    return c < 0x20 || c == 0x7f || strchr(":\"*?<>|", c) != NULL;
}

int sh_curl_name_clean(const char *raw, char *out, size_t cap)
{
    const char *name = raw, *p;
    size_t n;

    for (p = raw; *p; p++) {
        if (*p == '/' || *p == '\\')
            name = p + 1;
    }
    n = strlen(name);
    if (n == 0 || strstr(name, "..") || name[n - 1] == '.' || name[n - 1] == ' ') {
        errno = EINVAL;
        return -1;
    }
    for (p = name; *p; p++) {
        if (fat_illegal((unsigned char)*p)) {
            errno = EINVAL;
            return -1;
        }
    }
    if (n > SH_CURL_NAME_MAX || n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, name, n + 1);
    return 0;
}

int sh_curl_url_basename(const char *url, char *out, size_t cap)
{
    const char *auth = url + scheme_len(url);
    const char *path = auth + strcspn(auth, "/?#");
    size_t plen = strcspn(path, "?#");
    const char *seg = path, *p;
    char buf[SH_CURL_URL_MAX + 1];

    for (p = path; p < path + plen; p++) {
        if (*p == '/')
            seg = p + 1;
    }
    plen -= (size_t)(seg - path);
    if (plen >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, seg, plen);
    buf[plen] = '\0';
    return sh_curl_name_clean(buf, out, cap);
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Finds parameter `key` (with its '=') in a header value; returns a pointer past the '=' or NULL. */
static const char *find_param(const char *value, const char *key)
{
    size_t klen = strlen(key);
    const char *p = value;

    while (*p) {
        while (*p == ';' || *p == ' ' || *p == '\t')
            p++;
        if (strncasecmp(p, key, klen) == 0)
            return p + klen;
        /* skip this parameter, honoring a quoted value that may hold ';' */
        while (*p && *p != ';') {
            if (*p == '"') {
                for (p++; *p && *p != '"'; p++) {
                    if (*p == '\\' && p[1])
                        p++;
                }
            }
            if (*p)
                p++;
        }
    }
    return NULL;
}

int sh_curl_disposition_name(const char *value, char *out, size_t cap)
{
    const char *p;
    char raw[SH_CURL_NAME_MAX * 3 + 2];
    size_t n = 0;

    if ((p = find_param(value, "filename*="))) {
        /* charset'language'percent-encoded-name */
        const char *q = strchr(p, '\'');
        q = q ? strchr(q + 1, '\'') : NULL;
        if (!q) {
            errno = EINVAL;
            return -1;
        }
        for (q++; *q && *q != ';' && *q != ' ' && *q != '\t'; q++) {
            int hi, lo, c = (unsigned char)*q;
            if (c == '%') {
                hi = hex_val((unsigned char)q[1]);
                lo = hi < 0 ? -1 : hex_val((unsigned char)q[2]);
                if (lo < 0) {
                    errno = EINVAL;
                    return -1;
                }
                c = hi * 16 + lo;
                q += 2;
            }
            if (n + 1 >= sizeof(raw) || c == 0) {
                errno = c == 0 ? EINVAL : ENAMETOOLONG;
                return -1;
            }
            raw[n++] = (char)c;
        }
    } else if ((p = find_param(value, "filename="))) {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1])
                    p++;
                if (n + 1 >= sizeof(raw)) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                raw[n++] = *p;
            }
        } else {
            for (; *p && *p != ';' && *p != ' ' && *p != '\t'; p++) {
                if (n + 1 >= sizeof(raw)) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                raw[n++] = *p;
            }
        }
    } else {
        return 1;
    }
    raw[n] = '\0';
    return sh_curl_name_clean(raw, out, cap);
}
