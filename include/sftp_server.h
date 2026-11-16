#ifndef SFTP_SERVER_H_VITA
#define SFTP_SERVER_H_VITA

/*
 * SFTP for the sshd app: libssh's stock server handlers, adapted at the libc
 * boundary rather than forked.
 *
 * The stock handlers call open/opendir/stat/mkdir/... with whatever path the
 * client sent, and the Vita's libc only understands "ux0:/..." names. The
 * server target links with -Wl,--wrap on those calls, and while a data callback
 * is running the wrappers send every path through sh_path.c first. Outside that
 * window they are pass-throughs, so the shell builtins are unaffected.
 *
 * Uploads are written to "<name>.part" and renamed over the target when the
 * client closes the handle, so an interrupted transfer never leaves a short
 * file under the real name.
 */

#include <stdint.h>

#include <libssh/libssh.h>

#include "sh_path.h"

typedef struct sh_sftp sh_sftp_t;

/* Starts the SFTP server on an accepted "sftp" subsystem request. NULL on
 * failure. */
sh_sftp_t *sh_sftp_new(ssh_session session, ssh_channel channel);

/* Feeds channel data in. Returns 0, or -1 when the stream is unusable and the
 * channel should be closed. fs supplies the session's cwd and home for
 * relative paths. */
int sh_sftp_data(sh_sftp_t *sftp, const sh_fsctx_t *fs, const void *data,
                 uint32_t len);

/* Closes every handle the client left open and removes the .part files of
 * uploads that never finished. */
void sh_sftp_free(sh_sftp_t *sftp);

#endif /* SFTP_SERVER_H_VITA */
