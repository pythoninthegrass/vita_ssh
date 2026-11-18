/* Host test for src/vpk_policy.c; driven by scripts/test_vpk_policy.py. */
#include "vpk_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            failures++;                                                             \
        }                                                                           \
    } while (0)

#define OWN "ROM000070"
#define UPD "ROM000072"

static vpk_verdict_t decide(const char *title, int installed, int running, int replace)
{
    return vpk_decide(title, OWN, UPD, installed, running, replace);
}

static void test_title_ok(void)
{
    CHECK(vpk_title_ok("ROM000070"));
    CHECK(vpk_title_ok("MBVK00001"));
    CHECK(vpk_title_ok("VITASHELL"));
    CHECK(vpk_title_ok("NPXS10015"));
    CHECK(vpk_title_ok("A00000000"));
    CHECK(vpk_title_ok("ZZZZZZZZZ"));

    CHECK(!vpk_title_ok(NULL));
    CHECK(!vpk_title_ok(""));
    CHECK(!vpk_title_ok("SHORT"));
    CHECK(!vpk_title_ok("MBVK0000"));   /* eight */
    CHECK(!vpk_title_ok("MBVK000011")); /* ten */
    CHECK(!vpk_title_ok("mbvk00001"));
    CHECK(!vpk_title_ok("MbVK00001"));
    CHECK(!vpk_title_ok("1BCDEFGHI")); /* starts with a digit */
    CHECK(!vpk_title_ok("MB-K00001"));
    CHECK(!vpk_title_ok("MBVK 0001"));
    CHECK(!vpk_title_ok("MBVK00001\n"));
    CHECK(!vpk_title_ok("../../ux0"));
    CHECK(!vpk_title_ok("MBVK0000/"));
}

static void test_system(void)
{
    CHECK(vpk_title_is_system("NPXS10015"));
    CHECK(vpk_title_is_system("NPXS99999"));
    CHECK(vpk_title_is_system("NPXSXXXXX"));
    CHECK(!vpk_title_is_system("NPX000000"));
    CHECK(!vpk_title_is_system("XNPXS1000"));
    CHECK(!vpk_title_is_system("MBVK00001"));
    CHECK(!vpk_title_is_system(NULL));
    CHECK(!vpk_title_is_system(""));
}

static void test_names(void)
{
    CHECK(strcmp(vpk_verdict_name(VPK_OWN), "own") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_INSTALL), "install") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_REPLACE), "replace") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_BAD_TITLE), "bad-title") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_REFUSE_UPDATER), "updater") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_REFUSE_SYSTEM), "system") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_REFUSE_RUNNING), "running") == 0);
    CHECK(strcmp(vpk_verdict_name(VPK_REFUSE_INSTALLED), "installed") == 0);
}

/* The acceptance criteria, one at a time. */
static void test_cases(void)
{
    /* AC1: a third-party title that is neither installed nor running installs. */
    CHECK(decide("MBVK00001", 0, 0, 0) == VPK_INSTALL);
    /* AC2: a running title is refused, even with the replace option. */
    CHECK(decide("MBVK00001", 1, 1, 1) == VPK_REFUSE_RUNNING);
    CHECK(decide("MBVK00001", 0, 1, 0) == VPK_REFUSE_RUNNING);
    /* AC4: an installed title needs the option. */
    CHECK(decide("MBVK00001", 1, 0, 0) == VPK_REFUSE_INSTALLED);
    CHECK(decide("MBVK00001", 1, 0, 1) == VPK_REPLACE);
    /* AC5: the own title keeps the handoff whatever else is true; the updater is refused whatever else. */
    CHECK(decide(OWN, 0, 0, 0) == VPK_OWN);
    CHECK(decide(OWN, 1, 1, 0) == VPK_OWN);
    CHECK(decide(OWN, 1, 1, 1) == VPK_OWN);
    CHECK(decide(UPD, 0, 0, 0) == VPK_REFUSE_UPDATER);
    CHECK(decide(UPD, 1, 0, 1) == VPK_REFUSE_UPDATER);
    CHECK(decide(UPD, 1, 1, 1) == VPK_REFUSE_UPDATER);
    /* A system app is never installed or replaced, with or without the option. */
    CHECK(decide("NPXS10015", 0, 0, 0) == VPK_REFUSE_SYSTEM);
    CHECK(decide("NPXS10015", 1, 0, 1) == VPK_REFUSE_SYSTEM);
    CHECK(decide("NPXS10015", 1, 1, 1) == VPK_REFUSE_SYSTEM);
    /* A malformed title is reported as one before anything else, including the own and updater IDs. */
    CHECK(decide("", 0, 0, 0) == VPK_BAD_TITLE);
    CHECK(decide(NULL, 0, 0, 0) == VPK_BAD_TITLE);
    CHECK(decide("../../ux0", 1, 1, 1) == VPK_BAD_TITLE);
    CHECK(vpk_decide("bad", "bad", "bad", 0, 0, 0) == VPK_BAD_TITLE);
    /* NULL own or updater IDs name nothing. */
    CHECK(vpk_decide("MBVK00001", NULL, NULL, 0, 0, 0) == VPK_INSTALL);
}

/* Every title kind against every combination of installed, running and replace, from a literal table. */
static void test_table(void)
{
    /* index = installed * 4 + running * 2 + replace */
    static const vpk_verdict_t third[8] = {
        VPK_INSTALL,         VPK_INSTALL,        VPK_REFUSE_RUNNING, VPK_REFUSE_RUNNING,
        VPK_REFUSE_INSTALLED, VPK_REPLACE,       VPK_REFUSE_RUNNING, VPK_REFUSE_RUNNING,
    };
    static const struct {
        const char *title;
        const char *kind; /* own, updater, system, third, bad */
    } titles[] = {
        {"ROM000070", "own"},      {"ROM000072", "updater"}, {"NPXS10015", "system"},
        {"NPXS00000", "system"},   {"MBVK00001", "third"},   {"VITASHELL", "third"},
        {"ROM000071", "third"},    {"", "bad"},               {"SHORT", "bad"},
        {"lowercase", "bad"},      {"TOOLONG1234", "bad"},    {"1BCDEFGHI", "bad"},
        {"AB-DEFGHI", "bad"},
    };
    size_t i;
    int f;

    for (i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
        for (f = 0; f < 8; f++) {
            int installed = (f >> 2) & 1, running = (f >> 1) & 1, replace = f & 1;
            vpk_verdict_t want, got = decide(titles[i].title, installed, running, replace);

            if (strcmp(titles[i].kind, "own") == 0) {
                want = VPK_OWN;
            } else if (strcmp(titles[i].kind, "updater") == 0) {
                want = VPK_REFUSE_UPDATER;
            } else if (strcmp(titles[i].kind, "system") == 0) {
                want = VPK_REFUSE_SYSTEM;
            } else if (strcmp(titles[i].kind, "bad") == 0) {
                want = VPK_BAD_TITLE;
            } else {
                want = third[f];
            }
            if (got != want) {
                printf("FAIL table \"%s\" installed=%d running=%d replace=%d: got %s want %s\n", titles[i].title,
                       installed, running, replace, vpk_verdict_name(got), vpk_verdict_name(want));
                failures++;
            }
        }
    }
}

int main(void)
{
    test_title_ok();
    test_system();
    test_names();
    test_cases();
    test_table();
    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
