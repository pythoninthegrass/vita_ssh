/* Per-session environment variables: a bounded table with no I/O and no Vita APIs. */
#ifndef SH_ENV_H
#define SH_ENV_H

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

/* Defines or replaces name. Bytes below 0x20 and 0x7F are dropped from value, because a value
 * can come from a client and reaches the terminal. Returns 0, or -1 with errno: EINVAL (bad name,
 * NULL table or value), ENAMETOOLONG (name or cleaned value past its limit), ENOSPC (a new name
 * when the table is full). */
int sh_env_set(sh_env_table_t *t, const char *name, const char *value);

/* The value, or NULL when name is not set or t is NULL. */
const char *sh_env_get(const sh_env_table_t *t, const char *name);

/* The table in the order names were first set: count, then index 0..count-1; NULL past the end. */
int sh_env_count(const sh_env_table_t *t);
const char *sh_env_name_at(const sh_env_table_t *t, int i);
const char *sh_env_value_at(const sh_env_table_t *t, int i);

#endif
