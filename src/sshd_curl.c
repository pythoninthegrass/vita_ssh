/* The HTTP(S) download engine behind curl; see include/sshd_curl.h. */
#include "sshd_curl.h"
#include "sh_curl_name.h"

#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define POLL_MS 50
#define PROGRESS_S 1
#define DISPO_MAX 1024

/* Tried in order; libcurl has no default CA path, so one must always be set. */
static const char *const ca_stores[] = {
    "vs0:data/external/cert/CA_LIST.cer",
    "ux0:data/cacert.pem",
};

typedef struct {
    sh_session_t *s;
    const sshd_curl_opts_t *o;
    int to_file;
    int fd;
    char final_sce[SH_SCE_PATH_MAX];
    char part_sce[SH_SCE_PATH_MAX + 8];
    char label[NAME_MAX + 1];
    char url_name[NAME_MAX + 1];
    char dispo[DISPO_MAX];
    int dispo_too_long;
    curl_off_t total;
    int capped;
    int open_failed;
    int write_errno;
} xfer_t;

static int ensure_global_init(void)
{
    static int done;

    if (!done) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            return -1;
        done = 1;
    }
    return 0;
}

/* The name the transfer's file gets: -o as typed, or the Content-Disposition or URL name for -O. */
static int choose_name(xfer_t *x, const char **name)
{
    if (x->o->out_arg) {
        *name = x->o->out_arg;
        return 0;
    }
    if (x->o->use_dispo && x->dispo[0] != '\0') {
        char clean[NAME_MAX + 1];
        int r = x->dispo_too_long ? -1 : sh_curl_disposition_name(x->dispo, clean, sizeof(clean));

        if (r < 0) {
            sh_eprintf(x->s, "%s: refusing the file name in Content-Disposition: %s\n", x->o->tool, x->dispo);
            return -1;
        }
        if (r == 0) {
            memcpy(x->label, clean, strlen(clean) + 1);
            *name = x->label;
            return 0;
        }
    }
    *name = x->url_name;
    return 0;
}

/* Opens the .part file next to the final path, so a failure never destroys an earlier file. */
static int open_target(xfer_t *x)
{
    const char *name;

    if (choose_name(x, &name) != 0 || x->o->resolve_out(x->o->ctx, name, x->final_sce, sizeof(x->final_sce)) != 0)
        return -1;
    if (snprintf(x->part_sce, sizeof(x->part_sce), "%s.part", x->final_sce) >= (int)sizeof(x->part_sce)) {
        sh_eprintf(x->s, "%s: %s: %s\n", x->o->tool, name, strerror(ENAMETOOLONG));
        return -1;
    }
    x->fd = open(x->part_sce, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (x->fd < 0) {
        sh_eprintf(x->s, "%s: %s: %s\n", x->o->tool, name, strerror(errno));
        return -1;
    }
    if (name != x->label)
        snprintf(x->label, sizeof(x->label), "%s", name);
    return 0;
}

static size_t on_body(char *p, size_t size, size_t nmemb, void *user)
{
    xfer_t *x = user;
    size_t len = size * nmemb;
    size_t put = 0;

    if (x->total + (curl_off_t)len > (curl_off_t)SSHD_CURL_MAX_BYTES) {
        x->capped = 1;
        return 0;
    }
    x->total += (curl_off_t)len;
    if (!x->to_file) {
        sh_write(x->s, p, len);
        return len;
    }
    if (x->fd < 0 && open_target(x) != 0) {
        x->open_failed = 1;
        return 0;
    }
    while (put < len) {
        ssize_t one = write(x->fd, p + put, len - put);

        if (one <= 0) {
            x->write_errno = errno ? errno : EIO;
            return 0;
        }
        put += (size_t)one;
    }
    return len;
}

static size_t on_header(char *line, size_t size, size_t nmemb, void *user)
{
    xfer_t *x = user;
    size_t len = size * nmemb;
    static const char key[] = "content-disposition:";

    if (x->o->head_only) {
        sh_write(x->s, line, len);
        return len;
    }
    if (len >= 5 && strncasecmp(line, "HTTP/", 5) == 0) {
        x->dispo[0] = '\0'; /* a redirect's headers say nothing about the final response */
        x->dispo_too_long = 0;
    } else if (len > sizeof(key) - 1 && strncasecmp(line, key, sizeof(key) - 1) == 0) {
        const char *v = line + sizeof(key) - 1;
        size_t n = len - (sizeof(key) - 1);

        while (n > 0 && (*v == ' ' || *v == '\t')) {
            v++;
            n--;
        }
        while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == '\n'))
            n--;
        if (n >= sizeof(x->dispo)) {
            x->dispo_too_long = 1;
            n = sizeof(x->dispo) - 1;
        }
        memcpy(x->dispo, v, n);
        x->dispo[n] = '\0';
    }
    return len;
}

static void progress(xfer_t *x, CURL *easy, int final)
{
    curl_off_t want = 0;
    int bytes;

    curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &want);
    bytes = (want > 0 ? want : x->total) < 1024;
    if (want > 0) {
        sh_eprintf(x->s, "\r  %u of %u %s (%u%%)   ", (unsigned)(bytes ? x->total : x->total / 1024),
                   (unsigned)(bytes ? want : want / 1024), bytes ? "bytes" : "KB", (unsigned)(x->total * 100 / want));
    } else {
        sh_eprintf(x->s, "\r  %u %s   ", (unsigned)(bytes ? x->total : x->total / 1024), bytes ? "bytes" : "KB");
    }
    if (final)
        sh_eprintf(x->s, "\n");
}

static const char *pick_ca_store(void)
{
    struct stat st;
    size_t i;

    for (i = 0; i < sizeof(ca_stores) / sizeof(ca_stores[0]); i++) {
        if (stat(ca_stores[i], &st) == 0)
            return ca_stores[i];
    }
    return NULL;
}

static void report(xfer_t *x, CURLcode rc, const char *host, const char *detail, long http)
{
    sh_session_t *s = x->s;

    switch (rc) {
    case CURLE_HTTP_RETURNED_ERROR:
        sh_eprintf(s, "%s: %s: the server answered HTTP %ld\n", x->o->tool, host, http);
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
        sh_eprintf(s, "%s: could not resolve host %s\n", x->o->tool, host);
        break;
    case CURLE_COULDNT_CONNECT:
        sh_eprintf(s, "%s: could not connect to %s\n", x->o->tool, host);
        break;
    case CURLE_OPERATION_TIMEDOUT:
        sh_eprintf(s, "%s: %s: timed out (no connection in %d s, or under 1 KB/s for %d s)\n", x->o->tool, host,
                   SSHD_CURL_CONNECT_S, SSHD_CURL_STALL_S);
        break;
    case CURLE_TOO_MANY_REDIRECTS:
        sh_eprintf(s, "%s: %s: more than %d redirects (limit reached or a redirect loop)\n", x->o->tool, host,
                   SSHD_CURL_MAX_REDIRS);
        break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
        sh_eprintf(s, "%s: %s: certificate could not be verified (%s); %s skips the check\n", x->o->tool, host,
                   detail[0] ? detail : curl_easy_strerror(rc), x->o->insecure_flag);
        break;
    default:
        sh_eprintf(s, "%s: %s: %s\n", x->o->tool, host, detail[0] ? detail : curl_easy_strerror(rc));
        break;
    }
}

static void finish_file(xfer_t *x, int ok)
{
    if (x->fd < 0)
        return;
    close(x->fd);
    x->fd = -1;
    if (!ok) {
        unlink(x->part_sce);
        return;
    }
    unlink(x->final_sce);
    if (rename(x->part_sce, x->final_sce) != 0) {
        sh_eprintf(x->s, "%s: %s: %s; the download is kept as %s\n", x->o->tool, x->label, strerror(errno), x->part_sce);
    }
}

int sshd_curl_run(sh_session_t *s, const sshd_curl_opts_t *o)
{
    xfer_t *x;
    CURL *easy = NULL;
    CURLM *multi = NULL;
    CURLcode rc = CURLE_FAILED_INIT;
    char host[256], err[CURL_ERROR_SIZE];
    const char *why = NULL, *ca = NULL;
    long http = 0;
    int status = 2, running = 1;
    time_t shown = 0;

    if (sh_curl_url_check(o->url, &why) != 0) {
        sh_eprintf(s, "%s: %s: %s\n", o->tool, o->url, why);
        return 2;
    }
    if (sh_curl_url_host(o->url, host, sizeof(host)) != 0)
        snprintf(host, sizeof(host), "the server");
    if (o->insecure) {
        sh_eprintf(s, "%s: warning: certificate verification is off (%s); the connection is not authenticated\n", o->tool, o->insecure_flag);
    } else if (strncasecmp(o->url, "https://", 8) == 0 && (ca = pick_ca_store()) == NULL) {
        sh_eprintf(s, "%s: no CA store found (%s, %s); %s skips certificate checks\n", o->tool, ca_stores[0], ca_stores[1],
                   o->insecure_flag);
        return 2;
    }
    if (ensure_global_init() != 0) {
        sh_eprintf(s, "%s: libcurl failed to start\n", o->tool);
        return 2;
    }
    x = calloc(1, sizeof(*x));
    if (x == NULL) {
        sh_eprintf(s, "%s: out of memory\n", o->tool);
        return 2;
    }
    x->s = s;
    x->o = o;
    x->fd = -1;
    x->to_file = o->out_arg != NULL || o->by_url_name;
    err[0] = '\0';

    if (o->by_url_name && sh_curl_url_basename(o->url, x->url_name, sizeof(x->url_name)) != 0) {
        sh_eprintf(s, "%s: %s: no usable file name in the URL; name one with %s\n", o->tool, o->url, o->out_flag);
        goto out;
    }
    if (x->to_file && !o->head_only) {
        const char *name = o->out_arg ? o->out_arg : x->url_name;

        if (o->resolve_out(o->ctx, name, x->final_sce, sizeof(x->final_sce)) != 0) {
            status = 1;
            goto out;
        }
    }
    easy = curl_easy_init();
    multi = curl_multi_init();
    if (easy == NULL || multi == NULL) {
        sh_eprintf(s, "%s: out of memory\n", o->tool);
        goto out;
    }
    curl_easy_setopt(easy, CURLOPT_URL, o->url);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "vitasshd-curl");
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, (long)SSHD_CURL_CONNECT_S);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, (long)SSHD_CURL_STALL_S);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, o->follow ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, (long)SSHD_CURL_MAX_REDIRS);
    curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)SSHD_CURL_MAX_BYTES);
    curl_easy_setopt(easy, CURLOPT_FAILONERROR, o->fail_http ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_NOBODY, o->head_only ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, x);
    if (o->insecure) {
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    } else if (ca) {
        curl_easy_setopt(easy, CURLOPT_CAINFO, ca);
    }
    curl_multi_add_handle(multi, easy);

    while (running) {
        int numfds = 0;

        curl_multi_perform(multi, &running);
        if (!running)
            break;
        if (sh_session_interrupted(s)) {
            sh_eprintf(s, "%s: session closed, transfer abandoned\n", o->tool);
            status = 1;
            goto out;
        }
        sh_session_yield(s);
        if (x->fd >= 0 && !o->silent && time(NULL) - shown >= PROGRESS_S) {
            progress(x, easy, 0);
            shown = time(NULL);
        }
        /* Data waiting means loop again at once; only an idle socket sleeps, in the server's poll. */
        curl_multi_wait(multi, NULL, 0, 0, &numfds);
        if (numfds == 0)
            sh_server_poll_slice(POLL_MS);
    }
    {
        int left;
        CURLMsg *m = curl_multi_info_read(multi, &left);

        if (m && m->msg == CURLMSG_DONE)
            rc = m->data.result;
    }
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http);
    if (rc == CURLE_OK && x->to_file && !o->head_only && x->fd < 0 && !x->open_failed && open_target(x) != 0) {
        rc = CURLE_WRITE_ERROR; /* an empty body still creates the file */
        x->open_failed = 1;
    }
    if (x->to_file && !o->silent && !o->head_only && x->fd >= 0)
        progress(x, easy, 1);
    if (rc == CURLE_OK) {
        finish_file(x, 1);
        status = 0;
    } else {
        finish_file(x, 0);
        status = (int)rc;
        if (x->capped || rc == CURLE_FILESIZE_EXCEEDED) {
            status = (int)CURLE_FILESIZE_EXCEEDED;
            if (!o->silent || o->show_errors)
                sh_eprintf(s, "%s: %s: larger than the %u MB limit; stopped%s\n", o->tool, host, SSHD_CURL_MAX_BYTES >> 20,
                           x->to_file ? " and the partial file removed" : "");
        } else if (x->open_failed) {
            status = 23; /* open_target() already said why */
        } else if (x->write_errno) {
            if (!o->silent || o->show_errors)
                sh_eprintf(s, "%s: %s: %s\n", o->tool, x->label, strerror(x->write_errno));
            status = 23;
        } else if (!o->silent || o->show_errors) {
            report(x, rc, host, err, http);
        }
    }
out:
    finish_file(x, 0);
    if (multi && easy)
        curl_multi_remove_handle(multi, easy);
    if (easy)
        curl_easy_cleanup(easy);
    if (multi)
        curl_multi_cleanup(multi);
    free(x);
    return status;
}
