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

/* Where the server keeps its own state. ux0:data is the one place a homebrew
 * app may write that the user can also see from a file manager. */
#define SSHD_DATA_SCE "ux0:data/vitasshd"
#define SSHD_DATA_POSIX "/ux0/data/vitasshd"
#define SSHD_HOSTKEY_SCE SSHD_DATA_SCE "/ssh_host_ed25519_key"
#define SSHD_HOSTKEY_PUB_SCE SSHD_HOSTKEY_SCE ".pub"
#define SSHD_KEYS_SCE SSHD_DATA_SCE "/authorized_keys"
#define SSHD_KEYS_POSIX SSHD_DATA_POSIX "/authorized_keys"

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
    char key_path[SH_SCE_PATH_MAX];
    int keys_loaded;       /* authorized_keys entries accepted */
    int password_used;     /* no authorized_keys: a generated password is live */
    char password[24];     /* shown once, on the Vita's own screen */
} sh_auth_t;

/* Loads the host key, generating and saving an Ed25519 one if absent, then
 * reads authorized_keys. Fills auth for the console. Returns 0 or -1. */
int sh_auth_prepare(sh_auth_t *auth);

/* The single prepared state, for the session callbacks. */
const sh_auth_t *sh_server_auth(void);
ssh_key sh_server_host_key(void);
/* The SSH_AUTH_METHOD_* mask to offer: public key when authorized_keys has
 * keys, password only when it does not and a generated password is live. */
int sh_server_auth_methods(void);

int sh_auth_check_pubkey(const sh_auth_t *auth, const char *user, ssh_key key);
int sh_auth_check_password(const sh_auth_t *auth, const char *user,
                           const char *password);

/* --- the Vita's own screen (console code in sshd.c) --- */

void sh_console_log(const char *fmt, ...);
void sh_console_draw(struct ssh_bind_s *bind, const sh_auth_t *auth,
                     int sessions, const char *ip, int port);

#endif /* SSHD_H */
