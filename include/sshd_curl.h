/* The HTTP(S) download engine behind curl: libcurl driven from the server's poll loop. */
#ifndef SSHD_CURL_H
#define SSHD_CURL_H

#include <stddef.h>

#include "sshd.h"

/* Most redirects -L follows, and most bytes one transfer may deliver. */
#define SSHD_CURL_MAX_REDIRS 10
#define SSHD_CURL_MAX_BYTES (256u * 1024u * 1024u)

/* Seconds to connect, and seconds a transfer may stay under 1 KB/s before it is abandoned. */
#define SSHD_CURL_CONNECT_S 15
#define SSHD_CURL_STALL_S 30

typedef struct {
    const char *url;
    const char *out_arg;   /* -o FILE, or NULL */
    int by_url_name;       /* -O: name the file after the URL's last path segment */
    int use_dispo;         /* -J: prefer the Content-Disposition name (with -O) */
    int follow;            /* -L */
    int head_only;         /* -I */
    int fail_http;         /* -f */
    int silent;            /* -s */
    int show_errors;       /* -S */
    int insecure;          /* -k */
    /* Resolves a file name to its device path and checks the place is writable; 0, or -1 after
     * printing the reason. Called before the transfer for a name known up front, and again once
     * headers are in when -J chooses the name. */
    int (*resolve_out)(void *ctx, const char *name, char *sce, size_t cap);
    void *ctx;
} sshd_curl_opts_t;

/* Runs one transfer to completion and returns the exit status: 0, curl's own code for a transfer
 * failure (22 for -f, 28 timeout, 60 certificate, 63 size cap, ...), or 2 for a bad request. Body
 * bytes go to the output file, or through sh_write() with none; messages go through sh_eprintf(). */
int sshd_curl_run(sh_session_t *s, const sshd_curl_opts_t *o);

#endif
