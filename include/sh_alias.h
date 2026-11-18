/* Per-session shell aliases: a bounded table with no I/O and no Vita APIs. */
#ifndef SH_ALIAS_H
#define SH_ALIAS_H

/* Most aliases one session holds. */
#define SH_ALIAS_MAX 32

/* Longest name and longest value, not counting the terminator. */
#define SH_ALIAS_NAME_MAX 32
#define SH_ALIAS_VALUE_MAX 256

typedef struct sh_alias_table sh_alias_table_t;

/* An empty table; NULL when memory is short. */
sh_alias_table_t *sh_alias_new(void);
void sh_alias_free(sh_alias_table_t *t);

/* Adds l, ll, la, `..` and `...`. Returns 0, or -1 when the table cannot hold them. */
int sh_alias_seed(sh_alias_table_t *t);

/* A name is 1 to SH_ALIAS_NAME_MAX letters, digits or one of _ . - + , : @ % ^ , so it never needs quoting. */
int sh_alias_name_ok(const char *name);

/* Defines or replaces name. Returns 0, or -1 with errno: EINVAL (bad name, blank value, NULL table),
 * ENAMETOOLONG (name or value past its limit), ENOSPC (a new name when the table is full), or ELOOP
 * when the value would close a ring of aliases (a -> b -> a). A value whose first word is name itself
 * is not a ring (`alias ls='ls -l'`). Bash allows a ring and fails when it runs; this refuses it. */
int sh_alias_set(sh_alias_table_t *t, const char *name, const char *value);

/* Removes name. Returns 0, or -1 with ENOENT. */
int sh_alias_unset(sh_alias_table_t *t, const char *name);

/* How a command word expands, as in bash: the alias for word, then the alias for the first word of that
 * value, and so on, each alias at most once, so alias ls='ls -l' reaches the builtin ls. Fills out with
 * the values to apply in order (borrowed from the table, valid until it changes) and returns how many,
 * at most max; 0 when word is not an alias. */
int sh_alias_chain(const sh_alias_table_t *t, const char *word, const char *out[], int max);

/* The value, or NULL when name is not an alias or t is NULL. */
const char *sh_alias_get(const sh_alias_table_t *t, const char *name);

/* The table in name order, for listing: count, then index 0..count-1; NULL past the end. */
int sh_alias_count(const sh_alias_table_t *t);
const char *sh_alias_name_at(const sh_alias_table_t *t, int i);
const char *sh_alias_value_at(const sh_alias_table_t *t, int i);

#endif
