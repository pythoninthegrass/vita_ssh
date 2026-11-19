/* Host test for src/sh_curl_name.c; driven by scripts/test_sh_curl_name.py. */
#include "sh_curl_name.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            failures++;                                                             \
        }                                                                           \
    } while (0)

/* The one property that matters: whatever came in, a name that comes out is a bare file name. */
static int bare_name(const char *s)
{
    size_t n = strlen(s);

    return n > 0 && n <= SH_CURL_NAME_MAX && !strpbrk(s, "/\\:\"*?<>|") && !strstr(s, "..") &&
           strcmp(s, ".") != 0 && s[n - 1] != '.' && s[n - 1] != ' ';
}

static int url_why(const char *url, const char *want)
{
    const char *why = NULL;

    return sh_curl_url_check(url, &why) == -1 && why && strstr(why, want) != NULL;
}

static int url_ok(const char *url)
{
    const char *why = NULL;

    return sh_curl_url_check(url, &why) == 0;
}

static void test_url_check(void)
{
    char longurl[SH_CURL_URL_MAX + 32];

    CHECK(url_ok("https://github.com/x/y.suprx"));
    CHECK(url_ok("HTTP://a.b:8080/p?q#f"));
    CHECK(url_ok("http://192.168.8.1"));
    CHECK(url_ok("http://host?x=1"));
    CHECK(url_why("github.com/x", "missing scheme"));
    CHECK(url_why("", "missing scheme"));
    CHECK(url_why("ftp://h/x", "not http or https"));
    CHECK(url_why("file:///ux0/x", "not http or https"));
    CHECK(url_why("https://", "missing host"));
    CHECK(url_why("https:///x", "missing host"));
    CHECK(url_why("https://:80/x", "missing host"));
    CHECK(url_why("https://u:p@h/x", "credentials"));
    CHECK(url_why("https://h:/x", "bad port"));
    CHECK(url_why("https://h:0/x", "bad port"));
    CHECK(url_why("https://h:65536/x", "bad port"));
    CHECK(url_why("https://h:80a/x", "bad port"));
    CHECK(url_ok("https://h:65535/x"));
    CHECK(url_why("https://[::1]/x", "IPv6"));
    CHECK(url_why("https://h ost/x", "space"));
    CHECK(url_why("https://h\n/x", "control"));
    CHECK(url_why("https://h$t/x", "bad character in host"));
    memset(longurl, 'a', sizeof(longurl) - 1);
    longurl[sizeof(longurl) - 1] = '\0';
    memcpy(longurl, "https://", 8);
    CHECK(url_why(longurl, "too long"));
}

static void test_url_host(void)
{
    char h[64];

    CHECK(sh_curl_url_host("https://github.com/x", h, sizeof(h)) == 0 && strcmp(h, "github.com") == 0);
    CHECK(sh_curl_url_host("http://a.b:8080/p", h, sizeof(h)) == 0 && strcmp(h, "a.b") == 0);
    CHECK(sh_curl_url_host("http://a.b?x", h, sizeof(h)) == 0 && strcmp(h, "a.b") == 0);
    errno = 0;
    CHECK(sh_curl_url_host("https://github.com/x", h, 5) == -1 && errno == ENAMETOOLONG);
}

static int clean_is(const char *raw, const char *want)
{
    char out[64];

    return sh_curl_name_clean(raw, out, sizeof(out)) == 0 && strcmp(out, want) == 0;
}

static int clean_fails(const char *raw, int want_errno)
{
    char out[64];

    errno = 0;
    return sh_curl_name_clean(raw, out, sizeof(out)) == -1 && errno == want_errno;
}

static void test_clean(void)
{
    char small[4];
    char big[SH_CURL_NAME_MAX + 8];

    CHECK(clean_is("wifiturbo.suprx", "wifiturbo.suprx"));
    CHECK(clean_is("a/b/c.txt", "c.txt"));
    CHECK(clean_is("..\\..\\x.bin", "x.bin"));
    CHECK(clean_is("/etc/passwd", "passwd"));
    CHECK(clean_is("../x", "x"));
    CHECK(clean_fails("", EINVAL));
    CHECK(clean_fails("dir/", EINVAL));
    CHECK(clean_fails(".", EINVAL));
    CHECK(clean_fails("..", EINVAL));
    CHECK(clean_fails("a/..", EINVAL));
    CHECK(clean_fails("a..b", EINVAL));
    CHECK(clean_fails("x.", EINVAL));
    CHECK(clean_fails("x ", EINVAL));
    CHECK(clean_fails("ux0:evil", EINVAL));
    CHECK(clean_fails("a\nb", EINVAL));
    CHECK(clean_fails("a|b", EINVAL));
    CHECK(clean_is(".hidden", ".hidden"));
    errno = 0;
    CHECK(sh_curl_name_clean("abcd", small, sizeof(small)) == -1 && errno == ENAMETOOLONG);
    CHECK(sh_curl_name_clean("abc", small, sizeof(small)) == 0 && strcmp(small, "abc") == 0);
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    errno = 0;
    CHECK(sh_curl_name_clean(big, big, sizeof(big)) == -1 && errno == ENAMETOOLONG);
}

static int base_is(const char *url, const char *want)
{
    char out[64];

    return sh_curl_url_basename(url, out, sizeof(out)) == 0 && strcmp(out, want) == 0;
}

static void test_basename(void)
{
    char out[64];

    CHECK(base_is("https://github.com/a/b/wifiturbo.suprx", "wifiturbo.suprx"));
    CHECK(base_is("https://h/x.bin?token=a/b#frag", "x.bin"));
    CHECK(base_is("https://h/x.bin#a/b", "x.bin"));
    CHECK(base_is("https://h/a%20b", "a%20b"));
    errno = 0;
    CHECK(sh_curl_url_basename("https://h/dir/", out, sizeof(out)) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(sh_curl_url_basename("https://h", out, sizeof(out)) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(sh_curl_url_basename("https://h/..", out, sizeof(out)) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(sh_curl_url_basename("https://h/a/%2e%2e", out, sizeof(out)) == 0);
    CHECK(bare_name(out));
}

static int disp_is(const char *value, const char *want)
{
    char out[64];

    return sh_curl_disposition_name(value, out, sizeof(out)) == 0 && strcmp(out, want) == 0;
}

static int disp_none(const char *value)
{
    char out[64];

    return sh_curl_disposition_name(value, out, sizeof(out)) == 1;
}

static int disp_refused(const char *value)
{
    char out[64];

    errno = 0;
    return sh_curl_disposition_name(value, out, sizeof(out)) == -1 && errno != 0;
}

static void test_disposition(void)
{
    CHECK(disp_is("attachment; filename=\"wifiturbo.suprx\"", "wifiturbo.suprx"));
    CHECK(disp_is("attachment; filename=wifiturbo.suprx", "wifiturbo.suprx"));
    CHECK(disp_is("attachment;filename=x.bin;size=4", "x.bin"));
    CHECK(disp_is("ATTACHMENT; FILENAME=\"x.bin\"", "x.bin"));
    CHECK(disp_is("attachment; filename=\"a;b.bin\"; size=1", "a;b.bin"));
    CHECK(disp_refused("attachment; filename=\"a\\\"b.bin\""));
    CHECK(disp_is("attachment; filename=\"fallback.bin\"; filename*=UTF-8''real%20name.bin", "real name.bin"));
    CHECK(disp_is("attachment; filename*=UTF-8'en'caf%C3%A9.txt", "caf\xC3\xA9.txt"));
    CHECK(disp_none("attachment"));
    CHECK(disp_none("inline"));
    CHECK(disp_none(""));
    CHECK(disp_none("attachment; xfilename=\"a\""));
    CHECK(disp_none("attachment; note=\"filename=a\""));
    /* hostile values: stripped to a bare name or refused, never a path */
    CHECK(disp_is("attachment; filename=\"../../ux0/data/x\"", "x"));
    CHECK(disp_is("attachment; filename=\"..\\\\evil.bin\"", "evil.bin"));
    CHECK(disp_is("attachment; filename*=UTF-8''..%2F..%2Fx.bin", "x.bin"));
    CHECK(disp_is("attachment; filename*=UTF-8''%2fetc%2fpasswd", "passwd"));
    CHECK(disp_refused("attachment; filename=\"..\""));
    CHECK(disp_refused("attachment; filename=\"a..b\""));
    CHECK(disp_refused("attachment; filename=\"ux0:evil\""));
    CHECK(disp_refused("attachment; filename=\"\""));
    CHECK(disp_refused("attachment; filename*=UTF-8''%00x"));
    CHECK(disp_refused("attachment; filename*=UTF-8''%zz"));
    CHECK(disp_refused("attachment; filename*=UTF-8''%2"));
    CHECK(disp_refused("attachment; filename*=nonsense"));
}

/* Every string of up to five bytes from a hostile alphabet. */
static void sweep_clean(void)
{
    static const char alpha[] = {'a', '.', '/', '\\', ':', ' '};
    char s[6], out[16];
    size_t len, i;
    long total, k, v;

    for (len = 0; len <= 5; len++) {
        for (total = 1, i = 0; i < len; i++)
            total *= (long)sizeof(alpha);
        for (k = 0; k < total; k++) {
            for (v = k, i = 0; i < len; i++, v /= (long)sizeof(alpha))
                s[i] = alpha[v % (long)sizeof(alpha)];
            s[len] = '\0';
            if (sh_curl_name_clean(s, out, sizeof(out)) == 0)
                CHECK(bare_name(out));
        }
    }
}

/* Every sequence of up to four fragments, run through the Content-Disposition parser at two buffer sizes. */
static void sweep_disposition(void)
{
    static const char *frag[] = {"attachment;", " filename=", ";filename*=UTF-8''", "\"", "../", "%2F", "%2f",
                                 "..%2F", "a", ";", "\\", "%00", "%", "'", ":"};
    const size_t nfrag = sizeof(frag) / sizeof(frag[0]);
    char s[128], out8[8], out300[300];
    size_t len, i;
    long total, k, v;

    for (len = 1; len <= 4; len++) {
        for (total = 1, i = 0; i < len; i++)
            total *= (long)nfrag;
        for (k = 0; k < total; k++) {
            s[0] = '\0';
            for (v = k, i = 0; i < len; i++, v /= (long)nfrag)
                strcat(s, frag[v % (long)nfrag]);
            if (sh_curl_disposition_name(s, out8, sizeof(out8)) == 0)
                CHECK(bare_name(out8) && strlen(out8) < sizeof(out8));
            if (sh_curl_disposition_name(s, out300, sizeof(out300)) == 0)
                CHECK(bare_name(out300));
        }
    }
}

int main(void)
{
    test_url_check();
    test_url_host();
    test_clean();
    test_basename();
    test_disposition();
    sweep_clean();
    sweep_disposition();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
