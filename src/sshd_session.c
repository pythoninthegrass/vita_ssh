/*
 * One SSH session: authentication callbacks, the channel, the line editor, and
 * the hand-off to the builtin table.
 *
 * Two things here are not how a normal shell works, and both come from the
 * platform rather than from taste.
 *
 * The first is that everything runs in one thread on a single poll loop. There
 * is no fork, so a command runs inside the server. That means a command must
 * never be started from the channel data callback: a `du` over a large tree
 * would sit in that callback with the poll loop stopped, and every other
 * session's socket would go unread until it finished. So the editor only marks
 * a line complete, and sshd.c runs it from the loop via sh_session_pump().
 *
 * The second follows from that and is worth stating plainly rather than
 * hiding: a command already running cannot be interrupted with Ctrl-C, because
 * noticing the keystroke would require polling the socket, which is exactly
 * what the running command is blocking. sh_session_interrupted() therefore
 * reports state we already knew (the channel closed, `exit` was typed) and the
 * tree walks are bounded by their own depth and read limits instead.
 */
#include "sshd.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* callbacks.h declares ssh_server_callbacks_struct, ssh_channel_callbacks_struct,
 * ssh_callbacks_init() and the two setters below. server.h does not pull it in,
 * and without it both callback structs come back incomplete. */
#include <libssh/callbacks.h>
#include <libssh/server.h>

#define MAX_OUTPUT 2048
/* Kept small on purpose: the expansion buffer below is twice this and lives on
 * the stack, and a Vita thread's stack is whatever we asked sceKernelCreateThread
 * for, not the megabytes a desktop process gets. */
#define WRITE_CHUNK 4096

/* Keys the editor claims. Anything else is inserted. */
#define KEY_CTRL_C 0x03
#define KEY_CTRL_D 0x04
#define KEY_CTRL_L 0x0c
#define KEY_CTRL_W 0x17
#define KEY_CTRL_U 0x15
#define KEY_CR 0x0d
#define KEY_LF 0x0a
#define KEY_BS 0x08
#define KEY_DEL 0x7f
#define KEY_TAB 0x09
#define KEY_ESC 0x1b

struct sh_session {
    ssh_session ssh;
    ssh_channel channel;

    /* Embedded, not heap: libssh keeps the pointer and reads it on every
     * dispatch, so it has to outlive every callback that touches it. */
    struct ssh_server_callbacks_struct server_cbs;
    struct ssh_channel_callbacks_struct channel_cbs;

    char user[64];
    char peer[72];

    char cwd[SH_POSIX_PATH_MAX];
    char home[SH_POSIX_PATH_MAX];

    char line[SSHD_LINE_MAX];
    size_t linelen;

    char *history[SSHD_HISTORY];
    int hist_len;
    int hist_pos; /* SSHD_HISTORY means "not browsing" */

    int cols;
    int rows;
    int pty;
    int shell;
    int authenticated;
    int exit_requested;
    int finished;

    /* Set by the editor on Enter, consumed by sh_session_pump(). */
    int pending_ready;
    char pending[SSHD_LINE_MAX];

    /* Escape-sequence state so arrow keys and terminal reports never land in
     * the line: 0 normal, 1 after ESC, 2 after "ESC [", 3 after "ESC O". */
    int esc;
    int esc_len;
};

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

static void chan_write_raw(sh_session_t *s, const char *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        uint32_t want = (uint32_t)((len - off > WRITE_CHUNK) ? WRITE_CHUNK
                                                             : (len - off));
        int n = ssh_channel_write(s->channel, data + off, want);

        if (n <= 0) {
            /* The channel is gone. Dropping the rest is the only safe answer:
             * there is nobody left to report a short write to. */
            return;
        }
        off += (size_t)n;
    }
}

/*
 * A pty was requested but nobody emulates the termios behind it, so ONLCR is
 * ours to implement: without it every "\n" the builtins print draws a staircase
 * on the client. Done here rather than in sh_printf so `cat` of a text file is
 * fixed too, since that path bypasses the formatter entirely.
 */
static void chan_write(sh_session_t *s, const char *data, size_t len)
{
    char out[WRITE_CHUNK * 2];
    size_t off = 0;

    if (s == NULL || s->channel == NULL || s->finished || len == 0) {
        return;
    }
    if (!s->pty) {
        chan_write_raw(s, data, len);
        return;
    }
    while (off < len) {
        size_t n = 0;

        while (off < len && n < sizeof(out) / 2) {
            if (data[off] == '\n' && (off == 0 || data[off - 1] != '\r')) {
                /* Only in the second half, so the worst case (every byte a
                 * newline) still fits: 2 bytes out per byte in. */
                if (n + 1 >= sizeof(out) / 2) {
                    break;
                }
                out[n++] = '\r';
                out[n++] = '\n';
                off++;
                continue;
            }
            out[n++] = data[off++];
        }
        chan_write_raw(s, out, n);
        if (n == 0) {
            return; /* cannot happen with len > 0, but never spin */
        }
    }
}

void sh_write(sh_session_t *s, const char *data, size_t len)
{
    chan_write(s, data, len);
}

void sh_printf(sh_session_t *s, const char *fmt, ...)
{
    char stack[MAX_OUTPUT];
    va_list ap;
    int need;

    va_start(ap, fmt);
    need = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (need < 0) {
        return;
    }
    if ((size_t)need < sizeof(stack)) {
        chan_write(s, stack, (size_t)need);
        return;
    }
    {
        char *heap = malloc((size_t)need + 1);

        if (heap == NULL) {
            /* Fall back to the truncated stack copy: a partial line beats
             * silence, and the caller has no way to report a failed print. */
            chan_write(s, stack, sizeof(stack) - 1);
            return;
        }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)need + 1, fmt, ap);
        va_end(ap);
        chan_write(s, heap, (size_t)need);
        free(heap);
    }
}

void sh_eprintf(sh_session_t *s, const char *fmt, ...)
{
    char stack[MAX_OUTPUT];
    va_list ap;
    int need;

    va_start(ap, fmt);
    need = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (need < 0) {
        return;
    }
    if ((size_t)need < sizeof(stack)) {
        chan_write(s, stack, (size_t)need);
        return;
    }
    {
        char *heap = malloc((size_t)need + 1);

        if (heap == NULL) {
            chan_write(s, stack, sizeof(stack) - 1);
            return;
        }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)need + 1, fmt, ap);
        va_end(ap);
        chan_write(s, heap, (size_t)need);
        free(heap);
    }
}

/* ------------------------------------------------------------------ */
/* accessors                                                          */
/* ------------------------------------------------------------------ */

void sh_session_get_fsctx(const sh_session_t *s, sh_fsctx_t *fs)
{
    if (s == NULL || fs == NULL) {
        return;
    }
    fs->cwd = s->cwd;
    fs->home = s->home;
}

void sh_session_set_cwd(sh_session_t *s, const char *posix)
{
    if (s == NULL || posix == NULL) {
        return;
    }
    snprintf(s->cwd, sizeof(s->cwd), "%s", posix);
}

void sh_session_request_exit(sh_session_t *s)
{
    if (s != NULL) {
        s->exit_requested = 1;
    }
}

const char *sh_session_user(const sh_session_t *s)
{
    return (s == NULL || s->user[0] == '\0') ? "vita" : s->user;
}

const char *sh_session_peer(const sh_session_t *s)
{
    return (s == NULL || s->peer[0] == '\0') ? "unknown" : s->peer;
}

int sh_session_cols(const sh_session_t *s)
{
    if (s == NULL || s->cols < 20) {
        return 80; /* the width a client that never asked for a pty implies */
    }
    return s->cols;
}

int sh_session_interrupted(const sh_session_t *s)
{
    /* See the file header: this cannot see a Ctrl-C that arrives mid-command. */
    return s == NULL || s->exit_requested || s->finished;
}

int sh_session_finished(const sh_session_t *s)
{
    return s == NULL || s->finished;
}

ssh_session sh_session_ssh(const sh_session_t *s)
{
    return (s == NULL) ? NULL : s->ssh;
}

/* ------------------------------------------------------------------ */
/* prompt and history                                                 */
/* ------------------------------------------------------------------ */

/* ~/short for the prompt, the way every shell does it. */
static void prompt_path(const sh_session_t *s, char *dst, size_t dstlen)
{
    size_t hl = strlen(s->home);

    if (hl > 1 && strncmp(s->cwd, s->home, hl) == 0 &&
        (s->cwd[hl] == '/' || s->cwd[hl] == '\0')) {
        snprintf(dst, dstlen, "~%s", s->cwd + hl);
        return;
    }
    snprintf(dst, dstlen, "%s", s->cwd);
}

static void write_prompt(sh_session_t *s)
{
    char path[SH_POSIX_PATH_MAX];

    if (!s->shell) {
        return;
    }
    prompt_path(s, path, sizeof(path));
    if (s->pty) {
        sh_printf(s, "\033[1;32m%s@vita\033[0m:\033[1;34m%s\033[0m$ ",
                  sh_session_user(s), path);
    } else {
        sh_printf(s, "%s@vita:%s$ ", sh_session_user(s), path);
    }
}

void sh_prompt(sh_session_t *s)
{
    write_prompt(s);
}

static void redraw_line(sh_session_t *s)
{
    if (!s->pty) {
        return;
    }
    /* Carriage return, erase to end of line, then the whole line again: cheap
     * and it works without knowing the prompt's rendered width, which the
     * colour codes would otherwise make impossible. */
    sh_printf(s, "\r");
    write_prompt(s);
    sh_write(s, s->line, s->linelen);
    sh_printf(s, "\033[K");
}

static void history_push(sh_session_t *s, const char *line)
{
    if (line[0] == '\0') {
        return;
    }
    /* Consecutive duplicates are noise; non-adjacent ones are a person working
     * in a loop, so those stay. */
    if (s->hist_len > 0 && strcmp(s->history[s->hist_len - 1], line) == 0) {
        return;
    }
    if (s->hist_len == SSHD_HISTORY) {
        free(s->history[0]);
        memmove(&s->history[0], &s->history[1],
                (SSHD_HISTORY - 1) * sizeof(s->history[0]));
        s->hist_len--;
    }
    s->history[s->hist_len] = strdup(line);
    if (s->history[s->hist_len] != NULL) {
        s->hist_len++;
    }
}

static void recall_history(sh_session_t *s, int up)
{
    int next;

    if (s->hist_len == 0) {
        return;
    }
    if (s->hist_pos == SSHD_HISTORY) {
        if (!up) {
            return;
        }
        next = s->hist_len - 1;
    } else {
        next = up ? (s->hist_pos - 1) : (s->hist_pos + 1);
    }
    if (next < 0) {
        return;
    }
    if (next >= s->hist_len) {
        s->hist_pos = SSHD_HISTORY;
        s->linelen = 0;
        s->line[0] = '\0';
        redraw_line(s);
        return;
    }
    s->hist_pos = next;
    snprintf(s->line, sizeof(s->line), "%s", s->history[next]);
    s->linelen = strlen(s->line);
    redraw_line(s);
}

/* ------------------------------------------------------------------ */
/* tab completion                                                     */
/* ------------------------------------------------------------------ */

/* The word being completed, and the space it starts after. */
static void split_word(const sh_session_t *s, const char **dir_text,
                       const char **prefix)
{
    const char *slash = strrchr(s->line, '/');
    const char *space = strrchr(s->line, ' ');

    if (slash != NULL && (space == NULL || slash > space)) {
        size_t dirlen = (size_t)(slash - s->line);

        /* The directory is printed back verbatim on a second Tab, so it lives
         * in a static rather than on the stack. */
        static char dir[SH_POSIX_PATH_MAX];

        if (dirlen == 0) {
            snprintf(dir, sizeof(dir), "/");
        } else {
            memcpy(dir, s->line, dirlen);
            dir[dirlen] = '\0';
        }
        *dir_text = dir;
        *prefix = slash + 1;
        return;
    }
    *dir_text = NULL; /* complete against the cwd */
    *prefix = (space == NULL) ? s->line : space + 1;
}

static void insert_completion(sh_session_t *s, const char *name, int is_dir)
{
    size_t extra = strlen(name) + (is_dir ? 1u : 0u);

    if (s->linelen + extra + 1 > sizeof(s->line)) {
        return; /* would not fit: leave the line exactly as it was */
    }
    s->linelen += strlen(name);
    sh_write(s, name, strlen(name));
    if (is_dir) {
        s->line[s->linelen++] = '/';
        sh_write(s, "/", 1);
    }
    s->line[s->linelen] = '\0';
}

static void complete_word(sh_session_t *s)
{
    const char *dir_text, *prefix;
    size_t plen;

    split_word(s, &dir_text, &prefix);
    plen = strlen(prefix);

    /* First word: complete a command name, which is the completion people
     * actually reach for on a box with 21 commands. */
    if (dir_text == NULL && strchr(s->line, ' ') == NULL) {
        const sh_builtin_t *table;
        size_t count, i;
        const char *only = NULL;
        int n = 0;

        table = sh_builtins(&count);
        for (i = 0; i < count; i++) {
            if (strncmp(table[i].name, prefix, plen) == 0) {
                only = table[i].name;
                n++;
            }
        }
        if (n == 1) {
            insert_completion(s, only + plen, 0);
            if (s->linelen + 2 <= sizeof(s->line)) {
                s->line[s->linelen++] = ' ';
                s->line[s->linelen] = '\0';
                sh_write(s, " ", 1);
            }
        } else if (n > 1) {
            sh_printf(s, "\n");
            for (i = 0; i < count; i++) {
                if (strncmp(table[i].name, prefix, plen) == 0) {
                    sh_printf(s, "%s  ", table[i].name);
                }
            }
            sh_printf(s, "\n");
            write_prompt(s);
            sh_write(s, s->line, s->linelen);
        }
        return;
    }

    {
        sh_fsctx_t fs;
        char sce[SH_SCE_PATH_MAX];
        DIR *dir;
        struct dirent *ent;
        char **names;
        int n = 0, cap = 128, i;
        const char *only = NULL;

        sh_session_get_fsctx(s, &fs);
        if (dir_text == NULL) {
            if (sh_path_sce(&fs, NULL, sce, sizeof(sce)) != 0) {
                return;
            }
        } else if (sh_path_sce(&fs, dir_text, sce, sizeof(sce)) != 0) {
            return;
        }
        names = malloc((size_t)cap * sizeof(*names));
        if (names == NULL) {
            return;
        }
        dir = opendir(sce);
        if (dir == NULL) {
            free(names);
            return;
        }
        while ((ent = readdir(dir)) != NULL && n < cap) {
            if (plen == 0 ? ent->d_name[0] == '.'
                          : strncmp(ent->d_name, prefix, plen) != 0) {
                continue;
            }
            names[n] = strdup(ent->d_name);
            if (names[n] == NULL) {
                break;
            }
            only = names[n];
            n++;
        }
        closedir(dir);

        if (n == 1) {
            char *tail = strdup(only + plen);
            struct stat st;
            char probe[SH_SCE_PATH_MAX];
            int is_dir = 0;

            /* Is it a directory? Ask the filesystem, so the completion can add
             * the slash and save the next keystroke. */
            if (sh_join(probe, sizeof(probe), sce, only) == 0) {
                if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode)) {
                    is_dir = 1;
                }
            }
            if (tail != NULL) {
                insert_completion(s, tail, is_dir);
                free(tail);
            }
        } else if (n > 1) {
            sh_printf(s, "\n");
            for (i = 0; i < n; i++) {
                sh_printf(s, "%s  ", names[i] + plen);
            }
            sh_printf(s, "\n");
            write_prompt(s);
            sh_write(s, s->line, s->linelen);
        }
        for (i = 0; i < n; i++) {
            free(names[i]);
        }
        free(names);
    }
}

/* ------------------------------------------------------------------ */
/* line editor                                                        */
/* ------------------------------------------------------------------ */

static void finish_line(sh_session_t *s)
{
    sh_printf(s, "\n");
    snprintf(s->pending, sizeof(s->pending), "%s", s->line);
    s->pending_ready = 1;
    history_push(s, s->line);
    s->linelen = 0;
    s->line[0] = '\0';
    s->hist_pos = SSHD_HISTORY;
}

static void erase_char(sh_session_t *s)
{
    if (s->linelen == 0) {
        return;
    }
    s->linelen--;
    s->line[s->linelen] = '\0';
    if (s->pty) {
        sh_write(s, "\b \b", 3);
    }
}

static void erase_word(sh_session_t *s)
{
    while (s->linelen > 0 && s->line[s->linelen - 1] == ' ') {
        erase_char(s);
    }
    while (s->linelen > 0 && s->line[s->linelen - 1] != ' ') {
        erase_char(s);
    }
}

static void insert_char(sh_session_t *s, unsigned char c)
{
    if (s->linelen + 1 >= sizeof(s->line)) {
        return; /* the line is full; a beep nobody hears helps no one */
    }
    s->line[s->linelen++] = (char)c;
    s->line[s->linelen] = '\0';
    if (s->pty) {
        sh_write(s, (const char *)&c, 1);
    }
}

/* Returns 1 if the byte was consumed as part of an escape sequence. */
static int feed_escape(sh_session_t *s, unsigned char c)
{
    if (s->esc == 0) {
        return 0;
    }
    if (s->esc == 3) { /* ESC O <final>: one function key, drop it */
        s->esc = 0;
        s->esc_len = 0;
        return 1;
    }
    if (s->esc == 1) {
        if (c == '[') {
            s->esc = 2;
            s->esc_len = 0;
        } else if (c == 'O') {
            s->esc = 3;
        } else {
            s->esc = 0; /* ESC <x>: some other two-byte sequence */
        }
        return 1;
    }
    /* In a CSI: parameters and intermediates until a final byte in @ through ~.
     * esc_len bounds how long we will sit in this state, so a truncated
     * sequence from a client that died mid-report cannot swallow the next
     * thing the user types. */
    if (++s->esc_len > 32) {
        s->esc = 0;
        s->esc_len = 0;
        return 1;
    }
    if (c >= '@' && c <= '~') {
        s->esc = 0;
        s->esc_len = 0;
        if (c == 'A') {
            recall_history(s, 1);
        } else if (c == 'B') {
            recall_history(s, 0);
        }
        /* C/D (arrows), and every report we did not ask for, are dropped: the
         * editor is append-only, so there is nothing to move. */
    }
    return 1;
}

static void feed_byte(sh_session_t *s, unsigned char c)
{
    if (feed_escape(s, c)) {
        return;
    }
    if (c == KEY_ESC) {
        s->esc = 1;
        return;
    }
    switch (c) {
    case KEY_CR:
    case KEY_LF:
        finish_line(s);
        return;
    case KEY_BS:
    case KEY_DEL:
        erase_char(s);
        return;
    case KEY_CTRL_C:
        sh_printf(s, "^C\n");
        s->linelen = 0;
        s->line[0] = '\0';
        s->hist_pos = SSHD_HISTORY;
        write_prompt(s);
        return;
    case KEY_CTRL_D:
        if (s->linelen == 0) {
            s->exit_requested = 1; /* EOF on an empty line, as every shell does */
        }
        return;
    case KEY_CTRL_U:
        while (s->linelen > 0) {
            erase_char(s);
        }
        return;
    case KEY_CTRL_W:
        erase_word(s);
        return;
    case KEY_CTRL_L:
        if (s->pty) {
            sh_write(s, "\033[2J\033[H", 7);
            write_prompt(s);
            sh_write(s, s->line, s->linelen);
        }
        return;
    case KEY_TAB:
        complete_word(s);
        return;
    default:
        if (c < 0x20) {
            return; /* unclaimed control character: ignore rather than store */
        }
        insert_char(s, c);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* channel callbacks                                                  */
/* ------------------------------------------------------------------ */

static int on_channel_data(ssh_session session, ssh_channel channel,
                           void *data, uint32_t len, int is_stderr,
                           void *userdata)
{
    sh_session_t *s = userdata;
    unsigned char *p = data;
    uint32_t i;

    (void)session;
    (void)channel;
    (void)is_stderr;
    if (s == NULL) {
        return (int)len;
    }
    for (i = 0; i < len; i++) {
        feed_byte(s, p[i]);
    }
    /* Return what we took: anything left over is re-delivered on the next poll,
     * and the editor consumes everything, so claiming the whole buffer is the
     * truthful answer. */
    return (int)len;
}

static int on_pty_request(ssh_session session, ssh_channel channel,
                          const char *term, int cols, int rows, int pxw,
                          int pxh, void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    (void)pxw;
    (void)pxh;
    if (s == NULL) {
        return 1;
    }
    s->pty = 1;
    s->cols = cols;
    s->rows = rows;
    sh_console_log("pty %s %dx%d from %s", term ? term : "?", cols, rows,
                   sh_session_peer(s));
    return 0; /* 0 accepts; nonzero makes libssh send the failure */
}

static void write_banner(sh_session_t *s)
{
    sh_printf(s, "VitaSSH -- PS Vita shell\r\n");
    sh_printf(s, "Connected from %s as %s.\r\n", sh_session_peer(s),
              sh_session_user(s));
    sh_printf(s, "`ls /` lists the devices, `help` lists the commands.\r\n");
}

static int on_shell_request(ssh_session session, ssh_channel channel,
                            void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    if (s == NULL) {
        return 1;
    }
    s->shell = 1;
    write_banner(s);
    write_prompt(s);
    return 0;
}

static int on_exec_request(ssh_session session, ssh_channel channel,
                           const char *command, void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    if (s == NULL || command == NULL) {
        return 1;
    }
    /* `ssh vita 'ls /ux0/vita'` has to work or the server is only good for an
     * interactive terminal. It runs from the pump, like a typed line, so the
     * poll loop stays in charge; the exit status goes back when it finishes. */
    snprintf(s->pending, sizeof(s->pending), "%s", command);
    s->pending_ready = 1;
    s->shell = 0;
    sh_console_log("exec \"%s\" from %s", command, sh_session_peer(s));
    return 0;
}

static int on_subsystem_request(ssh_session session, ssh_channel channel,
                                const char *what, void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    if (s == NULL || what == NULL) {
        return 1;
    }
    sh_console_log("subsystem \"%s\" from %s", what, sh_session_peer(s));
    /* Rejected until sshd_sftp.c wires sftp_server_new() in here; nonzero
     * makes libssh send the channel failure so scp and sftp fail fast with a
     * clear message instead of hanging on an accepted subsystem. */
    sh_eprintf(s, "%s: subsystem not supported\n", what);
    return 1;
}

/* Both are void-returning: ssh_channel_close_callback and
 * ssh_channel_eof_callback have no status for libssh to act on, the channel
 * being already gone. Each flags the session so the poll loop tears it down on
 * its next pass rather than doing the teardown from inside the callback. */
static void on_channel_close(ssh_session session, ssh_channel channel,
                             void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    if (s != NULL) {
        s->finished = 1;
    }
}

static void on_channel_eof(ssh_session session, ssh_channel channel,
                           void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    if (s != NULL) {
        s->finished = 1;
    }
}

static ssh_channel on_open_session(ssh_session session, void *userdata)
{
    sh_session_t *s = userdata;
    ssh_channel channel;

    if (s == NULL || session == NULL) {
        return NULL;
    }
    if (s->channel != NULL) {
        /* One channel per session: this shell has no multiplexing to offer, and
         * returning NULL makes libssh refuse the second open. */
        return NULL;
    }
    channel = ssh_channel_new(session);
    if (channel == NULL) {
        return NULL;
    }
    s->channel = channel;
    /* This libssh has no ssh_channel_set_callbacks_userdata(): the userdata the
     * callbacks receive is whatever channel_cbs.userdata already holds, and
     * sh_session_new() set that when it filled in the callback struct. So the
     * order is what matters — the struct is complete before it is registered. */
    ssh_set_channel_callbacks(channel, &s->channel_cbs);
    return channel;
}

/* ------------------------------------------------------------------ */
/* server callbacks                                                   */
/* ------------------------------------------------------------------ */

static void remember_user(sh_session_t *s, const char *user)
{
    if (user != NULL && user[0] != '\0') {
        snprintf(s->user, sizeof(s->user), "%s", user);
    }
}

static int on_auth_password(ssh_session session, const char *user,
                            const char *password, void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    if (s == NULL) {
        return SSH_AUTH_ERROR;
    }
    if (sh_auth_check_password(sh_server_auth(), user, password) == 0) {
        remember_user(s, user);
        s->authenticated = 1;
        sh_console_log("password login: %s@%s", user ? user : "?",
                       sh_session_peer(s));
        return SSH_AUTH_SUCCESS;
    }
    sh_console_log("password rejected: %s@%s", user ? user : "?",
                   sh_session_peer(s));
    return SSH_AUTH_DENIED;
}

static int on_auth_pubkey(ssh_session session, const char *user,
                          struct ssh_key_struct *pubkey, char signature_state,
                          void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    if (s == NULL) {
        return SSH_AUTH_ERROR;
    }
    /*
     * libssh calls this twice per offered key: once with STATE_NONE for the
     * probe ("would you accept this key?"), then again after checking the
     * signature. Answering the probe honestly is worth the extra branch -- if we
     * said yes to every key the way the shipped sample does, the client would
     * sign with a key this server was never going to accept and then fail,
     * instead of moving on to the next identity.
     */
    if (signature_state != SSH_PUBLICKEY_STATE_NONE &&
        signature_state != SSH_PUBLICKEY_STATE_VALID) {
        sh_console_log("publickey rejected (bad signature): %s@%s",
                       user ? user : "?", sh_session_peer(s));
        return SSH_AUTH_DENIED;
    }
    if (sh_auth_check_pubkey(sh_server_auth(), user, pubkey) == 0) {
        if (signature_state == SSH_PUBLICKEY_STATE_VALID) {
            remember_user(s, user);
            s->authenticated = 1;
            sh_console_log("publickey login: %s@%s", user ? user : "?",
                           sh_session_peer(s));
        }
        return SSH_AUTH_SUCCESS;
    }
    sh_console_log("publickey rejected: %s@%s", user ? user : "?",
                   sh_session_peer(s));
    return SSH_AUTH_DENIED;
}

static int on_service_request(ssh_session session, const char *service,
                              void *userdata)
{
    (void)session;
    (void)userdata;
    /* Only "ssh-connection" is meaningful here and there is nothing to
     * configure per service, so accept it and let the channel requests decide
     * what a session may actually do. */
    return (service != NULL && strcmp(service, "ssh-connection") == 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

sh_session_t *sh_session_new(ssh_session ssh, const char *peer)
{
    sh_session_t *s = calloc(1, sizeof(*s));

    if (s == NULL) {
        return NULL;
    }
    s->ssh = ssh;
    s->hist_pos = SSHD_HISTORY;
    s->cols = 80;
    s->rows = 24;
    snprintf(s->peer, sizeof(s->peer), "%s", peer ? peer : "unknown");
    /* The server's data directory, whichever one of the candidates it turned out
     * to be: ~ is where the user's authorized_keys lives, so it has to be the
     * place the server itself could write. */
    snprintf(s->home, sizeof(s->home), "%s", sh_server_home_posix());
    snprintf(s->cwd, sizeof(s->cwd), "%s", sh_server_home_posix());

    ssh_callbacks_init(&s->server_cbs);
    s->server_cbs.userdata = s;
    s->server_cbs.auth_password_function = on_auth_password;
    s->server_cbs.auth_pubkey_function = on_auth_pubkey;
    s->server_cbs.service_request_function = on_service_request;
    s->server_cbs.channel_open_request_session_function = on_open_session;

    ssh_callbacks_init(&s->channel_cbs);
    s->channel_cbs.userdata = s;
    s->channel_cbs.channel_data_function = on_channel_data;
    s->channel_cbs.channel_pty_request_function = on_pty_request;
    s->channel_cbs.channel_shell_request_function = on_shell_request;
    s->channel_cbs.channel_exec_request_function = on_exec_request;
    s->channel_cbs.channel_subsystem_request_function = on_subsystem_request;
    s->channel_cbs.channel_close_function = on_channel_close;
    s->channel_cbs.channel_eof_function = on_channel_eof;

    if (ssh_set_server_callbacks(ssh, &s->server_cbs) != SSH_OK) {
        free(s);
        return NULL;
    }
    /* The auth methods to offer are a policy the host key layer decides: with
     * authorized_keys present, password authentication is still allowed only
     * when a generated password is also live. */
    ssh_set_auth_methods(ssh, sh_server_auth_methods());
    return s;
}

void sh_session_free(sh_session_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->hist_len; i++) {
        free(s->history[i]);
    }
    free(s);
}

int sh_session_pump(sh_session_t *s)
{
    if (s == NULL) {
        return 1;
    }
    if (s->exit_requested) {
        if (s->channel != NULL) {
            ssh_channel_request_send_exit_status(s->channel, 0);
            ssh_channel_send_eof(s->channel);
            ssh_channel_close(s->channel);
        }
        s->finished = 1;
        return 1;
    }
    if (!s->pending_ready) {
        return 0;
    }
    {
        char line[SSHD_LINE_MAX];

        snprintf(line, sizeof(line), "%s", s->pending);
        s->pending_ready = 0;
        s->pending[0] = '\0';
        s->linelen = 0;
        s->line[0] = '\0';
        {
            int status = sh_execute_line(s, line);

            if (!s->shell) {
                /* exec mode: the client asked for one command, so report its
                 * status and hang up the way sshd would. */
                ssh_channel_request_send_exit_status(s->channel, status);
                ssh_channel_send_eof(s->channel);
                s->finished = 1;
                return 1;
            }
        }
    }
    if (s->exit_requested || s->finished) {
        return 1;
    }
    write_prompt(s);
    return 0;
}
