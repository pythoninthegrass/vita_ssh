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

#include "sh_alias.h"
#include "sh_env.h"
#include "sh_path.h"
#include "sh_stdin.h"

#define SSHD_PORT_PRIMARY 22
#define SSHD_PORT_FALLBACK 2222
/* Four concurrent sessions is a hardware limit, not a preference: every slot is a
 * libssh session with its own buffers and channel, all driven from one poll loop on
 * a handheld with no swap. Raising it is a memory question, not an edit to this
 * number -- what it costs has to be measured on a device first. */
#define SSHD_MAX_SESSIONS 4
/* Room for a remote command that carries what authorized_keys can: an ssh-rsa
 * 8192-bit key line is about 1.6 KB, and `ssh host "echo KEY > path"` has to
 * hold the key AND the path in one line. The interactive editor shares the
 * size. A command longer than this is REJECTED, never truncated -- a silent
 * `rm -r /ux0/data/vita` with the tail missing is how a cleanup command
 * becomes a disaster. */
#define SSHD_LINE_MAX 4096
/* Per-connection history: what `history` lists and what `!n` recalls. Heap
 * copies behind the pointers, so this is pointer slots, not text. Enough to
 * retrace a working session; not bash's 500, because every entry is a strdup
 * on a device whose spare memory is the OS's too. */
#define SSHD_HISTORY 32
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

/* Redirects what sh_write() puts out into sce_path for one command -- the
 * `command > file` and `command >> file` forms. sce_path is a resolved device
 * path, the same string a builtin would open() for reading. Returns 0, or -1
 * with errno set for the caller to report. Every path that calls _start calls
 * _end, which flushes and closes. sh_eprintf keeps going to the terminal:
 * `cmd > file` redirects stdout, and errors belong where the user is. */
int sh_session_redirect_start(sh_session_t *s, const char *sce_path,
                              int append);
void sh_session_redirect_end(sh_session_t *s);

/* The largest source `command < file` will take as stdin, and the most one pipeline
 * segment can hand the next. */
#define SSHD_STDIN_MAX (1024 * 1024)

/* Gives one command everything rd produces as stdin -- the `command < file` form.
 * Returns 0, or -1 with errno set for the caller to report (EFBIG past
 * SSHD_STDIN_MAX bytes). Every path that calls _start calls _end. A session with no
 * source reads as an empty file, so a reader never waits. */
int sh_session_stdin_start(sh_session_t *s, sh_stdin_reader_t rd, void *ctx);
size_t sh_session_stdin_read(sh_session_t *s, char *buf, size_t n);
void sh_session_stdin_end(sh_session_t *s);

/* Holds what sh_write() puts out instead of sending it, for one pipeline segment; a `>`
 * redirect still wins. _end makes the held bytes the session's stdin and returns 0, or
 * returns -1 (stdin left empty) when more than SSHD_STDIN_MAX bytes came out. Every
 * path that calls _start calls _end. sh_eprintf is not captured. */
void sh_session_capture_start(sh_session_t *s);
int sh_session_capture_end(sh_session_t *s);

void sh_prompt(sh_session_t *s);

void sh_session_get_fsctx(const sh_session_t *s, sh_fsctx_t *fs);
/* Only `cd` and the initial setup call these. The cwd is per session, never
 * libc's, so two people logged in cannot move each other's relative paths. */
void sh_session_set_cwd(sh_session_t *s, const char *posix);
/* This session's aliases, never shared with another session; NULL if memory was short at setup. */
sh_alias_table_t *sh_session_aliases(const sh_session_t *s);
/* This session's variables, never shared with another session; NULL if memory was short at
 * setup. USER, LOGNAME, TERM, HOME and the rest live here; PWD is not stored and is read from
 * the session's cwd when printed, so it cannot disagree with `cd`. */
sh_env_table_t *sh_session_env(const sh_session_t *s);
void sh_session_request_exit(sh_session_t *s);
const char *sh_session_user(const sh_session_t *s);
const char *sh_session_peer(const sh_session_t *s);
/* Nonzero once a password or key login has succeeded. */
int sh_session_authenticated(const sh_session_t *s);
/* Why the session ended, for the log line; "no reason recorded" if nothing set one. */
const char *sh_session_end_reason(const sh_session_t *s);
int sh_session_cols(const sh_session_t *s);

/* Copies up to max history entries into entries, oldest first, and returns how
 * many: the `history` builtin numbers them 1..n from here, and `!n` in the
 * editor indexes the same way. Pointers stay valid until the session's next
 * history push. */
int sh_session_history(sh_session_t *s, const char **entries, int max);

/* Set by Ctrl-C at the line editor and by the close callbacks; long-running
 * commands (rm -r, cp -r, du) check it so a stuck walk is not a hung device. */
int sh_session_interrupted(const sh_session_t *s);

/* Called per entry by a long walk: runs a poll slice at most every 100 ms so
 * other sessions can connect and be read, and only from a command run by
 * sh_session_pump(); from anywhere else it does nothing. */
void sh_session_yield(sh_session_t *s);

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

/* --- package install (sshd_vpk.c) --- */

/* Both take the package in the syscall spelling plus the name the user typed, for
 * messages, and return a shell exit status. list never writes. */
int sh_vpk_list(sh_session_t *s, const char *sce, const char *name);
int sh_vpk_install(sh_session_t *s, const char *sce, const char *name, int replace);
int sh_vpk_pending(void);
void sh_vpk_handoff(void);

/* --- host key and authentication (sshd_hostkey.c) --- */

typedef struct {
    char fingerprint[160]; /* SHA256:... as OpenSSH prints it */
    /* The two paths are in the syscall spelling ("ux0:/data/vitasshd/...") because
     * they are the paths the server actually opened, and a file manager takes the
     * same string with or without its slash. `home` is POSIX, because that is a
     * path a session types. */
    char key_path[SH_SCE_PATH_MAX];  /* host key on storage; "" if it is only in memory */
    char keys_path[SH_SCE_PATH_MAX]; /* the authorized_keys that was read; "" if none */
    char home[SH_POSIX_PATH_MAX];    /* where a session lands: the user's home, <device>/data/home */
    int keys_loaded;    /* authorized_keys entries accepted */
    int keys_rejected;  /* lines in that file that were not usable */
    int key_persisted;  /* the host key is on storage: same fingerprint next boot */
    int password_used;  /* no authorized_keys: a generated passphrase is live */
    char password[64];  /* PASSPHRASE_WORDS words, no separators; shown once, on the
                         * Vita's own screen. Comfortably fits the longest words the
                         * table can produce -- scripts/gen_wordlist.py --check is
                         * what proves that, since the runtime guard can only fail
                         * closed on an empty string. */
} sh_auth_t;

/* Loads the host key -- read from wherever it is, else generate and save it
 * wherever will have it, else keep it in memory and say so -- then reads
 * authorized_keys from the first candidate directory holding one. Fills auth for
 * the console. Returns 0, or -1 only when no host key could be obtained at all,
 * which is the one condition that stops the server. */
int sh_auth_prepare(sh_auth_t *auth);

/* The single prepared state, for the session callbacks. */
const sh_auth_t *sh_server_auth(void);

/* Re-reads authorized_keys when it changed on disk and returns 1 when it did.
 * Called once per connection at session setup: the exec that installs the
 * first key runs on a server that booted before the file existed. */
int sh_auth_reload_if_changed(void);
/* Copies the live auth state for drawing; the console must show a key that was
 * installed after boot. */
void sh_auth_snapshot(sh_auth_t *out);
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

/* --- the poll loop (sshd.c) --- */

/* Runs one slice of the server's poll event, waiting at most ms. A command that
 * has to wait for a client (an exec's output with the client's window shut) calls
 * this instead of blocking inside libssh, so every session's socket keeps being
 * read. Only from a command run by sh_session_pump(), never from a libssh
 * callback: the poll loop is not reentrant from inside its own callbacks. */
void sh_server_poll_slice(int ms);

/* --- the Vita's own screen (console code in sshd.c) --- */

void sh_console_log(const char *fmt, ...);

/* --- the log file (storage code in sshd_hostkey.c) --- */

/* Mirrors every console line into <candidate>/sshd.log, in the first candidate
 * directory that can be created and written, so a crash leaves something a file
 * manager or a USB mount can read: the console is a ring buffer in RAM and a
 * crash takes it. Open it before anything else can fail. Returns 0 when the log
 * is open, -1 when no directory would take it -- which never stops the boot. */
int sh_log_open(void);
void sh_log_line(const char *line);
const char *sh_log_path(void); /* "" when there is no log */
void sh_log_close(void);

/* Drawing the console is internal to sshd.c: it needs the vita2d font, the slot
 * table and the listening port, none of which belong in a shared header. The
 * console code therefore exposes only the log call above. */

#endif /* SSHD_H */
