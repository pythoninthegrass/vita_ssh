/* URL checks and untrusted file names for curl: no I/O and no Vita APIs, so host tests run this code. */
#ifndef SH_CURL_NAME_H
#define SH_CURL_NAME_H

#include <stddef.h>

/* Longest URL curl accepts, and longest file name it will create. */
#define SH_CURL_URL_MAX 2048
#define SH_CURL_NAME_MAX 255

/* Accepts only http:// and https:// URLs with a host and an optional port; returns 0, or -1 with
 * *why set to the rejected part in words (a string literal). */
int sh_curl_url_check(const char *url, const char **why);

/* Copies the host of a URL that passed sh_curl_url_check, without port; returns 0 or -1 (ENAMETOOLONG). */
int sh_curl_url_host(const char *url, char *out, size_t cap);

/* Reduces a name from outside to a bare file name: drops any directory part, then refuses an empty
 * name, one containing "..", a control byte or a character FAT cannot hold, and a trailing dot or
 * space. Returns 0 with the name in out, or -1 with errno EINVAL or ENAMETOOLONG. */
int sh_curl_name_clean(const char *raw, char *out, size_t cap);

/* The file name for -O: the last path segment of the URL, query and fragment dropped, then cleaned.
 * Returns 0, or -1 with errno as sh_curl_name_clean (EINVAL when the path ends in a slash). */
int sh_curl_url_basename(const char *url, char *out, size_t cap);

/* The file name for -J from a Content-Disposition value: filename*= (RFC 5987, percent-decoded) wins
 * over filename=, and the result is cleaned. Returns 0 with the name in out, 1 when the header
 * offers no filename, or -1 with errno when it offers one that is refused. */
int sh_curl_disposition_name(const char *value, char *out, size_t cap);

#endif
