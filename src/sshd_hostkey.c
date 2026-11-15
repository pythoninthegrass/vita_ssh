/*
 * Host key and authentication.
 *
 * The policy is the one that makes a headless device reachable without a
 * keyboard in front of it: if ux0:data/vitasshd/authorized_keys exists and has
 * at least one key, that is the only way in. If it does not, the server
 * generates a password, prints it on the Vita's own screen, and lets you log in
 * with any username to put a key in place. Bootstrap, not a credential -- the
 * console says so.
 *
 * The host key is Ed25519: smallest, fastest, and the one every current client
 * accepts. It is generated on first boot and kept, so a client's known_hosts
 * entry stays valid across reboots -- which is the whole reason it is stored
 * rather than derived.
 *
 * Entropy comes from the hardware. The VitaSDK OpenSSL port patches
 * rand_unix.o to call sceKernelGetRandomNumber, so RAND_bytes (and therefore
 * ssh_get_random, ssh_pki_generate, and every kex) is seeded by the device's
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

#define MAX_AUTHORIZED_KEYS 32
#define PASSWORD_LEN 12

/* No i, l, o, 1, 0 or u: the password is read off a small screen and typed on a
 * different machine, so characters that blur together are a bug. */
static const char password_alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";

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

static void ensure_data_dir(void)
{
    /* Both levels, because ux0:data itself may not exist on a fresh card. */
    if (mkdir_quiet("ux0:data") != 0) {
        return;
    }
    mkdir_quiet(SSHD_DATA_SCE);
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

static int load_or_create_host_key(void)
{
    int rc = ssh_pki_import_privkey_file(SSHD_HOSTKEY_SCE, NULL, NULL, NULL,
                                         &host_key);

    if (rc == SSH_OK && host_key != NULL) {
        return 0;
    }
    host_key = NULL;
    ensure_data_dir();
    /* ssh_pki_generate() is deprecated in this libssh. Its replacement takes a
     * pki context, which carries the FIDO2/security-key enrolment options an
     * ed25519 host key has no use for — but it is still a required argument, so
     * one is created, used and dropped right here. */
    {
        ssh_pki_ctx ctx = ssh_pki_ctx_new();

        if (ctx == NULL) {
            return -1;
        }
        rc = ssh_pki_generate_key(SSH_KEYTYPE_ED25519, ctx, &host_key);
        ssh_pki_ctx_free(ctx);
    }
    if (rc != SSH_OK || host_key == NULL) {
        host_key = NULL;
        return -1;
    }
    /* Saving is best-effort on purpose. A device that cannot write ux0:data can
     * still serve a session; it just gets a new fingerprint every boot, which
     * the console shows so the client's key change warning is explicable. */
    if (ssh_pki_export_privkey_file(host_key, NULL, NULL, NULL,
                                    SSHD_HOSTKEY_SCE) != SSH_OK) {
        sh_console_log("warning: host key not saved (new fingerprint each boot)");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* authorized_keys                                                    */
/* ------------------------------------------------------------------ */

/*
 * Parsed here rather than with ssh_pki_import_pubkey_file() because that helper
 * reads one key per file and stops at the first syntax error. authorized_keys
 * files accumulate: a single malformed line (or a leftover from a rotated key)
 * must not cost you the three good ones above it, and it must not look like the
 * file was empty.
 */
static int load_authorized_keys(void)
{
    FILE *fp;
    char line[1024];

    fp = fopen(SSHD_KEYS_SCE, "r");
    if (fp == NULL) {
        return 0; /* no file: password mode, which is a state, not an error */
    }
    while (fgets(line, sizeof(line), fp) != NULL &&
           n_authorized < MAX_AUTHORIZED_KEYS) {
        char *algo, *b64, *save, *p;
        ssh_key key = NULL;
        enum ssh_keytypes_e type;

        p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }
        algo = strtok_r(p, " \t", &save);
        if (algo == NULL) {
            continue;
        }
        b64 = strtok_r(NULL, " \t", &save);
        if (b64 == NULL) {
            continue;
        }
        type = ssh_key_type_from_name(algo);
        if (type == SSH_KEYTYPE_UNKNOWN) {
            sh_console_log("authorized_keys: unknown algorithm %s (skipped)",
                           algo);
            continue;
        }
        if (ssh_pki_import_pubkey_base64(b64, type, &key) != SSH_OK ||
            key == NULL) {
            sh_console_log("authorized_keys: unparseable key for %s (skipped)",
                           algo);
            continue;
        }
        authorized[n_authorized++] = key;
    }
    fclose(fp);
    return n_authorized;
}

static void generate_password(char *dst, size_t dstlen)
{
    unsigned char raw[32];
    size_t i, room = dstlen - 1;
    size_t len = (PASSWORD_LEN < room) ? PASSWORD_LEN : room;

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
        dst[i] = password_alphabet[raw[i] % sizeof(password_alphabet) - 1];
    }
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* public interface                                                   */
/* ------------------------------------------------------------------ */

int sh_auth_prepare(sh_auth_t *auth)
{
    unsigned char probe[8];

    memset(&auth_state, 0, sizeof(auth_state));
    n_authorized = 0;

    if (ssh_get_random(probe, (int)sizeof(probe), 1) != SSH_OK) {
        sh_console_log("error: no entropy from the platform RNG");
        return -1;
    }
    if (load_or_create_host_key() != 0) {
        sh_console_log("error: could not obtain a host key");
        return -1;
    }
    if (record_fingerprint(host_key, auth_state.fingerprint,
                           sizeof(auth_state.fingerprint)) != 0) {
        snprintf(auth_state.fingerprint, sizeof(auth_state.fingerprint),
                 "(unavailable)");
    }
    snprintf(auth_state.key_path, sizeof(auth_state.key_path), "%s",
             SSHD_HOSTKEY_SCE);

    load_authorized_keys();
    auth_state.keys_loaded = n_authorized;
    if (n_authorized == 0) {
        generate_password(auth_state.password, sizeof(auth_state.password));
        auth_state.password_used = (auth_state.password[0] != '\0');
    }
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
