/* What `vpk install` may do with a package, decided from facts the caller gathered: no I/O, no Vita APIs. */
#ifndef VPK_POLICY_H
#define VPK_POLICY_H

typedef enum {
    VPK_OWN = 0,         /* the server's own title: the updater handoff, as before */
    VPK_INSTALL,         /* another title that is not installed and not running */
    VPK_REPLACE,         /* another title that is installed, not running, with the replace option given */
    VPK_BAD_TITLE,       /* not a title ID at all */
    VPK_REFUSE_UPDATER,  /* the bundled updater title, which only the server installs for itself */
    VPK_REFUSE_SYSTEM,   /* a system app (NPXS...), which this path never overwrites */
    VPK_REFUSE_RUNNING,  /* the title is running, and a running title is never replaced or closed */
    VPK_REFUSE_INSTALLED /* installed, and the replace option was not given */
} vpk_verdict_t;

/* A title ID is nine characters from A-Z and 0-9, the first a letter: ROM000070, MBVK00001, VITASHELL. */
int vpk_title_ok(const char *title);

/* Whether title names a system app; those IDs begin NPXS. */
int vpk_title_is_system(const char *title);

/* Decides from the package's title, the server's own and updater titles, whether the title is installed
 * and whether it is running, and whether the replace option was given. The order of the checks is: a bad
 * title, the own title, the updater, a system app, a running title, then installed against replace. */
vpk_verdict_t vpk_decide(const char *title, const char *own, const char *updater, int installed, int running,
                         int replace);

/* A stable lowercase word for each verdict, for `vpk list` and for tests. */
const char *vpk_verdict_name(vpk_verdict_t v);

#endif
