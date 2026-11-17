/* Which SFTP opens are staged to <target>.part and promoted on close. */
#ifndef SFTP_OPEN_POLICY_H
#define SFTP_OPEN_POLICY_H

#include <fcntl.h>

/* OpenSSH scp opens with WRITE|CREAT and never sends TRUNC, so a create open that is not
 * an append, an exclusive create or a read-write open without TRUNC replaces the file. */
static inline int sftp_open_stages(int flags)
{
    if (!(flags & O_CREAT) || (flags & (O_APPEND | O_EXCL))) {
        return 0;
    }
    return (flags & O_TRUNC) || !(flags & O_RDWR);
}

#endif
