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

#include <libssh/server.h>
#include <vita2d.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "network.h"

#define CONSOLE_LINES 12
#define CONSOLE_WIDTH 108
#define DRAW_INTERVAL_US (200 * 1000)
#define SERVER_STACK (512 * 1024)

typedef struct {
    sh_session_t *sh;
    ssh_session ssh;
    int used;
} slot_t;

static ssh_event poll_event;
static slot_t slots[SSHD_MAX_SESSIONS];
static int listen_port;
static volatile int quit_requested;

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
 * this SDK reaches a debug screen that the app never initialises (there is no
 * sceDebugScreenInit in these headers), so a message sent there is a message
 * nobody will ever read. That is also why every failure path below calls
 * sh_console_log rather than fprintf.
 */
void sh_console_log(const char *fmt, ...)
{
    va_list ap;
    /* Room for the "[mmm.nnn] " prefix, so a long line loses characters from the
     * body and never loses the timestamp that makes the log readable. */
    char body[CONSOLE_WIDTH - 16];
    unsigned long long now = sceKernelGetProcessTimeWide() / 1000000ULL;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    snprintf(log_ring[log_head], CONSOLE_WIDTH, "[%3llu.%03llu] %s",
             (now / 60) % 1000, now % 1000, body);
    sh_log_line(log_ring[log_head]); /* same entry, before the head moves on */
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
        int tail = 120 + (int)strlen(fp) * 8 + 16;

        vita2d_pgf_draw_text(font, 120, y, WHITE, 1.0f, fp);
        /*
         * Whether that key is on storage is the difference between a fingerprint
         * a client can trust forever and one that moves under its known_hosts
         * file every boot. It goes in the status area rather than only the log,
         * because the log scrolls and this is the line somebody needs a day later
         * when their client starts complaining about exactly that.
         */
        if (auth->key_persisted && auth->key_path[0] != '\0') {
            vita2d_pgf_draw_textf(font, tail, y, DIM, 1.0f, "saved in %s",
                                  auth->key_path);
        } else {
            vita2d_pgf_draw_text(font, tail, y, AMBER, 1.0f,
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
        vita2d_pgf_draw_textf(font, 120, y, AMBER, 1.0f,
                              "password \"%s\" (any username)",
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

static int on_incoming(socket_t fd, int revents, void *userdata)
{
    ssh_bind bind = (ssh_bind)userdata;
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    socket_t client;
    ssh_session ssh;
    sh_session_t *sh;
    char ip[INET_ADDRSTRLEN] = "unknown";
    int index;

    (void)fd;
    (void)revents;

    /* One accept per callback. See the file header for why the loop below is
     * deliberately absent. */
    memset(&peer, 0, sizeof(peer));
    client = accept((int)fd, (struct sockaddr *)&peer, &peerlen);
    if (client < 0) {
        return SSH_OK; /* spurious readiness: nothing to do */
    }
    if (peer.sin_family == AF_INET) {
        const char *seen = inet_ntoa(peer.sin_addr);

        if (seen != NULL) {
            snprintf(ip, sizeof(ip), "%s", seen);
        }
    }
    index = slot_alloc();
    if (index < 0) {
        sh_console_log("refused %s: %d sessions already open", ip,
                       SSHD_MAX_SESSIONS);
        close((int)client);
        return SSH_OK;
    }

    ssh = ssh_new();
    if (ssh == NULL) {
        sh_console_log("refused %s: out of memory", ip);
        close((int)client);
        return SSH_OK;
    }
    if (ssh_bind_accept_fd(bind, ssh, client) != SSH_OK) {
        sh_console_log("refused %s: %s", ip, ssh_get_error(ssh));
        ssh_free(ssh);
        close((int)client);
        return SSH_OK;
    }

    sh = sh_session_new(ssh, ip);
    if (sh == NULL) {
        sh_console_log("refused %s: session allocation failed", ip);
        ssh_free(ssh);
        close((int)client);
        return SSH_OK;
    }
    if (ssh_handle_key_exchange(ssh) != SSH_OK) {
        /* Reaching here and failing is normal for a port scan or a client that
         * does not speak the same kex: log it, but do not treat it as an error
         * worth more than one line. */
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
    sh_console_log("connection from %s (%d open)", ip, count_sessions());
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
    /* IMPORT_KEY hands libssh the key we already hold. SSH_BIND_OPTIONS_HOSTKEY
     * would make libssh re-read it off storage, which fails on exactly the device
     * where the save did not work -- and that is a server with no identity rather
     * than a server with a warning. */
    if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key) != 0) {
        sh_console_log("host key rejected by libssh");
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

static void server_loop(ssh_bind bind, const sh_auth_t *auth, vita2d_pgf *font)
{
    unsigned long long next_draw = 0;
    char ip[64];

    local_ip(ip, sizeof(ip));
    sh_console_log("ready: ssh any@%s -p %d", ip, listen_port);

    while (!quit_requested) {
        int i;

        if (ssh_event_dopoll(poll_event, 50) == SSH_ERROR) {
            /* dopoll reports an error for any session's socket; the per-session
             * check right below is what decides who to drop, so there is
             * nothing to do here but keep going. */
        }
        for (i = 0; i < SSHD_MAX_SESSIONS; i++) {
            int status;

            if (!slots[i].used) {
                continue;
            }
            if (sh_session_pump(slots[i].sh) != 0) {
                sh_console_log("session %s closed", sh_session_peer(slots[i].sh));
                slot_free(i);
                continue;
            }
            status = ssh_get_status(slots[i].ssh);
            if ((status & (SSH_CLOSED | SSH_CLOSED_ERROR)) != 0) {
                sh_console_log("session %s dropped (socket closed)",
                               sh_session_peer(slots[i].sh));
                slot_free(i);
            }
        }
        if (start_pressed()) {
            sh_console_log("START held: stopping");
            break;
        }
        {
            unsigned long long now = sceKernelGetProcessTimeWide();

            if (now >= next_draw) {
                draw_console(font, auth, ip);
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

    /* Before anything else, because everything worth knowing about a run that
     * dies early is decided before anything else happens: the last line in the
     * log is the stage the app did not come back from. */
    if (sh_log_open() == 0) {
        sh_console_log("=== VitaSSHd starting, log %s ===", sh_log_path());
    } else {
        sh_console_log("=== VitaSSHd starting, no writable log ===");
    }

    vita2d_init();
    vita2d_set_clear_color(RGBA8(16, 18, 22, 255));
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    font = vita2d_load_default_pgf();
    if (font == NULL) {
        /* Nothing can be shown without the font, but the log file reaches the
         * user the same way, so say what happened there and leave the device in
         * a state they can still recover from. */
        sh_console_log("error: the console font did not load, cannot start");
        sh_log_close();
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

    if (ssh_init() != SSH_OK) {
        sh_console_log("error: ssh_init failed");
    } else if (bring_up_network() != 0) {
        /* keep going: the console must show why nothing is listening */
    } else if (sh_auth_prepare(&auth) != 0) {
        sh_console_log("error: cannot start without a host key");
    } else {
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
            server_loop(bind, &auth, font);
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
    sh_log_close();
    sceKernelExitDeleteThread(0);
    return 0;
}

int main(void)
{
    /* Arg 6 is an int cpuAffinityMask, not a pointer: 0 lets the kernel pick. */
    SceUID thread = sceKernelCreateThread("sshd", server_thread, 0x10000100,
                                          SERVER_STACK, 0, 0, NULL);

    if (thread < 0) {
        return 1;
    }
    if (sceKernelStartThread(thread, 0, NULL) != 0) {
        return 1;
    }
    /* The main thread's only job is to stay alive until the server finishes:
     * if it exited, the process would go with it. */
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelExitProcess(0);
    return 0;
}
