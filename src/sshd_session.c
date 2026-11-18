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
#include "sftp_server.h"
#include "sh_defer.h"
#include "sh_lineq.h"
#include "sshd.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <psp2/kernel/processmgr.h>

/* callbacks.h declares ssh_server_callbacks_struct, ssh_channel_callbacks_struct,
 * ssh_callbacks_init() and the two setters below. server.h does not pull it in,
 * and without it both callback structs come back incomplete. */
#include <libssh/callbacks.h>
#include <libssh/server.h>

#define MAX_OUTPUT 2048
/* Seconds an sftp channel may take to answer our close before the session is dropped. */
#define SFTP_CLOSE_WAIT_S 3
/* Kept small on purpose: the expansion buffer below is twice this and lives on
 * the stack, and a Vita thread's stack is whatever we asked sceKernelCreateThread
 * for, not the megabytes a desktop process gets. */
#define WRITE_CHUNK 4096
/* Seconds a command's output may wait on a client that keeps its window shut
 * before the session is dropped. */
#define WINDOW_STALL_S 30
/* Length of one poll slice while waiting for the window to reopen. */
#define WINDOW_POLL_MS 50
/* Minimum gap between the poll slices a long walk runs for the other sessions. */
#define YIELD_INTERVAL_US (100 * 1000)

/* This newlib ships no NAME_MAX (sshd_builtins.c defines the same value); 255
 * is the POSIX minimum and FAT's own limit for long names. */
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* Keys the editor claims. Anything else is inserted. */
#define KEY_CTRL_A 0x01
#define KEY_CTRL_C 0x03
#define KEY_CTRL_D 0x04
#define KEY_CTRL_E 0x05
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
    sh_sftp_t *sftp; /* non-NULL once the sftp subsystem was accepted */

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
    size_t cursor; /* insertion point within line, 0..linelen */

    sh_alias_table_t *aliases;
    sh_env_table_t *env;

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
    const char *end_reason; /* why the session ended, first cause wins; NULL until it does */
    int in_pump; /* set while sh_session_pump() runs a command: only then may a write wait on the poll loop */
    int in_cb; /* set while on_channel_data() feeds the editor: a libssh callback, so no write may wait on the poll loop */
    sh_defer_t defer; /* what the editor wrote from a callback while a command ran; sh_session_pump() sends it when the command ends */
    time_t sftp_closing; /* when the sftp channel close was started, 0 before */
    unsigned long long last_yield_us; /* system time of the last sh_session_yield() slice */

    /* Completed lines, pushed by the editor on Enter (or by exec) and popped by sh_session_pump(). */
    sh_lineq_t lineq;
    int after_cr; /* the previous byte was CR, so an LF now is the tail of a CRLF pair */
    int eof_seen; /* the client sent EOF with lines still queued: finish once they have run */

    /* While set, sh_write() lands here instead of the channel: `command > file`
     * and `command >> file`, opened and closed by sh_execute_line around one
     * command. sh_eprintf ignores this, on purpose: `cmd > file` redirects
     * stdout, and errors belong on the terminal. */
    FILE *out_fp;

    /* What a command reads as stdin while `command < file` runs; empty otherwise. */
    sh_stdin_t in;

    /* While capturing, sh_write() lands here, so the next pipeline segment can read it as
     * stdin. A `>` redirect is checked first and wins; cap_over records that the bound broke. */
    sh_stdin_t cap;
    int capturing;
    int cap_over;

    /* Escape-sequence state so arrow keys and terminal reports never land in
     * the line: 0 normal, 1 after ESC, 2 after "ESC [", 3 after "ESC O". */
    int esc;
    int esc_len;
};

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

/* libssh must hold no unsent bytes (its blocking flush has no timeout) and, with need_window, the window must be open. */
static int output_has_room(const sh_session_t *s, int need_window)
{
    return (!need_window || ssh_channel_window_size(s->channel) > 0) &&
           (ssh_get_status(s->ssh) & SSH_WRITE_PENDING) == 0;
}

/* Records why a session ended; the first cause wins because later ones are fallout of it. */
static void note_end(sh_session_t *s, const char *why)
{
    if (s != NULL && s->end_reason == NULL) {
        s->end_reason = why;
    }
}

/* Polls every session until output_has_room(); -1 if the client is gone or stalled for WINDOW_STALL_S, with the session marked finished. */
static int wait_for_room(sh_session_t *s, int need_window)
{
    time_t start = time(NULL);

    while (!output_has_room(s, need_window)) {
        if (s->finished ||
            (ssh_get_status(s->ssh) & (SSH_CLOSED | SSH_CLOSED_ERROR)) != 0) {
            note_end(s, "transport closed while output waited for room");
            s->finished = 1;
            return -1;
        }
        if (time(NULL) - start >= WINDOW_STALL_S) {
            sh_console_log("%s stopped reading for %d s; dropping the session",
                           sh_session_peer(s), WINDOW_STALL_S);
            s->finished = 1;
            return -1;
        }
        sh_server_poll_slice(WINDOW_POLL_MS);
    }
    return 0;
}

static void chan_write_raw(sh_session_t *s, const char *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        uint32_t want = (uint32_t)((len - off > WRITE_CHUNK) ? WRITE_CHUNK
                                                             : (len - off));
        int n;

        if (s->in_pump) {
            /* Never hand libssh more than the window: a write past it would park
             * the whole server inside libssh until the client reads. */
            uint32_t window;

            if (wait_for_room(s, 1) != 0) {
                return;
            }
            window = ssh_channel_window_size(s->channel);
            if (want > window) {
                want = window;
            }
        }
        n = ssh_channel_write(s->channel, data + off, want);

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
    if (s->in_cb && s->in_pump) {
        /* A command is streaming and this came from a libssh callback, which must not wait on the
         * poll loop. A write that does not fit is dropped: it is echo, and the editor state is intact. */
        sh_defer_add(&s->defer, data, len);
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
    if (s->in_cb) {
        /* The editor's echo belongs on the terminal, never in the file or pipe a running command has open. */
        chan_write(s, data, len);
        return;
    }
    if (s->out_fp != NULL) {
        /* Flushed per write, for the same reason sh_log_line flushes: a ^C or a
         * dropped channel mid-command must not leave the buffered half of the
         * file on the floor. The volumes here are small (one key line, one
         * listing), so the cost is nothing. */
        fwrite(data, 1, len, s->out_fp);
        fflush(s->out_fp);
        return;
    }
    if (s->capturing) {
        if (!s->cap_over && sh_stdin_append(&s->cap, data, len, SSHD_STDIN_MAX) != 0) {
            s->cap_over = 1;
        }
        return;
    }
    chan_write(s, data, len);
}

int sh_session_redirect_start(sh_session_t *s, const char *sce_path,
                              int append)
{
    FILE *fp;

    if (s == NULL || sce_path == NULL || sce_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (s->out_fp != NULL) {
        errno = EINVAL; /* only one output file per command; the caller undoes */
        return -1;
    }
    /* "w" truncates, "a" appends, both create: the exact difference between
     * `>` and `>>`, and the modes the authorized_keys workflow needs. */
    fp = fopen(sce_path, append ? "a" : "w");
    if (fp == NULL) {
        return -1;
    }
    s->out_fp = fp;
    return 0;
}

void sh_session_redirect_end(sh_session_t *s)
{
    if (s == NULL || s->out_fp == NULL) {
        return;
    }
    /* fflush before fclose checks the error, because fclose alone swallows the
     * one case that matters: the card filled up mid-command and the last
     * writes never landed. The caller has already printed its own diagnostics;
     * there is nothing better to do with the second failure than drop it. */
    fflush(s->out_fp);
    fclose(s->out_fp);
    s->out_fp = NULL;
}

void sh_session_capture_start(sh_session_t *s)
{
    sh_stdin_free(&s->cap);
    s->capturing = 1;
    s->cap_over = 0;
}

int sh_session_capture_end(sh_session_t *s)
{
    int over = s->cap_over;

    s->capturing = 0;
    s->cap_over = 0;
    if (over) {
        sh_stdin_free(&s->cap);
        return -1;
    }
    sh_stdin_move(&s->in, &s->cap);
    return 0;
}

int sh_session_stdin_start(sh_session_t *s, sh_stdin_reader_t rd, void *ctx)
{
    if (s == NULL || rd == NULL) {
        errno = EINVAL;
        return -1;
    }
    return sh_stdin_load(&s->in, rd, ctx, SSHD_STDIN_MAX);
}

size_t sh_session_stdin_read(sh_session_t *s, char *buf, size_t n)
{
    return sh_stdin_read(&s->in, buf, n);
}

void sh_session_stdin_end(sh_session_t *s)
{
    if (s != NULL) {
        sh_stdin_free(&s->in);
    }
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
        sh_write(s, stack, (size_t)need);
        return;
    }
    {
        char *heap = malloc((size_t)need + 1);

        if (heap == NULL) {
            /* Fall back to the truncated stack copy: a partial line beats
             * silence, and the caller has no way to report a failed print. */
            sh_write(s, stack, sizeof(stack) - 1);
            return;
        }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)need + 1, fmt, ap);
        va_end(ap);
        sh_write(s, heap, (size_t)need);
        free(heap);
    }
}

void sh_eprintf(sh_session_t *s, const char *fmt, ...)
{
    /* Deliberately chan_write and not sh_write: this is stderr, and
     * `command > file` redirects stdout only. An error while writing a file --
     * the redirect target filling up, say -- must reach the terminal. */
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

sh_alias_table_t *sh_session_aliases(const sh_session_t *s)
{
    return s == NULL ? NULL : s->aliases;
}

sh_env_table_t *sh_session_env(const sh_session_t *s)
{
    return s == NULL ? NULL : s->env;
}

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
        note_end(s, "exit command");
        s->exit_requested = 1;
    }
}

const char *sh_session_user(const sh_session_t *s)
{
    return (s == NULL || s->user[0] == '\0') ? "vita" : s->user;
}

const char *sh_session_end_reason(const sh_session_t *s)
{
    return s != NULL && s->end_reason != NULL ? s->end_reason : "no reason recorded";
}

const char *sh_session_peer(const sh_session_t *s)
{
    return (s == NULL || s->peer[0] == '\0') ? "unknown" : s->peer;
}

int sh_session_authenticated(const sh_session_t *s)
{
    return s != NULL && s->authenticated;
}

int sh_session_cols(const sh_session_t *s)
{
    if (s == NULL || s->cols < 20) {
        return 80; /* the width a client that never asked for a pty implies */
    }
    return s->cols;
}

int sh_session_history(sh_session_t *s, const char **entries, int max)
{
    int i, n;

    if (s == NULL || entries == NULL || s->hist_len <= 0) {
        return 0;
    }
    n = (s->hist_len < max) ? s->hist_len : max;
    for (i = 0; i < n; i++) {
        entries[i] = s->history[i];
    }
    return n;
}

int sh_session_interrupted(const sh_session_t *s)
{
    /* See the file header: this cannot see a Ctrl-C that arrives mid-command. */
    return s == NULL || s->exit_requested || s->finished;
}

void sh_session_yield(sh_session_t *s)
{
    unsigned long long now;

    if (s == NULL || !s->in_pump) {
        return;
    }
    now = (unsigned long long)sceKernelGetSystemTimeWide();
    if (now - s->last_yield_us < YIELD_INTERVAL_US) {
        return;
    }
    s->last_yield_us = now;
    sh_server_poll_slice(0);
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

static void clear_line(sh_session_t *s)
{
    s->linelen = 0;
    s->cursor = 0;
    s->line[0] = '\0';
}

/* Moves the terminal cursor left n columns with backspaces, which every terminal honors. */
static void cursor_back(sh_session_t *s, size_t n)
{
    char bs[32];

    if (!s->pty) {
        return;
    }
    memset(bs, '\b', sizeof(bs));
    while (n > 0) {
        size_t chunk = n < sizeof(bs) ? n : sizeof(bs);

        sh_write(s, bs, chunk);
        n -= chunk;
    }
}

/* Moves the insertion point, repainting the characters it passes when it goes right. */
static void cursor_to(sh_session_t *s, size_t pos)
{
    if (pos > s->linelen) {
        pos = s->linelen;
    }
    if (pos < s->cursor) {
        cursor_back(s, s->cursor - pos);
    } else if (s->pty && pos > s->cursor) {
        sh_write(s, s->line + s->cursor, pos - s->cursor);
    }
    s->cursor = pos;
}

static void redraw_line(sh_session_t *s)
{
    if (!s->pty) {
        return;
    }
    /* Carriage return, erase to end of line, then the whole line again: cheap
     * and it works without knowing the prompt's rendered width, which the
     * color codes would otherwise make impossible. */
    sh_printf(s, "\r");
    write_prompt(s);
    sh_write(s, s->line, s->linelen);
    sh_printf(s, "\033[K");
    cursor_back(s, s->linelen - s->cursor);
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
        clear_line(s);
        redraw_line(s);
        return;
    }
    s->hist_pos = next;
    snprintf(s->line, sizeof(s->line), "%s", s->history[next]);
    s->linelen = strlen(s->line);
    s->cursor = s->linelen;
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
    memcpy(s->line + s->linelen, name, strlen(name));
    s->linelen += strlen(name);
    sh_write(s, name, strlen(name));
    if (is_dir) {
        s->line[s->linelen++] = '/';
        sh_write(s, "/", 1);
    }
    s->line[s->linelen] = '\0';
}

static void complete_word_at_end(sh_session_t *s)
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
            /* Same ambiguous-match handling as the path branch below: extend
             * the word as far as every MATCH agrees -- `w<TAB>` over
             * which/whoami fills "h" -- then show full names. The reference
             * word is the first match, not table[0]: minimizing over the
             * whole table would drag the common prefix to nothing. */
            const char *first = NULL;
            size_t common = 0;

            for (i = 0; i < (int)count; i++) {
                size_t j;

                if (strncmp(table[i].name, prefix, plen) != 0) {
                    continue;
                }
                if (first == NULL) {
                    first = table[i].name;
                    common = strlen(first);
                    continue;
                }
                j = 0;
                while (j < common && first[j] == table[i].name[j]) {
                    j++;
                }
                common = j;
            }
            if (first != NULL && common > plen) {
                char tail[NAME_MAX + 1];
                size_t len = common - plen;

                if (len >= sizeof(tail)) {
                    len = sizeof(tail) - 1;
                }
                memcpy(tail, first + plen, len);
                tail[len] = '\0';
                insert_completion(s, tail, 0);
            }
            sh_printf(s, "\n");
            for (i = 0; i < (int)count; i++) {
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
            /* Ambiguous. Extend the word by the longest prefix every match
             * shares -- `cd vitassh<TAB>` over vitassh/ and vitasshd/ gains
             * nothing, but `cd v<TAB>` fills "itassh" -- and then list the
             * FULL names. Printing suffixes instead turned that first case
             * into a bare "  d" on the user's screen: correct, and a puzzle.
             * write_prompt + the line re-echo go last, so the list sits above
             * the line being typed, as readline does it. */
            size_t common = strlen(names[0]);

            for (i = 1; i < n && names[i] != NULL; i++) {
                size_t j = 0;

                while (j < common && names[0][j] == names[i][j]) {
                    j++;
                }
                common = j;
            }
            if (common > plen) {
                char tail[NAME_MAX + 1];

                memcpy(tail, names[0] + plen, common - plen);
                tail[common - plen] = '\0';
                insert_completion(s, tail, 0);
            }
            sh_printf(s, "\n");
            for (i = 0; i < n; i++) {
                if (names[i] != NULL) {
                    sh_printf(s, "%s  ", names[i]);
                }
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

/* Completion reads the whole line and appends to it, so it runs from the end. */
static void complete_word(sh_session_t *s)
{
    cursor_to(s, s->linelen);
    complete_word_at_end(s);
    s->cursor = s->linelen;
}

/* ------------------------------------------------------------------ */
/* line editor                                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* history expansion                                                  */
/* ------------------------------------------------------------------ */

/*
 * Replaces a `!n` / `!!` / `!prefix` reference with the line it names, bash
 * style: `!12` runs line 12 of `history`, `!!` the previous line, `!cd` the
 * most recent line starting with "cd". Whatever follows the reference in the
 * typed line is appended to it, so `!12 -la` re-runs line 12 with an extra
 * argument. Numbering matches what the `history` builtin prints: oldest line
 * is 1.
 *
 * Takes the history as parameters rather than reading the session, so the
 * parser can be lifted out and tested on a host without libssh. Single pass:
 * the substituted text is not re-expanded, so a line that expands to itself
 * cannot loop. History expansion is interactive-only, which is also why this
 * sits in the editor and not in sh_execute_line: exec-mode lines have no
 * history to expand against.
 *
 * Returns 0 with the expanded line in out (identical to the input when there
 * was no reference), or -1 after printing "event not found"; the caller then
 * runs nothing and pushes nothing, as bash does.
 */
static int expand_history_ref(const char **history, int hist_len,
                              const char *line, char *out, size_t outlen)
{
    const char *entry = NULL, *rest = NULL;

    if (line[0] != '!') {
        snprintf(out, outlen, "%s", line);
        return 0;
    }

    if (line[1] == '!') {
        entry = (hist_len > 0) ? history[hist_len - 1] : NULL;
        rest = line + 2;
    } else if (line[1] >= '0' && line[1] <= '9') {
        char num[8];
        size_t d = 0;
        int idx;

        while (line[1 + d] >= '0' && line[1 + d] <= '9' && d < sizeof(num) - 1) {
            num[d] = line[1 + d];
            d++;
        }
        num[d] = '\0';
        idx = atoi(num);
        rest = line + 1 + d;
        /* 1-based oldest-first, matching the builtin's numbering */
        if (idx >= 1 && idx <= hist_len) {
            entry = history[idx - 1];
        }
    } else if (line[1] != '\0' && line[1] != ' ') {
        const char *sp = strchr(line + 1, ' ');
        size_t plen = (sp != NULL) ? (size_t)(sp - (line + 1)) : strlen(line + 1);
        int i;

        rest = (sp != NULL) ? sp : line + strlen(line);
        for (i = hist_len - 1; i >= 0; i--) {
            if (strncmp(history[i], line + 1, plen) == 0) {
                entry = history[i];
                break;
            }
        }
    }

    if (entry == NULL) {
        return -1; /* the caller reports it: this function stays session-free */
    }
    snprintf(out, outlen, "%s%s", entry, rest != NULL ? rest : "");
    return 0;
}

static void finish_line(sh_session_t *s)
{
    char expanded[SSHD_LINE_MAX];

    sh_printf(s, "\n");
    if (expand_history_ref((const char **)s->history, s->hist_len, s->line,
                           expanded, sizeof(expanded)) != 0) {
        /* Event not found: nothing runs, nothing is remembered. */
        sh_eprintf(s, "sh: %s: event not found\n", s->line);
        clear_line(s);
        s->hist_pos = SSHD_HISTORY;
        write_prompt(s);
        return;
    }
    if (sh_lineq_push(&s->lineq, expanded) != 0) {
        /* Refusing is the only honest answer: overwriting an earlier line is the failure the
         * queue exists to prevent, and a refused line is not remembered. */
        sh_eprintf(s, "sh: input queue full, line discarded\n");
        clear_line(s);
        s->hist_pos = SSHD_HISTORY;
        return;
    }
    if (strcmp(expanded, s->line) != 0) {
        /* The line that will run is not the line that was typed: show it,
         * bash-style, so `!12` is never a surprise. */
        sh_printf(s, "%s\n", expanded);
    }
    history_push(s, expanded);
    clear_line(s);
    s->hist_pos = SSHD_HISTORY;
}

/* Deletes the character before the cursor, closing the gap. */
static void erase_char(sh_session_t *s)
{
    size_t tail;

    if (s->cursor == 0) {
        return;
    }
    tail = s->linelen - s->cursor;
    memmove(s->line + s->cursor - 1, s->line + s->cursor, tail + 1);
    s->linelen--;
    s->cursor--;
    if (!s->pty) {
        return;
    }
    if (tail == 0) {
        sh_write(s, "\b \b", 3);
        return;
    }
    /* Repaint the tail one column left, blank the stale last column, come back. */
    sh_write(s, "\b", 1);
    sh_write(s, s->line + s->cursor, tail);
    sh_write(s, " ", 1);
    cursor_back(s, tail + 1);
}

static void erase_word(sh_session_t *s)
{
    while (s->cursor > 0 && s->line[s->cursor - 1] == ' ') {
        erase_char(s);
    }
    while (s->cursor > 0 && s->line[s->cursor - 1] != ' ') {
        erase_char(s);
    }
}

static void insert_char(sh_session_t *s, unsigned char c)
{
    if (s->linelen + 1 >= sizeof(s->line)) {
        return; /* the line is full; a beep nobody hears helps no one */
    }
    memmove(s->line + s->cursor + 1, s->line + s->cursor,
            s->linelen - s->cursor + 1);
    s->line[s->cursor++] = (char)c;
    s->linelen++;
    if (s->pty) {
        /* The new character and everything after it, then back to the cursor. */
        sh_write(s, s->line + s->cursor - 1, s->linelen - s->cursor + 1);
        cursor_back(s, s->linelen - s->cursor);
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
        } else if (c == 'C') {
            cursor_to(s, s->cursor + 1);
        } else if (c == 'D' && s->cursor > 0) {
            cursor_to(s, s->cursor - 1);
        } else if (c == 'H') {
            cursor_to(s, 0);
        } else if (c == 'F') {
            cursor_to(s, s->linelen);
        }
        /* Every report we did not ask for is dropped. */
    }
    return 1;
}

static void feed_byte(sh_session_t *s, unsigned char c)
{
    int was_cr = s->after_cr;

    s->after_cr = 0;
    if (feed_escape(s, c)) {
        return;
    }
    if (c == KEY_ESC) {
        s->esc = 1;
        return;
    }
    switch (c) {
    case KEY_CR:
        s->after_cr = 1;
        finish_line(s);
        return;
    case KEY_LF:
        /* A line ending \r\n delivers LF right after CR, which already completed the line;
         * running it again would add an empty line per output line of a scripted pty session. */
        if (!was_cr) {
            finish_line(s);
        }
        return;
    case KEY_BS:
    case KEY_DEL:
        erase_char(s);
        return;
    case KEY_CTRL_C:
        sh_printf(s, "^C\n");
        clear_line(s);
        s->hist_pos = SSHD_HISTORY;
        write_prompt(s);
        return;
    case KEY_CTRL_D:
        if (s->linelen == 0) {
            note_end(s, "ctrl-d on an empty line");
            s->exit_requested = 1; /* EOF on an empty line, as every shell does */
        }
        return;
    case KEY_CTRL_A:
        cursor_to(s, 0);
        return;
    case KEY_CTRL_E:
        cursor_to(s, s->linelen);
        return;
    case KEY_CTRL_U:
        cursor_to(s, s->linelen);
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
            cursor_back(s, s->linelen - s->cursor);
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
    if (s->sftp != NULL) {
        sh_fsctx_t fs;

        sh_session_get_fsctx(s, &fs);
        if (sh_sftp_data(s->sftp, &fs, data, len) != 0) {
            sh_console_log("sftp: closing the channel after a data error");
            ssh_channel_send_eof(channel);
            ssh_channel_close(channel);
            note_end(s, "sftp data error");
            s->finished = 1;
        }
        return (int)len;
    }
    s->in_cb = 1;
    for (i = 0; i < len; i++) {
        feed_byte(s, p[i]);
    }
    s->in_cb = 0;
    /* Return what we took: anything left over is re-delivered on the next poll,
     * and the editor consumes everything, so claiming the whole buffer is the
     * truthful answer. */
    return (int)len;
}

/* A table that is full or missing leaves the variable out; env then shows less, never something false. */
static void set_env(sh_session_t *s, const char *name, const char *value)
{
    if (s->env != NULL && sh_env_set(s->env, name, value) != 0) {
        sh_console_log("env %s from %s not stored: %s", name, sh_session_peer(s), strerror(errno));
    }
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
    if (term != NULL && term[0] != '\0') {
        set_env(s, "TERM", term);
    }
    sh_console_log("pty %s %dx%d from %s", term ? term : "?", cols, rows,
                   sh_session_peer(s));
    return 0; /* 0 accepts; nonzero makes libssh send the failure */
}

/* The window size has one authority, the pty request (s->cols, read by sh_session_cols()), so
 * COLUMNS and LINES are refused rather than stored: a stored copy would go stale on a resize.
 * TERM is stored because the pty request is the only other place it arrives, and an exec
 * session has no pty. Every other name is refused too, and every refusal is logged. */
static int on_env_request(ssh_session session, ssh_channel channel,
                          const char *name, const char *value, void *userdata)
{
    sh_session_t *s = userdata;
    int ok = name != NULL && value != NULL && (strcmp(name, "TERM") == 0 || strcmp(name, "LANG") == 0);

    (void)session;
    (void)channel;
    if (s == NULL) {
        return 1;
    }
    if (ok) {
        if (sh_env_set(s->env, name, value) == 0) {
            return 0;
        }
        sh_console_log("env %s from %s refused: %s", name, sh_session_peer(s), strerror(errno));
        return 1;
    }
    sh_console_log("env %s from %s refused: not accepted", name != NULL ? name : "?", sh_session_peer(s));
    return 1; /* 0 accepts, 1 denies (libssh/callbacks.h) */
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
    if (strlen(command) >= SSHD_LINE_MAX) {
        /* Truncating and running would be worse than refusing: `rm -r` with
         * its last path component cut off still runs. The client sees the
         * refusal as a failed exit status and this as its output. */
        sh_console_log("exec from %s refused: %zu bytes, line limit is %zu",
                       sh_session_peer(s), strlen(command),
                       (size_t)SSHD_LINE_MAX);
        sh_eprintf(s, "exec: command too long (max %u bytes)\n",
                   (unsigned)(SSHD_LINE_MAX - 1));
        return 1;
    }
    if (sh_lineq_push(&s->lineq, command) != 0) {
        sh_eprintf(s, "exec: %s\n", strerror(errno));
        return 1;
    }
    s->shell = 0;
    sh_console_log("exec \"%s\" from %s", command, sh_session_peer(s));
    return 0;
}

static int on_subsystem_request(ssh_session session, ssh_channel channel,
                                const char *what, void *userdata)
{
    sh_session_t *s = userdata;

    if (s == NULL || what == NULL) {
        return 1;
    }
    sh_console_log("subsystem \"%s\" from %s", what, sh_session_peer(s));
    if (strcmp(what, "sftp") == 0 && s->sftp == NULL && !s->shell &&
        sh_lineq_len(&s->lineq) == 0) {
        s->sftp = sh_sftp_new(session, channel);
        if (s->sftp != NULL) {
            return 0;
        }
    }
    /* Anything else is refused: nonzero makes libssh send the channel failure so
     * a client fails fast instead of hanging on an accepted subsystem. The
     * channel stays open for a later request. */
    /* Extended data, not the channel's data stream: a later request on this same
     * channel (sftp, say) reads that stream from its first byte, and the refusal
     * text would be taken for its first packet. */
    {
        char msg[96];
        int n = snprintf(msg, sizeof(msg), "%s: subsystem not supported\n", what);

        if (n > 0) {
            ssh_channel_write_stderr(channel, msg, (uint32_t)(n < (int)sizeof(msg) ? n : (int)sizeof(msg) - 1));
        }
    }
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
        if (s->sftp != NULL && !s->finished) {
            sh_console_log("sftp: client closed the channel");
        }
        note_end(s, "client closed the channel");
        s->finished = 1;
    }
}

static void on_channel_eof(ssh_session session, ssh_channel channel,
                           void *userdata)
{
    sh_session_t *s = userdata;

    (void)session;
    (void)channel;
    /* An exec client with no stdin sends EOF right behind the request; that only
     * means "no more input" and must not stop the command it asked for. */
    if (s != NULL && (s->sftp != NULL || s->shell)) {
        if (s->sftp != NULL && !s->finished) {
            sh_console_log("sftp: client sent EOF");
        }
        if (s->shell && sh_lineq_len(&s->lineq) > 0) {
            s->eof_seen = 1; /* a piped script's last lines are still waiting to run */
            return;
        }
        note_end(s, "client sent EOF");
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
    char clean[sizeof(s->user)];

    if (sh_env_strip_ctrl(clean, sizeof(clean), user) > 0) {
        memcpy(s->user, clean, sizeof(s->user));
        set_env(s, "USER", sh_session_user(s));
        set_env(s, "LOGNAME", sh_session_user(s));
    }
}

/* The login name as the logs show it: control bytes dropped, "?" when the client sent none. */
static const char *loggable_user(char *buf, size_t size, const char *user)
{
    sh_env_strip_ctrl(buf, size, user);
    return buf[0] != '\0' ? buf : "?";
}

static int on_auth_password(ssh_session session, const char *user,
                            const char *password, void *userdata)
{
    sh_session_t *s = userdata;
    char whobuf[64];
    const char *who = loggable_user(whobuf, sizeof(whobuf), user);

    (void)session;
    if (s == NULL) {
        return SSH_AUTH_ERROR;
    }
    if (sh_auth_check_password(sh_server_auth(), user, password) == 0) {
        remember_user(s, user);
        s->authenticated = 1;
        sh_console_log("password login: %s@%s", who,
                       sh_session_peer(s));
        return SSH_AUTH_SUCCESS;
    }
    sh_console_log("password rejected: %s@%s", who,
                   sh_session_peer(s));
    return SSH_AUTH_DENIED;
}

static int on_auth_pubkey(ssh_session session, const char *user,
                          struct ssh_key_struct *pubkey, char signature_state,
                          void *userdata)
{
    sh_session_t *s = userdata;
    char whobuf[64];
    const char *who = loggable_user(whobuf, sizeof(whobuf), user);

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
                       who, sh_session_peer(s));
        return SSH_AUTH_DENIED;
    }
    if (sh_auth_check_pubkey(sh_server_auth(), user, pubkey) == 0) {
        if (signature_state == SSH_PUBLICKEY_STATE_VALID) {
            remember_user(s, user);
            s->authenticated = 1;
            sh_console_log("publickey login: %s@%s", who,
                           sh_session_peer(s));
        }
        return SSH_AUTH_SUCCESS;
    }
    sh_console_log("publickey rejected: %s@%s", who,
                   sh_session_peer(s));
    return SSH_AUTH_DENIED;
}

static int on_service_request(ssh_session session, const char *service,
                              void *userdata)
{
    sh_session_t *s = userdata;
    const char *peer = s != NULL ? sh_session_peer(s) : "unknown";

    (void)session;
    /*
     * The name matters, and getting it wrong ends the connection before any
     * auth callback is reached: after NEWKEYS the client's next packet is
     * SSH_MSG_SERVICE_REQUEST for "ssh-userauth", and this callback is the
     * only gate -- returning nonzero makes libssh send its own disconnect
     * (code 11, "Bye Bye") with no error string to debug from. What the client
     * asks for is therefore not a detail to guess at. "ssh-connection" is the
     * service named *inside* the later userauth request (RFC 4252 section 5),
     * not a service an SSHv2 client requests here, so accepting only it made
     * every login die at this line.
     */
    if (service != NULL && strcmp(service, "ssh-userauth") == 0) {
        sh_console_log("service accepted: %s from %s", service, peer);
        return 0; /* libssh answers with SSH_MSG_SERVICE_ACCEPT */
    }
    /* No SERVICE_DENY in this libssh, so a refused service ends the session --
     * which is why the name is logged: it is the whole diagnosis. */
    sh_console_log("service refused: %s from %s", service ? service : "(none)",
                   peer);
    return 1;
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
    /* The user's home on whichever candidate device the server could write; its
     * .ssh directory holds the authorized_keys. */
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
    s->channel_cbs.channel_env_request_function = on_env_request;
    s->channel_cbs.channel_shell_request_function = on_shell_request;
    s->channel_cbs.channel_exec_request_function = on_exec_request;
    s->channel_cbs.channel_subsystem_request_function = on_subsystem_request;
    s->channel_cbs.channel_close_function = on_channel_close;
    s->channel_cbs.channel_eof_function = on_channel_eof;

    if (ssh_set_server_callbacks(ssh, &s->server_cbs) != SSH_OK) {
        free(s);
        return NULL;
    }
    s->env = sh_env_new();
    if (s->env != NULL) {
        /* PATH is decorative: commands come from the builtins table, never from a search path. */
        set_env(s, "PATH", "/bin:/usr/bin");
        set_env(s, "SHELL", "/bin/sh");
        set_env(s, "HOSTNAME", "vita");
        set_env(s, "LANG", "C");
        set_env(s, "HOME", s->home);
        set_env(s, "USER", sh_session_user(s));
        set_env(s, "LOGNAME", sh_session_user(s));
        set_env(s, "TERM", "dumb"); /* until the client names a terminal */
    }
    s->aliases = sh_alias_new();
    if (s->aliases != NULL) {
        sh_alias_seed(s->aliases);
    }
    /* The auth methods to offer are a policy the host key layer decides: with
     * authorized_keys present, password authentication is still allowed only
     * when a generated password is also live. The reload first is what makes
     * the bootstrap one-shot: the key the previous session just installed via
     * exec was written after this server booted, and without this the next
     * login would be asked for the passphrase forever. */
    sh_auth_reload_if_changed();
    ssh_set_auth_methods(ssh, sh_server_auth_methods());
    return s;
}

void sh_session_free(sh_session_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    sh_sftp_free(s->sftp);
    sh_stdin_free(&s->in);
    sh_stdin_free(&s->cap);
    sh_alias_free(s->aliases);
    sh_env_free(s->env);
    sh_lineq_clear(&s->lineq);
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
    if (s->sftp != NULL && !s->finished) {
        sh_fsctx_t fs;

        sh_session_get_fsctx(s, &fs);
        if (sh_sftp_pump(s->sftp, &fs) != 0) {
            sh_console_log("sftp: closing the channel after a pump error");
            if (s->channel != NULL) {
                ssh_channel_send_eof(s->channel);
                ssh_channel_close(s->channel);
            }
            note_end(s, "sftp pump error");
            s->finished = 1;
        }
    }
    if (s->sftp != NULL && s->finished) {
        time_t now = time(NULL);

        if (s->channel == NULL) {
            return 1;
        }
        /* The client sent EOF or closed: nothing else ends an sftp session, and
         * its ssh waits for our channel close before it exits. */
        if (s->sftp_closing == 0) {
            s->sftp_closing = now;
            ssh_channel_request_send_exit_status(s->channel, 0);
            ssh_channel_send_eof(s->channel);
            ssh_channel_close(s->channel);
        }
        /* Tearing the transport down before the client has answered the close
         * makes its ssh exit 255, and scp reports that as a failed copy. A client
         * that never answers is let go after a few seconds. */
        return ssh_channel_is_closed(s->channel) ||
               now - s->sftp_closing >= SFTP_CLOSE_WAIT_S;
    }
    {
        char line[SSHD_LINE_MAX];

        if (sh_lineq_pop(&s->lineq, line, sizeof(line)) != 0) {
            return 0;
        }
        {
            int status;

            /* Non-blocking for the length of the command, so a client that stops
             * reading can never park libssh in a flush; callbacks keep the
             * blocking session they were written for. */
            ssh_set_blocking(s->ssh, 0);
            s->in_pump = 1;
            status = sh_execute_line(s, line);

            if (!s->shell || sh_vpk_pending()) {
                /* exec mode: the client asked for one command, so report its
                 * status and hang up the way sshd would. The session is freed
                 * as soon as this returns, so what libssh still holds has to
                 * reach the socket first or the tail of the output is lost. */
                ssh_channel_request_send_exit_status(s->channel, status);
                ssh_channel_send_eof(s->channel);
                if (sh_vpk_pending()) {
                    time_t start = time(NULL);

                    ssh_channel_close(s->channel);
                    wait_for_room(s, 0);
                    /* Let OpenSSH acknowledge close before the app (and transport) exits. */
                    while (!ssh_channel_is_closed(s->channel) &&
                           (ssh_get_status(s->ssh) & (SSH_CLOSED | SSH_CLOSED_ERROR)) == 0 &&
                           time(NULL) - start < SFTP_CLOSE_WAIT_S) {
                        sh_server_poll_slice(WINDOW_POLL_MS);
                    }
                }
                wait_for_room(s, 0);
                sh_vpk_handoff();
                s->in_pump = 0;
                ssh_set_blocking(s->ssh, 1);
                note_end(s, "exec command done");
                s->finished = 1;
                return 1;
            }
            if (s->exit_requested || (s->eof_seen && sh_lineq_len(&s->lineq) == 0)) {
                /* The session is freed right after this, and queued commands can run back to
                 * back: what libssh still holds has to reach the socket first. */
                wait_for_room(s, 0);
            }
            s->in_pump = 0;
            ssh_set_blocking(s->ssh, 1);
            chan_write(s, sh_defer_data(&s->defer), sh_defer_len(&s->defer));
            sh_defer_reset(&s->defer);
        }
    }
    if (s->exit_requested || s->finished) {
        return 1;
    }
    if (s->eof_seen && sh_lineq_len(&s->lineq) == 0) {
        note_end(s, "client sent EOF");
        s->finished = 1;
        return 1;
    }
    if (s->linelen > 0) {
        redraw_line(s); /* typed ahead while the command ran: put the prompt before it */
    } else {
        write_prompt(s);
    }
    return 0;
}
