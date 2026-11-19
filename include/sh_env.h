/* Per-session environment variables: a bounded table with no I/O and no Vita APIs. */
#ifndef SH_ENV_H
#define SH_ENV_H

#include <stddef.h>

/* Most variables one session holds. */
#define SH_ENV_MAX 32

/* Longest name and longest value, not counting the terminator. */
#define SH_ENV_NAME_MAX 32
#define SH_ENV_VALUE_MAX 256

typedef struct sh_env_table sh_env_table_t;

/* An empty table; NULL when memory is short. */
sh_env_table_t *sh_env_new(void);
void sh_env_free(sh_env_table_t *t);

/* A name is 1 to SH_ENV_NAME_MAX letters, digits or underscores, and does not start with a digit. */
int sh_env_name_ok(const char *name);

/* Copies src into dst (size bytes, terminator included) without the bytes below 0x20 and 0x7F,
 * truncating at size - 1. Returns the length written; a NULL src or a size of 0 writes nothing
 * beyond an empty string. For text that came from a client and reaches a terminal or a log. */
size_t sh_env_strip_ctrl(char *dst, size_t size, const char *src);

/* Defines or replaces name. Bytes below 0x20 and 0x7F are dropped from value, because a value
 * can come from a client and reaches the terminal. Returns 0, or -1 with errno: EINVAL (bad name,
 * NULL table or value), ENAMETOOLONG (name or cleaned value past its limit), ENOSPC (a new name
 * when the table is full). */
int sh_env_set(sh_env_table_t *t, const char *name, const char *value);

/* Removes name, keeping the order of the others. Returns 0, or -1 with errno: EINVAL (NULL table or
 * name), ENOENT (not set). Does not consult readonly status: that is the caller's decision. */
int sh_env_unset(sh_env_table_t *t, const char *name);

/* Marks a set variable as supplied by the client, so a user's assignment must leave it alone. The mark
 * lasts as long as the variable. Returns 0, or -1 with ENOENT when name is not set. */
int sh_env_lock(sh_env_table_t *t, const char *name);

/* True for the server-owned names HOME, PWD, USER, LOGNAME and TMPDIR (set or not), and for a locked
 * variable. Only the shell's own assignment and unset ask; sh_env_set itself never refuses. */
int sh_env_is_readonly(const sh_env_table_t *t, const char *name);

/* A copy of the whole table, locks included, for a caller that must undo what a command did; NULL when t
 * is NULL or memory is short. sh_env_restore puts it back and frees it; both tolerate NULL. */
sh_env_table_t *sh_env_snapshot(const sh_env_table_t *t);
void sh_env_restore(sh_env_table_t *t, sh_env_table_t *saved);

/* The value, or NULL when name is not set or t is NULL. */
const char *sh_env_get(const sh_env_table_t *t, const char *name);

/* The table in the order names were first set: count, then index 0..count-1; NULL past the end. */
int sh_env_count(const sh_env_table_t *t);
const char *sh_env_name_at(const sh_env_table_t *t, int i);
const char *sh_env_value_at(const sh_env_table_t *t, int i);

#endif
