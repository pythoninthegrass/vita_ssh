#ifndef SSHD_H
#define SSHD_H

/*
 * Shared surface of the sshd app. The client app (src/main.c and friends)
 * shares nothing with these files: it is a libssh2 client with a vita2d
 * terminal, this is a libssh server with a builtin shell.
 *
 * The layering, bottom to top:
 *   sh_path.c        POSIX path -> "dev:" path, and the device table
 *   sshd_builtins.c  the command table; POSIX calls only, no libssh
 *   sshd_session.c   one SSH session: channel callbacks, line editor, dispatch
 *   sshd.c           net bring-up, host key, accept loop, poll loop, console
 *
 * Anything that reaches the filesystem goes through sh_path.c, and anything a
 * command prints goes out over its own channel via sh_printf. printf() here
 * would write to the debug screen instead of to the user.
 */

#include <stddef.h>

#include <libssh/libssh.h>

#include "sh_path.h"

#define SSHD_PORT_PRIMARY 22
#define SSHD_PORT_FALLBACK 2222
#define SSHD_MAX_SESSIONS 4
#define SSHD_LINE_MAX 512
#define SSHD_HISTORY 12
#define SSHD_COPY_BUFSIZE (64 * 1024)

/* Where the server keeps its own files -- host key, authorized_keys -- and the
 * directory a session lands in. That is a searched list rather than one path,
 * because which partition a homebrew app can write is not known until the device
 * says so; the table and the three search rules live in sshd_hostkey.c, and the
 * outcome of the search is reported through sh_auth_t below rather than
 * assumed.
 *
 * Those two files are opened by their syscall-spelled path directly rather than
 * through sh_path.c, which is the exception to the rule at the top of this file:
 * the path layer resolves against a session's cwd and home, and at auth time
 * there is no session yet. The chosen directory is handed back out through
 * sh_server_home_posix() so the shell and the server agree on one place. */

/* Opaque: the session struct lives in sshd_session.c. Commands talk to it
 * through the calls below and never see libssh types. */
typedef struct sh_session sh_session_t;

/* --- session (sshd_session.c) --- */

/* Allocates the session for an accepted connection and installs the server and
 * channel callbacks on it. peer is the client's address for the console; it is
 * copied. Returns NULL on allocation failure. */
sh_session_t *sh_session_new(ssh_session ssh, const char *peer);
void sh_session_free(sh_session_t *s);
ssh_session sh_session_ssh(const sh_session_t *s);

/* Runs any command line the client finished typing, then redraws the prompt.
 * Call this from the poll loop, once per session per iteration: executing from
 * the loop rather than from the data callback is what keeps one slow `du` from
 * starving every other session's socket. Returns 1 when the session should be
 * torn down (the client closed the channel or asked to exit). */
int sh_session_pump(sh_session_t *s);
int sh_session_finished(const sh_session_t *s);

void sh_printf(sh_session_t *s, const char *fmt, ...);
void sh_eprintf(sh_session_t *s, const char *fmt, ...);
void sh_write(sh_session_t *s, const char *data, size_t len);
void sh_prompt(sh_session_t *s);

void sh_session_get_fsctx(const sh_session_t *s, sh_fsctx_t *fs);
/* Only `cd` and the initial setup call these. The cwd is per session, never
 * libc's, so two people logged in cannot move each other's relative paths. */
void sh_session_set_cwd(sh_session_t *s, const char *posix);
void sh_session_request_exit(sh_session_t *s);
const char *sh_session_user(const sh_session_t *s);
const char *sh_session_peer(const sh_session_t *s);
int sh_session_cols(const sh_session_t *s);

/* Set by Ctrl-C at the line editor and by the close callbacks; long-running
 * commands (rm -r, cp -r, du) check it so a stuck walk is not a hung device. */
int sh_session_interrupted(const sh_session_t *s);

/* Runs one command line: tokenizing, quoting, wildcard expansion, dispatch.
 * Returns the exit status to report. */
int sh_execute_line(sh_session_t *s, const char *line);

/* --- commands (sshd_builtins.c) --- */

typedef struct {
    const char *name;
    int (*fn)(sh_session_t *s, int argc, char **argv);
    const char *usage;
} sh_builtin_t;

const sh_builtin_t *sh_builtins(size_t *count);
const sh_builtin_t *sh_builtin_lookup(const char *name);

/* --- host key and authentication (sshd_hostkey.c) --- */

typedef struct {
    char fingerprint[160]; /* SHA256:... as OpenSSH prints it */
    /* The two paths are in the syscall spelling ("ux0:/data/vitasshd/...") because
     * they are the paths the server actually opened, and a file manager takes the
     * same string with or without its slash. `home` is POSIX, because that is a
     * path a session types. */
    char key_path[SH_SCE_PATH_MAX];  /* host key on storage; "" if it is only in memory */
    char keys_path[SH_SCE_PATH_MAX]; /* the authorized_keys that was read; "" if none */
    char home[SH_POSIX_PATH_MAX];    /* where a session lands: the server's data dir */
    int keys_loaded;    /* authorized_keys entries accepted */
    int keys_rejected;  /* lines in that file that were not usable */
    int key_persisted;  /* the host key is on storage: same fingerprint next boot */
    int password_used;  /* no authorized_keys: a generated password is live */
    char password[24];  /* shown once, on the Vita's own screen */
} sh_auth_t;

/* Loads the host key -- read from wherever it is, else generate and save it
 * wherever will have it, else keep it in memory and say so -- then reads
 * authorized_keys from the first candidate directory holding one. Fills auth for
 * the console. Returns 0, or -1 only when no host key could be obtained at all,
 * which is the one condition that stops the server. */
int sh_auth_prepare(sh_auth_t *auth);

/* The single prepared state, for the session callbacks. */
const sh_auth_t *sh_server_auth(void);
ssh_key sh_server_host_key(void);
/* The directory a session starts in, in POSIX spelling: the one the server could
 * write, or the first that exists, or ux0's root. Valid once sh_auth_prepare()
 * has returned 0. */
const char *sh_server_home_posix(void);
/* The SSH_AUTH_METHOD_* mask to offer: public key when authorized_keys has
 * keys, password only when it does not and a generated password is live. */
int sh_server_auth_methods(void);

int sh_auth_check_pubkey(const sh_auth_t *auth, const char *user, ssh_key key);
int sh_auth_check_password(const sh_auth_t *auth, const char *user,
                           const char *password);

/* --- the Vita's own screen (console code in sshd.c) --- */

void sh_console_log(const char *fmt, ...);
/* Drawing the console is internal to sshd.c: it needs the vita2d font, the slot
 * table and the listening port, none of which belong in a shared header. The
 * console code therefore exposes only the log call above. */

#endif /* SSHD_H */
