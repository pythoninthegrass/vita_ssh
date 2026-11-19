/* Variable expansion for one shell word: no I/O, no Vita APIs, no allocation. */
#ifndef SH_EXPAND_H
#define SH_EXPAND_H

#include <stddef.h>

/* The line splitter writes this byte for a `$` that must stay text (single-quoted or backslashed),
 * and sh_expand_word turns it back into `$`. */
#define SH_EXPAND_LITERAL_DOLLAR '\x01'

/* ...and this one for a `$` inside double quotes: it starts a reference like `$`, but the value is not
 * split into fields. */
#define SH_EXPAND_QUOTED_DOLLAR '\x02'

typedef enum {
    SH_EXPAND_OK = 0,
    SH_EXPAND_UNSUPPORTED, /* a construct that is not implemented: $(...), $((...)), ${NAME:-x}, $1, $$ and the other special parameters */
    SH_EXPAND_MALFORMED,   /* a `${` that is empty, unterminated or not a name */
    SH_EXPAND_TOOLONG      /* the result does not fit in the buffer */
} sh_expand_status_t;

typedef struct {
    int refs;       /* references replaced, set or not */
    int nfields;    /* fields written to out */
    char what[32];  /* on UNSUPPORTED or MALFORMED, the text of the construct as written, cut short */
} sh_expand_info_t;

/* The value of name, or NULL when it is not set. `$?` asks for the name "?". */
typedef const char *(*sh_expand_lookup_fn)(void *ctx, const char *name);

/* Replaces `$NAME`, `${NAME}`, `$?` and `${?}` in word with the looked-up value (nothing for an unset name) and turns
 * SH_EXPAND_LITERAL_DOLLAR into `$`. The result is written to out (cap bytes in all) as nfields strings,
 * each with its terminator, back to back.
 *
 * Field splitting follows sh. The value of a reference that began with `$` is split at the characters of
 * ifs: runs of space, tab and newline in ifs separate fields and are trimmed at both ends, while any
 * other ifs character ends a field by itself, so `a::b` on ":" gives a, an empty field and b. ifs NULL
 * means " \t\n" and "" means no splitting. Text typed in the word and the value of a reference that
 * began with SH_EXPAND_QUOTED_DOLLAR are never split. A word that produces no characters from unquoted
 * references yields no field; a quoted reference always yields one, even if empty. A value is copied as
 * is and never scanned again. A `$` followed by anything else that cannot start a reference stays a `$`.
 * info is filled in even on failure; out is only meaningful on SH_EXPAND_OK. */
sh_expand_status_t sh_expand_word(const char *word, sh_expand_lookup_fn lookup, void *ctx, const char *ifs, char *out,
                                  size_t cap, sh_expand_info_t *info);

#endif
