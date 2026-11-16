/*
 * Host key, storage, and authentication.
 *
 * The policy is the one that makes a headless device reachable without a
 * keyboard in front of it: if an authorized_keys file exists and has at least
 * one key, that is the only way in. If it does not, the server generates a
 * password, prints it on the Vita's own screen, and lets you log in with any
 * username to put a key in place. Bootstrap, not a credential -- the console
 * says so.
 *
 * The host key is Ed25519: smallest, fastest, and the one every current client
 * accepts. It is generated on first boot and kept, so a client's known_hosts
 * entry stays valid across reboots -- which is the whole reason it is stored
 * rather than derived, and the reason a save that did not happen has to be said
 * out loud. An unannounced new key is a "REMOTE HOST IDENTIFICATION HAS CHANGED"
 * wall on the client and no way to tell sabotage from a full memory card.
 *
 * ---------------------------------------------------------------------------
 * Where the files live, and why it is a list
 *
 * Which partition a homebrew app can write is not knowable at build time. ux0
 * is the obvious candidate and the one every file manager can reach, but it is
 * not guaranteed to be there (a PCH-2000 with no card), not guaranteed to be
 * writable, and not guaranteed to have room. So the server owns an ordered list
 * of candidate directories and applies three rules to it:
 *
 *   1. host key IN  -- the first candidate holding a readable private key wins.
 *      Nothing is generated while a key exists anywhere, because a second
 *      identity is worse than a slow boot.
 *   2. host key OUT -- the first candidate that can be created and written
 *      receives the new key. If none can, the key stays in memory and the
 *      server still runs: no identity is better than no server, and the console
 *      status line says so in as many words.
 *   3. authorized_keys -- the first candidate that has the file is the one read,
 *      never a merge of all of them. Two files that both claim to be the
 *      authority is how a card you forgot about starts authorising you.
 *
 * The directory that received the host key, the file authorized_keys was read
 * from, and the directory a session lands in are each reported on the console.
 * A guess the user cannot see is worse than a fallback they can.
 * ---------------------------------------------------------------------------
 *
 * Entropy comes from the hardware. The VitaSDK OpenSSL port patches
 * rand_unix.o to call sceKernelGetRandomNumber, so RAND_bytes (and therefore
 * ssh_get_random, ssh_pki_generate_key, and every kex) is seeded by the device's
 * RNG, not by a clock. ssh_get_random is still checked once at startup: if that
 * ever stops being true, failing here is far better than generating a predictable
 * host key and finding out later.
 */
#include "sshd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libssh/server.h>

#include <psp2/kernel/rng.h>
#include <openssl/rand.h> /* the DRBG the port never feeds: see seed_entropy() */

#define MAX_AUTHORIZED_KEYS 32
#define PASSWORD_LEN 12

/* Longest authorized_keys line this file will read. An rsa-sha2-512 8192-bit
 * line is around 1.5 KB with its comment; 4 KB leaves room for a comment
 * somebody typed themselves. */
#define KEY_LINE_MAX 4096

/* How many per-line parse complaints reach the console before the count alone
 * has to carry them. A file of junk must not push the server's own startup
 * history off a 12-line screen. */
#define MAX_KEY_LOGS 3

#define HOSTKEY_NAME "ssh_host_ed25519_key"
#define HOSTKEY_PUB_NAME HOSTKEY_NAME ".pub"
#define KEYS_NAME "authorized_keys"

/* No i, l, o, 1, 0 or u: the password is read off a small screen and typed on a
 * different machine, so characters that blur together are a bug. */
static const char password_alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";

/* One entry per place the server is willing to keep its files. The relative
 * part is identical everywhere -- <device>:/data/vitasshd -- so what a person
 * learns once transfers to the next device.
 *
 * The spelling is the one sh_path.c hands the syscalls ("/" after the colon),
 * because these strings go to mkdir/stat/fopen, not to sceIo*: that is the
 * form every POSIX call in the server already uses, and sce_to_posix() below
 * is the reverse mapping the shell prints. A file manager takes the friendlier
 * ux0:data/vitasshd; both name the same directory. */
typedef struct {
    const char *sce; /* directory, in the syscall spelling */
    int ram;         /* backed by RAM: nothing in it survives a reboot */
} sh_datadir_t;

static const sh_datadir_t data_dirs[] = {
    { "ux0:/data/vitasshd",  0 }, /* the documented one: VitaShell and every file manager reach it */
    { "uma0:/data/vitasshd", 0 }, /* microSD / SD2Vita adapter, while mounted */
    { "xmc0:/data/vitasshd", 0 }, /* the Vita TV's own card */
    { "ur0:/data/vitasshd",  1 }, /* RAM disk: writable on every device, forgotten on reboot */
};
#define N_DATA_DIRS (sizeof(data_dirs) / sizeof(data_dirs[0]))

static sh_auth_t auth_state;
static ssh_key host_key;
static ssh_key authorized[MAX_AUTHORIZED_KEYS];
static int n_authorized;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int mkdir_quiet(const char *sce)
{
    if (mkdir(sce, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int is_directory(const char *sce)
{
    struct stat st;

    return stat(sce, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists(const char *sce)
{
    struct stat st;

    return stat(sce, &st) == 0;
}

/* dir + "/" + name. These buffers are 1 KB and the pieces are tens of
 * characters, so the failure branch is unreachable; it is here because a
 * snprintf that truncates would otherwise hand a syscall a cut-off path. */
static int join(char *dst, size_t dstlen, const char *dir, const char *name)
{
    int wrote;

    if (dst == NULL || dir == NULL || name == NULL || dstlen == 0) {
        return -1;
    }
    wrote = snprintf(dst, dstlen, "%s/%s", dir, name);
    return (wrote < 0 || (size_t)wrote >= dstlen) ? -1 : 0;
}

/* "ux0:/data/vitasshd" -> "/ux0/data/vitasshd". sh_path.c owns the mapping in
 * the other direction (and only for paths the shell was asked about); this is
 * the one place the server needs to spell its own files the way a session
 * prints them. */
static int sce_to_posix(const char *sce, char *dst, size_t dstlen)
{
    const char *colon = strchr(sce, ':');
    size_t devlen;

    /* Every string here comes from data_dirs[] above, so the shape is known: a
     * device name of at most four characters, then a slash. Anything else is not
     * a path we built, and guessing at it would put a session's ~ somewhere
     * nobody chose. */
    if (colon == NULL || strchr(colon + 1, ':') != NULL || colon[1] != '/') {
        return -1;
    }
    devlen = (size_t)(colon - sce);
    if (devlen == 0 || devlen > 4) {
        return -1;
    }
    if (snprintf(dst, dstlen, "/%.*s%s", (int)devlen, sce, colon + 1) >=
        (int)dstlen) {
        return -1;
    }
    return 0;
}

/*
 * Create a candidate directory, and the "data" directory above it.
 *
 * mkdir() makes one level and "<dev>:data" is missing on a card that has never
 * had homebrew write to it, so both are attempted. Then the answer is read back
 * with stat rather than trusted from the return code: mkdir's own idea of
 * success on a read-only partition is not consistent enough to make a
 * persistence decision with, and "is that directory there" is the only question
 * anyone is asking.
 */
static int ensure_dir(const char *sce_dir)
{
    char parent[SH_SCE_PATH_MAX];
    const char *slash = strrchr(sce_dir, '/');
    size_t plen;

    if (slash == NULL) {
        return -1;
    }
    plen = (size_t)(slash - sce_dir);
    if (plen == 0 || plen >= sizeof(parent)) {
        return -1;
    }
    memcpy(parent, sce_dir, plen);
    parent[plen] = '\0';

    if (is_directory(sce_dir)) {
        return 0;
    }
    mkdir_quiet(parent); /* may exist, may be read-only: the child decides */
    mkdir_quiet(sce_dir);
    return is_directory(sce_dir) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* the log file                                                       */
/* ------------------------------------------------------------------ */

#define LOG_NAME "sshd.log"

/*
 * The console is a ring buffer in RAM, so the run that most needs explaining is
 * the one that takes its explanation with it, and there is no serial cable and no
 * debug screen on this device to read instead. Every console line therefore goes
 * to a file too, in the first candidate directory that will take one: that file is
 * what a file manager or a USB mount shows after the app has already died.
 *
 * Appending rather than truncating, because the tail of the previous run is the
 * evidence and a crash is exactly the run you did not get to watch. Runs are told
 * apart by the banner sh_console_log writes when it opens.
 *
 * None of this can fail the boot: a server that cannot write a log is still a
 * server. Every error is ignored rather than reported, since the line that would
 * say "there is no log" has nowhere to be said.
 */
static FILE *log_fp;
static char log_path[SH_SCE_PATH_MAX];

int sh_log_open(void)
{
    size_t i;

    for (i = 0; i < N_DATA_DIRS; i++) {
        char path[SH_SCE_PATH_MAX];

        if (join(path, sizeof(path), data_dirs[i].sce, LOG_NAME) != 0) {
            continue;
        }
        if (ensure_dir(data_dirs[i].sce) != 0) {
            continue; /* read-only, not mounted, no room: try the next one */
        }
        log_fp = fopen(path, "a");
        if (log_fp != NULL) {
            snprintf(log_path, sizeof(log_path), "%s", path);
            return 0;
        }
    }
    log_path[0] = '\0';
    return -1;
}

void sh_log_line(const char *line)
{
    if (log_fp == NULL) {
        return;
    }
    fprintf(log_fp, "%s\n", line);
    /* Flushed per line, which is the whole point: the buffered half of the last
     * thing the app knew is the half a crash is guaranteed to lose. */
    fflush(log_fp);
}

const char *sh_log_path(void)
{
    return log_path;
}

void sh_log_close(void)
{
    if (log_fp != NULL) {
        fclose(log_fp);
        log_fp = NULL;
    }
    log_path[0] = '\0';
}

static void base64(const unsigned char *in, size_t inlen, char *out,
                   size_t outlen)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;

    for (i = 0; i + 2 < inlen; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) |
                     in[i + 2];

        if (o + 4 >= outlen) {
            out[o] = '\0';
            return;
        }
        out[o++] = tbl[(v >> 18) & 0x3f];
        out[o++] = tbl[(v >> 12) & 0x3f];
        out[o++] = tbl[(v >> 6) & 0x3f];
        out[o++] = tbl[v & 0x3f];
    }
    if (i < inlen) {
        unsigned v = (unsigned)in[i] << 16;
        int avail = (int)(inlen - i);

        if (avail > 1) {
            v |= (unsigned)in[i + 1] << 8;
        }
        if (o + 4 >= outlen) {
            out[o] = '\0';
            return;
        }
        out[o++] = tbl[(v >> 18) & 0x3f];
        out[o++] = tbl[(v >> 12) & 0x3f];
        out[o++] = (avail > 1) ? tbl[(v >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* OpenSSH prints SHA256:<base64 without padding>, so this does too: a person
 * comparing the Vita's screen against their client's host-key prompt is
 * matching two strings that have to look the same. */
static int record_fingerprint(ssh_key key, char *dst, size_t dstlen)
{
    unsigned char *hash = NULL;
    size_t hlen = 0;
    char b64[64];

    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) !=
        SSH_OK) {
        return -1;
    }
    base64(hash, hlen, b64, sizeof(b64));
    /* ssh_get_publickey_hash hands back an unsigned char *, and the free that
     * matches it takes unsigned char ** — which also NULLs it. The obvious
     * ssh_string_free_char() wants a char * and would need a cast. */
    ssh_clean_pubkey_hash(&hash);
    {
        char *pad = strchr(b64, '=');

        if (pad != NULL) {
            *pad = '\0';
        }
    }
    snprintf(dst, dstlen, "SHA256:%s", b64);
    return 0;
}

/* ------------------------------------------------------------------ */
/* host key                                                           */
/* ------------------------------------------------------------------ */

/* Rule 1: an existing key always wins over a new one, whichever candidate has
 * it. Returns that entry, or NULL when no candidate produced a key. */
static const sh_datadir_t *import_any_host_key(void)
{
    size_t i;

    for (i = 0; i < N_DATA_DIRS; i++) {
        char path[SH_SCE_PATH_MAX];

        if (join(path, sizeof(path), data_dirs[i].sce, HOSTKEY_NAME) != 0) {
            return NULL;
        }
        if (!path_exists(path)) {
            continue;
        }
        if (ssh_pki_import_privkey_file(path, NULL, NULL, NULL, &host_key) ==
                SSH_OK && host_key != NULL) {
            /* Read back from the RAM disk is still a new key every boot, so the
             * same warning belongs here as on the save path. */
            sh_console_log("host key: read %s%s", path,
                           data_dirs[i].ram
                               ? " (RAM disk: new fingerprint each boot)" : "");
            return &data_dirs[i];
        }
        host_key = NULL;
        /* There and unusable is worth a line of its own. It is the other way a
         * fingerprint moves under a client that already trusts it: a half-written
         * file from a card pulled mid-save, or a key someone encrypted. */
        sh_console_log("host key: %s is not a private key we can read", path);
    }
    return NULL;
}

static int generate_host_key(void)
{
    int rc;
    /* ssh_pki_generate_key() is deprecated in this libssh. Its replacement takes
     * a pki context, which carries the FIDO2/security-key enrolment options an
     * ed25519 host key has no use for -- but it is still a required argument, so
     * one is created, used and dropped right here. */
    ssh_pki_ctx ctx = ssh_pki_ctx_new();

    if (ctx == NULL) {
        return -1;
    }
    rc = ssh_pki_generate_key(SSH_KEYTYPE_ED25519, ctx, &host_key);
    ssh_pki_ctx_free(ctx);
    if (rc != SSH_OK || host_key == NULL) {
        host_key = NULL;
        return -1;
    }
    return 0;
}

/*
 * The public half, in OpenSSH's one-line form, so that
 * `ssh-keygen -lf ssh_host_ed25519_key.pub` on a devbox prints the same
 * SHA256:... the Vita's screen shows. That pairing is what turns a scary
 * key-change warning into a two-minute check: compare the file against the
 * screen and you know whether the key really changed.
 *
 * Written here rather than with ssh_pki_export_pubkey_file() because that helper
 * leaves off the comment field, and "vita-ssh-server" is what tells you which
 * app wrote the file when you are reading a memory card full of keys.
 */
static int write_public_key(const char *sce_path)
{
    const char *algo;
    char *b64 = NULL;
    FILE *fp;
    int rc = -1;

    algo = ssh_key_type_to_char(ssh_key_type(host_key));
    if (algo == NULL) {
        return -1;
    }
    if (ssh_pki_export_pubkey_base64(host_key, &b64) != SSH_OK || b64 == NULL) {
        ssh_string_free_char(b64);
        return -1;
    }
    fp = fopen(sce_path, "w");
    if (fp != NULL) {
        if (fprintf(fp, "%s %s vita-ssh-server\n", algo, b64) > 0) {
            rc = 0;
        }
        if (fclose(fp) != 0) {
            rc = -1;
        }
    }
    ssh_string_free_char(b64);
    return rc;
}

/* Rule 2: the first candidate that can be created and written takes the key.
 * Returns that entry, or NULL when nothing would accept it. */
static const sh_datadir_t *save_host_key(void)
{
    size_t i;

    for (i = 0; i < N_DATA_DIRS; i++) {
        char path[SH_SCE_PATH_MAX];
        char pub[SH_SCE_PATH_MAX];

        if (join(path, sizeof(path), data_dirs[i].sce, HOSTKEY_NAME) != 0 ||
            join(pub, sizeof(pub), data_dirs[i].sce, HOSTKEY_PUB_NAME) != 0) {
            return NULL;
        }
        if (ensure_dir(data_dirs[i].sce) != 0) {
            continue; /* read-only, not mounted, no room: try the next one */
        }
        if (ssh_pki_export_privkey_file(host_key, NULL, NULL, NULL, path) !=
            SSH_OK) {
            continue;
        }
        sh_console_log("host key: new ed25519 key in %s%s", data_dirs[i].sce,
                       data_dirs[i].ram
                           ? " (RAM disk: new fingerprint each boot)" : "");
        /* The private half is what makes the server work, so a public half that
         * will not write is a note and not a reason to call this a failed save. */
        if (write_public_key(pub) != 0) {
            sh_console_log("warning: wrote the key but not its .pub");
        }
        return &data_dirs[i];
    }
    return NULL;
}

/* Rules 1 and 2 together. Returns the directory the key came from or went to --
 * NULL when the key exists only in this process, which is a working server with
 * a fingerprint that will not survive the boot -- and -1 only when there is no
 * host key at all, the one failure that stops the server. */
static int load_or_create_host_key(const sh_datadir_t **dir_out)
{
    const sh_datadir_t *dir = import_any_host_key();

    *dir_out = dir;
    if (dir != NULL) {
        return 0;
    }
    if (generate_host_key() != 0) {
        return -1;
    }
    dir = save_host_key();
    *dir_out = dir;
    if (dir == NULL) {
        /* Deliberate: a device that cannot write anywhere still serves a
         * session, it just introduces itself at every boot. Said in the status
         * line as well as here, because this log scrolls away. */
        sh_console_log("host key: new ed25519 key NOT SAVED (new fingerprint each boot)");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* authorized_keys                                                    */
/* ------------------------------------------------------------------ */

/* Strip the trailing whitespace a file picks up from a Windows editor or a
 * browser, and the leading blanks a hand-indented line brings with it. A '\r'
 * glued to the end of the base64 is the single most common way a perfectly good
 * public key stops being one. */
static char *trim_line(char *line)
{
    char *p = line;
    size_t len;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    len = strlen(p);
    while (len > 0 && (unsigned char)p[len - 1] <= ' ') {
        p[--len] = '\0';
    }
    return p;
}

/* Throws away the tail of a line too long to read, so the remainder is not
 * later mistaken for a line of its own. c is the character already read past
 * the end of the buffer. */
static void discard_rest_of_line(FILE *fp, int c)
{
    while (c != EOF && c != '\n') {
        c = fgetc(fp);
    }
}

/*
 * One line of authorized_keys, in the usual `algorithm base64 comment` form.
 *
 * Returns 1 when a key was accepted, 0 when the line was skipped for a reason
 * now in *why (and possibly logged), which the caller counts.
 */
static int parse_key_line(char *line, int lineno, int *logged,
                          const char **why)
{
    enum ssh_keytypes_e type = SSH_KEYTYPE_UNKNOWN;
    ssh_key key = NULL;
    char *b64 = NULL;
    char *save = NULL;
    char *p;
    char *algo = NULL;
    int options_skipped = 0;

    *why = NULL;
    p = trim_line(line);
    if (*p == '\0' || *p == '#') {
        return 0; /* blank or comment: not a line worth a counter */
    }

    /*
     * OpenSSH allows option prefixes (from="…", command="…", no-pty). This
     * server honours none of them, but discarding the key because of one is how
     * a person locks themselves out of a device they configured themselves:
     * skip tokens until one names an algorithm we can parse, and say we did.
     */
    for (p = strtok_r(p, " \t", &save); p != NULL;
         p = strtok_r(NULL, " \t", &save)) {
        type = ssh_key_type_from_name(p);
        if (type != SSH_KEYTYPE_UNKNOWN) {
            algo = p;
            b64 = strtok_r(NULL, " \t", &save);
            break;
        }
        if (!options_skipped) {
            options_skipped = 1;
            if (*logged < MAX_KEY_LOGS) {
                (*logged)++;
                sh_console_log("authorized_keys line %d: ignoring options"
                               " before the key", lineno);
            }
        }
    }

    if (b64 == NULL) {
        *why = (algo == NULL) ? "no key on this line"
                              : "key data missing after the algorithm";
        return 0;
    }
    /* Parsed into a local: the table only ever holds keys that came back whole. */
    if (ssh_pki_import_pubkey_base64(b64, type, &key) != SSH_OK || key == NULL) {
        *why = "unparseable key";
        return 0;
    }
    if (!ssh_key_is_public(key)) {
        ssh_key_free(key);
        *why = "not a public key";
        return 0;
    }
    authorized[n_authorized++] = key;
    return 1;
}

static void parse_authorized_keys(FILE *fp, const char *path)
{
    char line[KEY_LINE_MAX];
    int lineno = 0;
    int rejected = 0;
    int ignored = 0;
    int logged = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *why = NULL;

        lineno++;
        /*
         * A line that filled the buffer without a newline has more of itself
         * coming. fgetc rather than feof() is what decides: the EOF flag only
         * sets once a read has run past the end, so testing it here would throw
         * away a file's last key whenever the file has no trailing newline --
         * which is how most people leave the file when they type it.
         */
        if (strchr(line, '\n') == NULL) {
            int c = fgetc(fp);

            if (c != EOF) {
                discard_rest_of_line(fp, c);
                rejected++;
                if (logged < MAX_KEY_LOGS) {
                    logged++;
                    sh_console_log("authorized_keys line %d: too long to read"
                                   " (skipped)", lineno);
                }
                continue;
            }
        }
        if (n_authorized >= MAX_AUTHORIZED_KEYS) {
            /* Counted rather than dropped quietly: the person with 40 keys in the
             * file needs to know the last 8 are not working. Comments past the
             * limit are not keys, so they do not pad the count. */
            char *rest = trim_line(line);

            if (*rest != '\0' && *rest != '#') {
                ignored++;
            }
            continue;
        }
        if (parse_key_line(line, lineno, &logged, &why) == 1) {
            continue;
        }
        if (why == NULL) {
            continue; /* blank or comment */
        }
        rejected++;
        if (logged < MAX_KEY_LOGS) {
            logged++;
            sh_console_log("authorized_keys line %d: %s (skipped)", lineno, why);
        }
    }

    if (n_authorized > 0) {
        sh_console_log("authorized_keys: %d key(s) from %s", n_authorized, path);
    } else {
        sh_console_log("authorized_keys: no usable key in %s", path);
    }
    if (rejected > 0 || ignored > 0) {
        sh_console_log("authorized_keys: %d unusable line(s), %d over the %d"
                       " key limit", rejected, ignored, MAX_AUTHORIZED_KEYS);
    }
    /* The screen's one line has to carry what the log scrolled away: every line
     * in that file that did not become a key, including the ones the limit
     * stopped us reading. */
    auth_state.keys_rejected = rejected + ignored;
}

/* Rule 3: first candidate holding the file wins, and the search stops there. */
static void load_authorized_keys(char *used_path, size_t usedlen)
{
    size_t i;

    for (i = 0; i < N_DATA_DIRS; i++) {
        char path[SH_SCE_PATH_MAX];
        FILE *fp;

        if (join(path, sizeof(path), data_dirs[i].sce, KEYS_NAME) != 0) {
            return;
        }
        fp = fopen(path, "r");
        if (fp == NULL) {
            continue; /* not there: another candidate may have one */
        }
        parse_authorized_keys(fp, path);
        fclose(fp);
        snprintf(used_path, usedlen, "%s", path);
        return;
    }
    /* No file anywhere is the normal first run, and the expected one: the
     * password this leaves live is what the bootstrap is for. */
    sh_console_log("authorized_keys: none found (tried %u directories, first"
                   " is %s)", (unsigned)N_DATA_DIRS, data_dirs[0].sce);
}

/* ------------------------------------------------------------------ */
/* password fallback                                                  */
/* ------------------------------------------------------------------ */

static void generate_password(char *dst, size_t dstlen)
{
    unsigned char raw[32];
    size_t i, room = dstlen - 1;
    size_t len = (PASSWORD_LEN < room) ? PASSWORD_LEN : room;
    size_t alphabet = sizeof(password_alphabet) - 1; /* the NUL is not a letter */

    if (ssh_get_random(raw, (int)sizeof(raw), 1) != SSH_OK) {
        /* Fall back to the kernel RNG rather than shipping a fixed password: if
         * this ever fires, the answer is to not accept password auth at all,
         * which is what the caller will see as an unusable password. */
        if (sceKernelGetRandomNumber(raw, sizeof(raw)) < 0) {
            dst[0] = '\0';
            return;
        }
    }
    for (i = 0; i < len; i++) {
        dst[i] = password_alphabet[raw[i] % alphabet];
    }
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* the directory a session lands in                                   */
/* ------------------------------------------------------------------ */

/*
 * Where ~ points. Same order as everything else above: the directory the host
 * key came from or went to -- the one place the search has proved is there, and
 * the one place a session's `cat ~/authorized_keys` means something -- else the
 * first candidate that exists at all, else ux0's root, which the shell can list
 * even when nothing on it can be written. A session that lands somewhere real is
 * worth more than one that lands on a path it only thinks it has.
 */
static void choose_home(const sh_datadir_t *key_dir)
{
    char posix[SH_POSIX_PATH_MAX];
    size_t i;

    if (key_dir != NULL && sce_to_posix(key_dir->sce, posix, sizeof(posix)) == 0) {
        snprintf(auth_state.home, sizeof(auth_state.home), "%s", posix);
        return;
    }
    for (i = 0; i < N_DATA_DIRS; i++) {
        if (is_directory(data_dirs[i].sce) &&
            sce_to_posix(data_dirs[i].sce, posix, sizeof(posix)) == 0) {
            snprintf(auth_state.home, sizeof(auth_state.home), "%s", posix);
            return;
        }
    }
    snprintf(auth_state.home, sizeof(auth_state.home), "/ux0");
}

/* ------------------------------------------------------------------ */
/* entropy                                                            */
/* ------------------------------------------------------------------ */

/*
 * The Vita OpenSSL port has exactly one patch that reads the device's hardware
 * RNG -- crypto/rand/rand_unix.c calls sceKernelGetRandomNumber where a desktop
 * build would call getentropy -- and that patch sits inside
 *
 *     #if defined(OPENSSL_RAND_SEED_GETRANDOM)
 *
 * which nothing ever turns on: the vita-cross target in libs/openssl/VITABUILD
 * passes no --with-rand-seed, and the installed configuration.h defines no
 * OPENSSL_RAND_SEED_* at all. The patched code is therefore not in the binary,
 * the DRBG is never fed, RAND_bytes returns 0, and everything above it fails
 * with it -- ssh_get_random, ssh_pki_generate_key, and so the host key. That is
 * what "error: no entropy from the platform RNG" meant on the device.
 *
 * Feeding the DRBG here is the smallest repair that can be proved. It is said
 * out loud rather than hoped for because a host key generated from a DRBG nobody
 * checked was seeded is a fingerprint no client can trust: the kernel RNG's own
 * return code and the DRBG's verdict before and after both go to the log, in the
 * order the layers sit, so the next failure of this kind names its own layer
 * without another build. If the kernel RNG itself fails there is nothing to fall
 * back to, and sh_auth_prepare() refuses to make a key.
 */
static void seed_entropy(void)
{
    unsigned char seed[64];
    unsigned char probe[16];
    int rng = sceKernelGetRandomNumber(seed, (int)sizeof(seed));
    int was = RAND_status();

    if (rng == 0) {
        RAND_seed(seed, sizeof(seed));
    }
    sh_console_log("entropy: kernel RNG 0x%08X, DRBG seeded %d -> %d",
                   (unsigned int)rng, was, RAND_status());
    /* The two calls the server actually depends on, reported side by side: if
     * these ever disagree, the bug is in libssh's path rather than OpenSSL's. */
    sh_console_log("entropy: RAND_bytes -> %d, ssh_get_random -> %d",
                   RAND_bytes(probe, (int)sizeof(probe)),
                   ssh_get_random(probe, (int)sizeof(probe), 1));
}

/* ------------------------------------------------------------------ */
/* public interface                                                   */
/* ------------------------------------------------------------------ */

int sh_auth_prepare(sh_auth_t *auth)
{
    const sh_datadir_t *key_dir = NULL;
    unsigned char probe[8];

    memset(&auth_state, 0, sizeof(auth_state));
    /* Zero the caller's struct too, not just the internal one: the console is
     * drawn whether this succeeds or fails, and on the failure path the copy
     * below never runs -- so a caller that printed auth.fingerprint, auth.home or
     * auth.password after an error would be printing uninitialised stack. A
     * screen showing a password nobody generated is worse than a screen refusing
     * to show one. */
    memset(auth, 0, sizeof(*auth));
    n_authorized = 0;

    seed_entropy();
    if (ssh_get_random(probe, (int)sizeof(probe), 1) != SSH_OK) {
        sh_console_log("error: no entropy from the platform RNG, "
                       "cannot make a host key");
        return -1;
    }
    if (load_or_create_host_key(&key_dir) != 0) {
        sh_console_log("error: could not obtain a host key");
        return -1;
    }
    if (record_fingerprint(host_key, auth_state.fingerprint,
                           sizeof(auth_state.fingerprint)) != 0) {
        snprintf(auth_state.fingerprint, sizeof(auth_state.fingerprint),
                 "(unavailable)");
    }
    /* Persisted means "same fingerprint after a reboot", which a key sitting on
     * the RAM disk does not. The screen says which of the three cases this is:
     * saved on storage, kept in RAM, or nowhere. */
    auth_state.key_persisted = (key_dir != NULL && !key_dir->ram);
    if (key_dir != NULL &&
        join(auth_state.key_path, sizeof(auth_state.key_path), key_dir->sce,
             HOSTKEY_NAME) != 0) {
        auth_state.key_path[0] = '\0';
        auth_state.key_persisted = 0;
    }

    load_authorized_keys(auth_state.keys_path, sizeof(auth_state.keys_path));
    auth_state.keys_loaded = n_authorized;
    if (n_authorized == 0) {
        generate_password(auth_state.password, sizeof(auth_state.password));
        auth_state.password_used = (auth_state.password[0] != '\0');
    }
    choose_home(key_dir);
    *auth = auth_state;
    return 0;
}

const sh_auth_t *sh_server_auth(void)
{
    return &auth_state;
}

ssh_key sh_server_host_key(void)
{
    return host_key;
}

const char *sh_server_home_posix(void)
{
    return (auth_state.home[0] != '\0') ? auth_state.home : "/ux0";
}

int sh_server_auth_methods(void)
{
    if (auth_state.keys_loaded > 0) {
        return SSH_AUTH_METHOD_PUBLICKEY;
    }
    return auth_state.password_used ? SSH_AUTH_METHOD_PASSWORD : 0;
}

int sh_auth_check_pubkey(const sh_auth_t *auth, const char *user, ssh_key key)
{
    int i;

    (void)user; /* any username is accepted; the key is the credential */
    if (auth == NULL || key == NULL || !ssh_key_is_public(key)) {
        return -1;
    }
    for (i = 0; i < n_authorized; i++) {
        if (ssh_key_cmp(key, authorized[i], SSH_KEY_CMP_PUBLIC) == 0) {
            return 0;
        }
    }
    return -1;
}

int sh_auth_check_password(const sh_auth_t *auth, const char *user,
                           const char *password)
{
    (void)user;
    if (auth == NULL || password == NULL || !auth->password_used) {
        return -1;
    }
    /* Not constant time. Accepted deliberately: this password exists to let you
     * install an authorized_keys file on a device with no keyboard, and it is
     * gone the moment that file has a key in it. */
    return (strcmp(password, auth->password) == 0) ? 0 : -1;
}
