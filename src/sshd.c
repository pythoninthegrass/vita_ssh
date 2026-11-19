/*
 * sshd: the Vita's SSH server.
 *
 * One thread, one poll loop. Everything -- vita2d, the pad, the sockets, every
 * session -- is driven from server_thread(), which means there is no locking
 * anywhere in this program and no way for a session's command to run while
 * another's data is half-read. The Vita has one core's worth of work to do here
 * anyway; the complexity a thread per session would buy would be spent on
 * bugs.
 *
 * Two platform details shaped the loop:
 *
 * The whole thing runs on a thread we created rather than in main() because a
 * homebrew main thread gets a small stack, and libssh's crypto plus the shell's
 * tree walks want hundreds of kilobytes. server_thread() gets an explicit one.
 *
 * Accepting is done by hand (accept + getpeername + ssh_bind_accept_fd) instead
 * of ssh_bind_accept() so the client's address is known before the handshake:
 * the Vita's screen is the only place a person can see who is connected, and
 * "a client" is not an answer. It also means exactly one accept per POLLIN
 * callback, which is what keeps the listening socket from ever blocking the
 * loop -- a drain loop would call accept() with nothing queued and park there,
 * since the listening fd is blocking by default.
 */
#include "sshd.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>

/* ssh_set_log_callback() lives here, not in libssh.h. */
#include <libssh/callbacks.h>
#include <libssh/server.h>
#include <vita2d.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include "network.h"
#include "sh_listen.h"

/*
 * scePowerSetIdleTimerCount() is not in psp2/power.h, but the ScePower stub the
 * app links (libScePower_stub.a, generated from Sony's own module exports)
 * carries it: passing 0 resets the system idle countdown that drives both
 * display-off and suspend. Ticked periodically, it is what keeps the device --
 * and with it the WiFi -- up while nothing is touching the screen. VitaShell
 * ticks it from its main loop for the same reason. When the tick stops, the
 * device suspends on the system's own schedule: sockets die under suspend, so
 * a suspended server is a server the client sees as "Host is down".
 */
int scePowerSetIdleTimerCount(int count);

/* The timers the server ticks; dimming is left alone, so the panel may dim but not turn off or suspend. */
#define POWER_TICK_TYPES 2
static const SceKernelPowerTickType power_tick_types[POWER_TICK_TYPES] = {
    SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND,
    SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF,
};

/* Ticks only, never sceKernelPowerLock: a lock outlives an app the system kills from the PS menu and hangs every later app launch. */
static void power_tick(void)
{
    int i;

    scePowerSetIdleTimerCount(0);
    for (i = 0; i < POWER_TICK_TYPES; i++) {
        sceKernelPowerTick(power_tick_types[i]);
    }
}

#define CONSOLE_LINES 12
#define CONSOLE_WIDTH 108
#define DRAW_INTERVAL_US (200 * 1000)
/* The idle timer is reset at most once a second; the poll loop wakes every 50ms
 * either way, so this only decides how many syscalls the loop makes. */
#define POWER_TICK_US (1000 * 1000)
#define SERVER_STACK (512 * 1024)
/* The poll loop wakes every 50 ms; a gap this long means the process was not running (suspend) or was starved. */
#define RESUME_GAP_US (3 * 1000 * 1000)

/* A connection that has not logged in by now is reclaimed, whether it is still in key exchange or sitting at the login prompt. */
#define HANDSHAKE_TIMEOUT_S 30

typedef struct {
    sh_session_t *sh;
    ssh_session ssh;
    int used;
    int handshaking;
    unsigned long long accepted_us;
} slot_t;

static ssh_event poll_event;

static slot_t slots[SSHD_MAX_SESSIONS];
/* The slot whose command is running inside sh_session_pump(), or -1; a poll slice must not free it. */
static int pumping_slot = -1;
static int listen_port;
static volatile int quit_requested;
/* Whether the listening socket works; on_incoming() reports into it and server_loop() acts on it. */
static sh_listen_t listener_health;

static char log_ring[CONSOLE_LINES][CONSOLE_WIDTH];
static int log_head;
static int log_count;

/* The one answer to "how many sessions are open", asked by the console's status
 * line and by the accept log. Nothing else walks the slot table. */
static int count_sessions(void)
{
    int i, n = 0;

    for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
        if (slots[i].used) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* the Vita's own screen                                              */
/* ------------------------------------------------------------------ */

/*
 * Status goes here, and from here to two places: the on-screen ring below, and
 * the log file sh_log_line() mirrors each line into (sshd_hostkey.c). printf() on
 * this SDK reaches a debug screen that the app never initializes (there is no
 * sceDebugScreenInit in these headers), so a message sent there is a message
 * nobody will ever read. That is also why every failure path below calls
 * sh_console_log rather than fprintf.
 */
/* The log file gets the local wall-clock time in front of the uptime stamp, so a
 * line can be matched to the moment a test ran. The screen keeps the short form:
 * its width is spoken for. The uptime stamp has no seconds field, which makes
 * lines within one minute look out of order; the wall clock carries them. If
 * localtime_r refuses the time (sceRtc can), the line goes out unchanged. */
static void mirror_to_file(const char *ring_line)
{
    char stamp[24];
    char line[CONSOLE_WIDTH + 32];
    time_t now = time(NULL);
    struct tm tm;

    if (localtime_r(&now, &tm) == NULL ||
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
        sh_log_line(ring_line);
        return;
    }
    /* ring_line is "[uptime] text": drop its bracket and put it after the stamp. */
    snprintf(line, sizeof(line), "[%s %s", stamp, ring_line + 1);
    sh_log_line(line);
}

void sh_console_log(const char *fmt, ...)
{
    va_list ap;
    /* Room for the "[mmm.nnn] " prefix, so a long line loses characters from the
     * body and never loses the timestamp that makes the log readable. */
    char body[CONSOLE_WIDTH - 16];
    /* The system timer, not sceKernelGetProcessTimeWide: that one read zero on
     * every line of the first device log, which is precisely when elapsed time
     * is the thing worth knowing -- process time only counts CPU the process
     * actually ran, and a boot spent waiting inside syscalls accumulates none.
     * Microseconds since the device booted, so the prefix is uptime-relative;
     * differences between lines are what the log is read for. */
    unsigned long long now =
        (unsigned long long)sceKernelGetSystemTimeWide() / 1000ULL;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    /* now is milliseconds: minutes are /60000, and the right-hand field is the
     * millisecond remainder. The first version divided the microsecond count by
     * 1e6 -- seconds -- and then printed the remainder of that under a field
     * labeled milliseconds, so [0.037] meant thirty-seven seconds. */
    snprintf(log_ring[log_head], CONSOLE_WIDTH, "[%3llu.%03llu] %s",
             (now / 60000) % 1000, now % 1000, body);
    mirror_to_file(log_ring[log_head]); /* same entry, before the head moves on */
    log_head = (log_head + 1) % CONSOLE_LINES;
    if (log_count < CONSOLE_LINES) {
        log_count++;
    }
}

/* network.c is shared with the client app, so it cannot print: this app never
 * calls sceDebugScreenInit, so printf() there would write to a screen nobody can
 * read while a real console is on. Forward each bring-up message into the ring
 * instead. The message is passed through "%s" so a stray % in it cannot be read
 * as a format. */
void net_log(const char *fmt, ...)
{
    va_list ap;
    char line[CONSOLE_WIDTH];

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    sh_console_log("%s", line);
}

/* libssh's own diagnostics, into the same log as ours so the two interleave in
 * the right order -- which is the point: our call sites can say a key exchange
 * returned OK while libssh's protocol log says it never finished. The level is
 * chosen at the install site; nothing there prints packet payloads, which is
 * what keeps this safe to leave world-readable on the USB mount while a
 * generated session password may be live. */
static void libssh_log(int priority, const char *function,
                       const char *buffer, void *userdata)
{
    (void)priority;
    (void)userdata;

    sh_console_log("libssh: %s%s%s", function ? function : "",
                   (function != NULL && function[0] != '\0') ? ": " : "",
                   buffer ? buffer : "");
}

static void draw_console(vita2d_pgf *font, const sh_auth_t *auth,
                         const char *ip)
{
    static const unsigned int WHITE = RGBA8(235, 235, 235, 255);
    static const unsigned int DIM = RGBA8(150, 150, 150, 255);
    static const unsigned int CYAN = RGBA8(90, 210, 230, 255);
    static const unsigned int GREEN = RGBA8(120, 220, 120, 255);
    static const unsigned int AMBER = RGBA8(240, 190, 90, 255);
    int y = 18;
    int sessions = count_sessions();
    int i;

    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(16, 18, 22, 255));
    vita2d_draw_rectangle(0, 0, 960, 3, sessions > 0 ? RGBA8(120, 220, 120, 255)
                                                     : RGBA8(70, 70, 90, 255));

    vita2d_pgf_draw_text(font, 16, y += 22, WHITE, 1.0f, "VitaSSH server");
    vita2d_pgf_draw_text(font, 16, y += 20,
                         sessions > 0 ? GREEN : DIM, 1.0f,
                         sessions > 0 ? "listening" : "idle");

    vita2d_pgf_draw_text(font, 200, y - 20, CYAN, 1.0f, ip);
    vita2d_pgf_draw_text(font, 200 + (int)strlen(ip) * 8 + 12, y - 20, WHITE,
                         1.0f, "");
    vita2d_pgf_draw_textf(font, 560, y - 20, WHITE, 1.0f, "port %d   sessions %d/%d",
                          listen_port, sessions, SSHD_MAX_SESSIONS);

    y += 12;
    vita2d_pgf_draw_text(font, 16, y += 20, DIM, 1.0f, "host key");
    {
        const char *fp = auth->fingerprint[0] ? auth->fingerprint : "-";
        vita2d_pgf_draw_text(font, 120, y, WHITE, 1.0f, fp);
        /*
         * Whether that key is on storage is the difference between a fingerprint
         * a client can trust forever and one that moves under its known_hosts
         * file every boot. It goes in the status area rather than only the log,
         * because the log scrolls and this is the line somebody needs a day later
         * when their client starts complaining about exactly that.
         */
        /* PGF is proportional: strlen * 8 underestimates the fingerprint's
         * width. Give persistence its own row instead of overlapping the hash. */
        y += 20;
        if (auth->key_persisted && auth->key_path[0] != '\0') {
            vita2d_pgf_draw_textf(font, 120, y, DIM, 1.0f, "saved in %s",
                                  auth->key_path);
        } else {
            vita2d_pgf_draw_text(font, 120, y, AMBER, 1.0f,
                                 "not saved: new fingerprint each boot");
        }
    }

    y += 4;
    vita2d_pgf_draw_text(font, 16, y += 20, DIM, 1.0f, "sign-in");
    if (auth->keys_loaded > 0) {
        vita2d_pgf_draw_textf(font, 120, y, GREEN, 1.0f,
                              "%d key(s) in %s", auth->keys_loaded,
                              auth->keys_path);
    } else if (auth->password_used) {
        /* One line, all of it: the person standing at the Vita has to read this
         * off a small screen and type it on another machine, so the passphrase
         * is shown in full, spaces included, with nothing that could be mistaken
         * for part of it (no quotes inside, no trailing punctuation). */
        vita2d_pgf_draw_textf(font, 120, y, AMBER, 1.0f,
                              "passphrase \"%s\" (any username)",
                              auth->password);
    } else {
        vita2d_pgf_draw_text(font, 120, y, AMBER, 1.0f,
                             "LOCKED: no host entropy and no authorized_keys");
    }

    /* A file that produced no keys looks exactly like no file from the line
     * above, and the person who just typed their key into it by hand needs to
     * know which of the two they are looking at. */
    if (auth->keys_rejected > 0 && auth->keys_path[0] != '\0') {
        y += 18;
        vita2d_pgf_draw_textf(font, 16, y, AMBER, 1.0f,
                              "%d line(s) in %s were not usable",
                              auth->keys_rejected, auth->keys_path);
    }

    y += 14;
    vita2d_draw_rectangle(16, y, 928, 1, RGBA8(60, 64, 74, 255));
    y += 8;
    for (i = 0; i < log_count; i++) {
        int idx = (log_head - log_count + i + CONSOLE_LINES * 2) % CONSOLE_LINES;

        vita2d_pgf_draw_text(font, 16, y += 18, DIM, 1.0f, log_ring[idx]);
    }

    vita2d_pgf_draw_text(font, 16, 528, WHITE, 1.0f,
                         "START: stop server      ssh <user>@");
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* ------------------------------------------------------------------ */
/* network                                                            */
/* ------------------------------------------------------------------ */

static void local_ip(char *dst, size_t dstlen)
{
    SceNetCtlInfo info;

    snprintf(dst, dstlen, "0.0.0.0");
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) == 0 &&
        info.ip_address[0] != '\0') {
        snprintf(dst, dstlen, "%s", info.ip_address);
    }
}

static int bring_up_network(void)
{
    if (load_network_modules() != 0) {
        sh_console_log("error: network modules failed to load");
        return -1;
    }
    if (init_network() != 0) {
        sh_console_log("error: sceNet did not come up (is Wi-Fi connected?)");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* accepting                                                          */
/* ------------------------------------------------------------------ */

static int slot_alloc(void)
{
    int i;

    for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
        if (!slots[i].used) {
            return i;
        }
    }
    return -1;
}

static void slot_free(int i)
{
    if (i < 0 || i >= SSHD_MAX_SESSIONS || !slots[i].used) {
        return;
    }
    if (slots[i].ssh != NULL) {
        ssh_event_remove_session(poll_event, slots[i].ssh);
        ssh_disconnect(slots[i].ssh);
        ssh_free(slots[i].ssh);
    }
    if (slots[i].sh != NULL) {
        sh_session_free(slots[i].sh);
    }
    memset(&slots[i], 0, sizeof(slots[i]));
}

/* Frees the slot if libssh saw its transport close; `when` is appended to the log line. Returns 1 if freed. */
static int slot_reap_closed(int i, const char *when)
{
    int status;

    if (!slots[i].used) {
        return 0;
    }
    status = ssh_get_status(slots[i].ssh);
    if ((status & (SSH_CLOSED | SSH_CLOSED_ERROR)) != 0) {
        /* The two bits mean different failures -- a peer that hung up
         * versus a protocol or crypto error on our side -- and libssh's
         * own error string names the reason. Without both, a dropped
         * session says nothing that the client did not already show.
         * Protocol text only: libssh never puts secrets in its error
         * buffer. */
        const char *why = ssh_get_error(slots[i].ssh);
        int has_why = why != NULL && why[0] != '\0';

        sh_console_log("session %s dropped: %s 0x%X%s%s%s",
                       sh_session_peer(slots[i].sh),
                       (status & SSH_CLOSED_ERROR) ? "error" : "peer closed",
                       status,
                       has_why ? " -- " : "",
                       has_why ? why : "",
                       when);
        slot_free(i);
        return 1;
    }
    return 0;
}

/* One poll of every session's socket. Called from a running command's yield, so a client that went away meanwhile is freed now, not when the command ends. */
void sh_server_poll_slice(int ms)
{
    int i;

    if (poll_event == NULL) {
        return;
    }
    ssh_event_dopoll(poll_event, ms);
    for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
        if (i != pumping_slot) {
            slot_reap_closed(i, " (while a command ran)");
        }
    }
}

/* The slot that has waited longest without a login, or -1 when every open session has logged in. */
static int slot_oldest_preauth(void)
{
    int i, oldest = -1;

    for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
        if (!slots[i].used || sh_session_authenticated(slots[i].sh)) {
            continue;
        }
        if (oldest < 0 || slots[i].accepted_us < slots[oldest].accepted_us) {
            oldest = i;
        }
    }
    return oldest;
}

/* Key exchange runs from the poll loop on a non-blocking session: a client that completes TCP and sends nothing must not park the one thread. */
static void handshake_done(int i)
{
    slots[i].handshaking = 0;
    ssh_set_blocking(slots[i].ssh, 1);
    sh_console_log("peer %s: key exchange complete", sh_session_peer(slots[i].sh));
    sh_console_log("connection from %s (%d open)", sh_session_peer(slots[i].sh),
                   count_sessions());
}

static void handshake_step(int i)
{
    int rc = ssh_handle_key_exchange(slots[i].ssh);

    if (rc == SSH_AGAIN) {
        return;
    }
    if (rc != SSH_OK) {
        /* A port scan or a client that does not speak the same kex ends here; one line is all it is worth. */
        sh_console_log("handshake failed from %s: %s", sh_session_peer(slots[i].sh),
                       ssh_get_error(slots[i].ssh));
        slot_free(i);
        return;
    }
    handshake_done(i);
}

static int on_incoming(socket_t fd, int revents, void *userdata)
{
    ssh_bind bind = (ssh_bind)userdata;
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    socket_t client;
    ssh_session ssh;
    sh_session_t *sh;
    char ip[INET_ADDRSTRLEN] = "unknown";
    int index, rc;

    sh_console_log("incoming: fd %d, events 0x%X; accepting",
                   (int)fd, (unsigned int)revents);

    /* One accept per callback. See the file header for why the loop below is
     * deliberately absent. */
    memset(&peer, 0, sizeof(peer));
    client = accept((int)fd, (struct sockaddr *)&peer, &peerlen);
    if (client < 0) {
        int error = errno;
        /* sceNet puts its own 0x80...... codes in errno and newlib's strerror()
         * has no text for them -- the first of these lines read "errno
         * -2143223389 ()" -- so print the hex, which is the only legible form. */
        sh_console_log("accept failed: errno %d (0x%08X) %s", error,
                       (unsigned int)error, strerror(error));
        /* The listener cannot be replaced from inside its own poll callback; server_loop() does it. A network
         * change leaves the old socket readable once and then silent, so one failure is enough to rebuild. */
        sh_listen_failed(&listener_health, sceKernelGetProcessTimeWide());
        return SSH_OK;
    }
    sh_listen_accepted(&listener_health);
    sh_console_log("accept returned fd %d, family %d", (int)client,
                   (int)peer.sin_family);
    if (peer.sin_family == AF_INET) {
        const char *seen = inet_ntoa(peer.sin_addr);

        if (seen != NULL) {
            snprintf(ip, sizeof(ip), "%s", seen);
        }
    }
    index = slot_alloc();
    if (index < 0) {
        /* A silent socket must not keep a real client out: take the slot of the longest-waiting session that has not logged in. */
        index = slot_oldest_preauth();
        if (index < 0) {
            sh_console_log("refused %s: %d sessions already open", ip,
                           SSHD_MAX_SESSIONS);
            close((int)client);
            return SSH_OK;
        }
        sh_console_log("evicting %s (no login after %llu s) for %s",
                       sh_session_peer(slots[index].sh),
                       (sceKernelGetProcessTimeWide() - slots[index].accepted_us) / 1000000ULL,
                       ip);
        slot_free(index);
    }

    sh_console_log("peer %s: allocating SSH session", ip);
    ssh = ssh_new();
    if (ssh == NULL) {
        sh_console_log("refused %s: out of memory", ip);
        close((int)client);
        return SSH_OK;
    }
    sh_console_log("peer %s: attaching fd %d to libssh", ip, (int)client);
    if (ssh_bind_accept_fd(bind, ssh, client) != SSH_OK) {
        sh_console_log("refused %s: %s", ip, ssh_get_error(ssh));
        ssh_free(ssh);
        close((int)client);
        return SSH_OK;
    }

    sh_console_log("peer %s: fd attached; installing session callbacks", ip);
    sh = sh_session_new(ssh, ip);
    if (sh == NULL) {
        sh_console_log("refused %s: session allocation failed", ip);
        ssh_free(ssh);
        close((int)client);
        return SSH_OK;
    }
    sh_console_log("peer %s: starting key exchange", ip);
    ssh_set_blocking(ssh, 0);
    rc = ssh_handle_key_exchange(ssh);
    if (rc == SSH_ERROR) {
        sh_console_log("handshake failed from %s: %s", ip,
                       ssh_get_error(ssh));
        sh_session_free(sh);
        ssh_free(ssh);
        close((int)client);
        return SSH_OK;
    }

    if (ssh_event_add_session(poll_event, ssh) != SSH_OK) {
        sh_console_log("refused %s: could not join the poll loop", ip);
        sh_session_free(sh);
        ssh_free(ssh);
        close((int)client);
        return SSH_OK;
    }

    slots[index].used = 1;
    slots[index].ssh = ssh;
    slots[index].sh = sh;
    slots[index].handshaking = rc != SSH_OK;
    slots[index].accepted_us = sceKernelGetProcessTimeWide();
    if (rc == SSH_OK) {
        handshake_done(index);
    }
    return SSH_OK;
}

/* ------------------------------------------------------------------ */
/* bind                                                              */
/* ------------------------------------------------------------------ */

static ssh_bind make_bind(const sh_auth_t *auth, int port)
{
    ssh_bind bind = ssh_bind_new();
    ssh_key key = sh_server_host_key();
    int verbosity = SSH_LOG_NOLOG;

    if (bind == NULL) {
        return NULL;
    }
    if (key == NULL) {
        ssh_bind_free(bind);
        return NULL;
    }
    /* IMPORT_KEY transfers ownership to the bind. Keep the auth layer's original
     * alive across failed listeners (notably port 22 -> 2222) and final cleanup.
     * Import an in-memory copy so RAM-only identities work too. */
    key = ssh_key_dup(key);
    if (key == NULL) {
        sh_console_log("could not copy host key for port %d", port);
        ssh_bind_free(bind);
        return NULL;
    }
    if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key) != 0) {
        sh_console_log("port %d host key: %s", port, ssh_get_error(bind));
        ssh_key_free(key); /* ownership transfers only on successful import */
        ssh_bind_free(bind);
        return NULL;
    }
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &verbosity);
    if (ssh_bind_listen(bind) != 0) {
        sh_console_log("port %d: %s", port, ssh_get_error(bind));
        ssh_bind_free(bind);
        return NULL;
    }
    (void)auth;
    return bind;
}

static int net_connected(void)
{
    int state = 0;

    return sceNetCtlInetGetState(&state) == 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

static void listener_drop(ssh_bind *bind)
{
    if (*bind == NULL) {
        return;
    }
    ssh_event_remove_fd(poll_event, ssh_bind_get_fd(*bind));
    ssh_bind_free(*bind);
    *bind = NULL;
}

/* Replaces the listening socket after it failed (sceNet kills it when the network changes under a suspend). A
 * failure here leaves it down for sh_listen_failed()'s backoff; open sessions are separate sockets and stay. */
static void listener_rebuild(ssh_bind *bind, const sh_auth_t *auth, const char *ip)
{
    unsigned long long now = sceKernelGetProcessTimeWide();

    sh_console_log("listener: rebuilding on port %d", listen_port);
    listener_drop(bind);
    if (!net_connected()) {
        sh_console_log("listener: network is not connected; will retry");
        sh_listen_failed(&listener_health, now);
        return;
    }
    *bind = make_bind(auth, listen_port);
    if (*bind == NULL) {
        sh_console_log("listener: could not bind port %d; will retry", listen_port);
        sh_listen_failed(&listener_health, now);
        return;
    }
    if (ssh_event_add_fd(poll_event, ssh_bind_get_fd(*bind), POLLIN, on_incoming, *bind) != SSH_OK) {
        sh_console_log("listener: could not watch the new socket; will retry");
        ssh_bind_free(*bind);
        *bind = NULL;
        sh_listen_failed(&listener_health, now);
        return;
    }
    sh_listen_rebuilt(&listener_health);
    sh_console_log("ready: ssh any@%s -p %d (listener rebuilt, fd %d)", ip, listen_port,
                   (int)ssh_bind_get_fd(*bind));
}

/* Re-reads the address so the screen and the ready line follow a network change; logs only when it moves. */
static void refresh_ip(char *ip, size_t len)
{
    char now_ip[64];

    local_ip(now_ip, sizeof(now_ip));
    if (strcmp(ip, now_ip) != 0) {
        sh_console_log("address changed: %s -> %s", ip, now_ip);
        snprintf(ip, len, "%s", now_ip);
    }
}

/* ------------------------------------------------------------------ */
/* the loop                                                           */
/* ------------------------------------------------------------------ */

static int start_pressed(void)
{
    static SceCtrlData pad;
    static int held;

    sceCtrlPeekBufferPositive(0, &pad, 1);
    if ((pad.buttons & SCE_CTRL_START) != 0) {
        if (++held > 30) { /* about a second at 30fps: no accidental exits */
            return 1;
        }
        return 0;
    }
    held = 0;
    return 0;
}

static void server_loop(ssh_bind *bind, const sh_auth_t *auth, vita2d_pgf *font)
{
    unsigned long long next_draw = 0;
    unsigned long long next_power_tick = 0;
    unsigned long long last_wake = sceKernelGetSystemTimeWide();
    char ip[64];
    /* The draw takes a copy, not the caller's: the live state can move under
     * it (a key installed over this very session), and a const pointer cannot
     * follow it. */
    sh_auth_t shown;

    (void)auth;

    local_ip(ip, sizeof(ip));
    sh_console_log("ready: ssh any@%s -p %d", ip, listen_port);
    sh_console_log("poll: watching listener fd %d", (int)ssh_bind_get_fd(*bind));

    while (!quit_requested) {
        int i;
        unsigned long long woke = sceKernelGetSystemTimeWide();

        if (woke - last_wake >= RESUME_GAP_US) {
            sh_console_log("poll: loop stalled for %llu ms (suspend or starvation); %d session(s) open",
                           (woke - last_wake) / 1000ULL, count_sessions());
        }
        last_wake = woke;
        if (ssh_event_dopoll(poll_event, 50) == SSH_ERROR) {
            /* dopoll reports an error for any session's socket; the per-session
             * check right below is what decides who to drop, so there is
             * nothing to do here but keep going. */
        }
        if (sh_listen_due(&listener_health, sceKernelGetProcessTimeWide())) {
            refresh_ip(ip, sizeof(ip));
            listener_rebuild(bind, auth, ip);
        }
        for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
            int pumped;

            if (!slots[i].used) {
                continue;
            }
            if (!sh_session_authenticated(slots[i].sh) &&
                sceKernelGetProcessTimeWide() - slots[i].accepted_us >=
                    HANDSHAKE_TIMEOUT_S * 1000000ULL) {
                sh_console_log("reclaimed %s: no login within %d s",
                               sh_session_peer(slots[i].sh), HANDSHAKE_TIMEOUT_S);
                slot_free(i);
                continue;
            }
            if (slots[i].handshaking) {
                handshake_step(i);
                if (!slots[i].used || slots[i].handshaking) {
                    continue;
                }
            }
            /* Before the pump, which would start a queued command for a client that is already gone. */
            if (slot_reap_closed(i, "")) {
                continue;
            }
            pumping_slot = i;
            pumped = sh_session_pump(slots[i].sh);
            pumping_slot = -1;
            /* A command that yielded the whole time is not a suspend: skip the stall line for its duration only. */
            if (sceKernelGetSystemTimeWide() - woke >= RESUME_GAP_US) {
                last_wake = sceKernelGetSystemTimeWide();
            }
            if (pumped != 0) {
                sh_console_log("session %s closed: %s", sh_session_peer(slots[i].sh),
                               sh_session_end_reason(slots[i].sh));
                slot_free(i);
                continue;
            }
            slot_reap_closed(i, "");
        }
        if (start_pressed()) {
            sh_console_log("START held: stopping");
            break;
        }
        {
            unsigned long long now = sceKernelGetProcessTimeWide();

            /* Keep the device awake while there is a reason to. The idle
             * timer is what suspended the process mid-session and looked like
             * "Host is down" on the client; the reset below is why it will
             * not happen again. An active session always counts as a reason,
             * even on a low battery -- a session dying to standby dies
             * anyway. With nobody connected, a low battery is allowed to win
             * and the device sleeps on its own schedule, rather than the
             * server holding it awake until the battery is flat. */
            if (now >= next_power_tick) {
                refresh_ip(ip, sizeof(ip));
                if (count_sessions() > 0 || !scePowerIsLowBattery()) {
                    power_tick();
                }
                next_power_tick = now + POWER_TICK_US;
            }
            if (now >= next_draw) {
                /* Refresh before drawing: a key installed over the session
                 * that is on screen right now has to change what the screen
                 * says, without waiting for a reboot. */
                sh_auth_snapshot(&shown);
                draw_console(font, &shown, ip);
                next_draw = now + DRAW_INTERVAL_US;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

static int server_thread(SceSize args, void *argp)
{
    sh_auth_t auth;
    ssh_bind bind = NULL;
    vita2d_pgf *font;
    int ports[2];
    int p;

    (void)args;
    (void)argp;

    /* The log is opened in main(), before this thread exists: failing to create
     * the thread at all is one of the ways this app can die, and it would leave
     * no trace from in here. */
    sh_console_log("server thread started (%d KB stack)", SERVER_STACK / 1024);

    vita2d_init();
    vita2d_set_clear_color(RGBA8(16, 18, 22, 255));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    font = vita2d_load_default_pgf();
    if (font == NULL) {
        /* Nothing can be shown without the font, but the log file reaches the
         * user the same way, so say what happened there and leave the device in
         * a state they can still recover from. */
        sh_console_log("error: the console font did not load, cannot start");
        vita2d_fini();
        sceKernelExitDeleteThread(0);
        return 0;
    }
    /* Positive checkpoints, not just failures: when the log stops mid-boot the
     * last line has to name the stage the app did not come back from, and a gap
     * between two error paths would say nothing at all. */
    sh_console_log("display and font ready (libssh %d.%d.%d)",
                   LIBSSH_VERSION_MAJOR, LIBSSH_VERSION_MINOR,
                   LIBSSH_VERSION_MICRO);
    /* FUNCTIONS, not the quieter levels: libssh 0.12.2 has no SSH_LOG_PROTOCOL
     * lines in kex.c, server.c, packet.c or session.c at all -- the packet
     * trace ("packet: read type N", "Dispatching handler for packet type N",
     * "Auth request for service ... method ...") is SSH_LOG_PACKET, and
     * socket.c's poll trace ("Poll callback on socket 5 (POLLIN ...)"), which is
     * the line that distinguishes a lost packet from a peer that hung up, is
     * priority 4. Nothing at these levels prints payload bytes: ssh_get_hexa()
     * appears in logging only under WITH_GSSAPI, which this build does not
     * enable, so the log stays safe to leave world-readable on the USB mount
     * while a generated password may still be live.
     *
     * The level and callback are thread-local storage in libssh unless the port
     * is configured with HAVE_GCC_THREAD_LOCAL_STORAGE=OFF (see
     * taskfiles/setup.yml), which this app's server thread needs because
     * sceKernelCreateThread gives it no TLS block. The line below is what proves
     * that took: without the port flag it reads back level 0 / unset here. */
    ssh_set_log_level(SSH_LOG_FUNCTIONS);
    ssh_set_log_callback(libssh_log);
    sh_console_log("libssh log: level %d, callback %s", ssh_get_log_level(),
                   ssh_get_log_callback() == NULL ? "unset" : "set");

    if (ssh_init() != SSH_OK) {
        sh_console_log("error: ssh_init failed");
    } else if (bring_up_network() != 0) {
        /* keep going: the console must show why nothing is listening */
    } else if (sh_auth_prepare(&auth) != 0) {
        sh_console_log("error: cannot start without a host key");
    } else {
        sh_listen_init(&listener_health);
        ports[0] = SSHD_PORT_PRIMARY;
        ports[1] = SSHD_PORT_FALLBACK;
        for (p = 0; p < 2; p++) {
            sh_console_log("trying port %d", ports[p]);
            bind = make_bind(&auth, ports[p]);
            if (bind != NULL) {
                listen_port = ports[p];
                break;
            }
        }
    }

    if (bind != NULL) {
        poll_event = ssh_event_new();
        if (poll_event == NULL) {
            sh_console_log("error: could not create the poll loop");
        } else if (ssh_event_add_fd(poll_event, ssh_bind_get_fd(bind), POLLIN,
                                   on_incoming, bind) != SSH_OK) {
            sh_console_log("error: could not watch the listening socket");
        } else {
            sh_console_log("poll loop starting");
            /* So the log explains the next mystery too: why the device stayed
             * awake with nobody touching it. */
            sh_console_log("power: idle timer reset and power ticks 1/s; "
                           "the device stays awake while the server runs");
            server_loop(&bind, &auth, font);
        }
    }

    {
        int i;

        for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
            slot_free(i);
        }
    }
    if (poll_event != NULL) {
        ssh_event_free(poll_event);
    }
    if (bind != NULL) {
        ssh_bind_free(bind);
    }
    cleanup_network();
    ssh_finalize();
    vita2d_fini();
    sh_console_log("stopped");
    /* No sh_log_close() here or in the font path: main() opened the log and owns
     * closing it, so it can still write the line saying how the thread ended.
     * Every line is flushed as it is written, so a thread that never comes back
     * loses nothing that was said before it died. */
    sceKernelExitDeleteThread(0);
    return 0;
}

int main(void)
{
    /* Arg 6 is an int cpuAffinityMask, not a pointer: 0 lets the kernel pick. */
    SceUID thread;
    int status = 0;

    /* Opened here rather than in the server thread, because the two quiet ways
     * this app can end both happen outside that thread: the thread may never be
     * created (no memory for a 512 KB stack being the real candidate), and the
     * server thread may return. Either one leaves the livearea with no dialog and
     * no frame drawn, which looks exactly like a crash and used to leave no
     * evidence whatsoever. */
    if (sh_log_open() == 0) {
        sh_console_log("=== VitaSSHd starting, log %s ===", sh_log_path());
    } else {
        sh_console_log("=== VitaSSHd starting, no writable log ===");
    }

    thread = sceKernelCreateThread("sshd", server_thread, 0x10000100,
                                   SERVER_STACK, 0, 0, NULL);
    if (thread < 0) {
        sh_console_log("error: could not create the server thread: 0x%08X",
                       (unsigned int)thread);
        sh_log_close();
        return 1;
    }
    if (sceKernelStartThread(thread, 0, NULL) != 0) {
        sh_console_log("error: could not start the server thread");
        sh_log_close();
        return 1;
    }
    /* The main thread's only job is to stay alive until the server finishes:
     * if it exited, the process would go with it. */
    sceKernelWaitThreadEnd(thread, &status, NULL);
    /* The distinction the user cannot see from the sofa: a thread that came back
     * on its own means the log already says why, while a run that stops here
     * without this line died somewhere it could not report. */
    sh_console_log("server thread ended, status 0x%08X", (unsigned int)status);
    sh_log_close();
    sceKernelExitProcess(0);
    return 0;
}
