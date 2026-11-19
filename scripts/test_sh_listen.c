/* Host test for src/sh_listen.c; driven by scripts/test_sh_listen.py. */
#include "sh_listen.h"

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

/* The wait after the Nth consecutive failure since the last accepted connection, written out from the specification. */
static const unsigned long long wait_after[] = {0,        500000,   1000000,  2000000,
                                                4000000,  8000000,  16000000, 30000000};
#define WAITS ((int)(sizeof(wait_after) / sizeof(wait_after[0])))

enum { EV_FAILED, EV_REBUILT, EV_ACCEPTED, EV_TICK_SMALL, EV_TICK_MEDIUM, EV_TICK_LONG, EV_COUNT };
static const unsigned long long tick_us[] = {100000, 600000, 40000000};

typedef struct {
    int down;
    int failures_in_a_row;
    unsigned long long retry_at;
} model_t;

static void apply(sh_listen_t *t, model_t *m, unsigned long long *now, int ev)
{
    switch (ev) {
    case EV_FAILED:
        sh_listen_failed(t, *now);
        m->down = 1;
        m->retry_at = *now + wait_after[m->failures_in_a_row < WAITS ? m->failures_in_a_row : WAITS - 1];
        m->failures_in_a_row++;
        break;
    case EV_REBUILT:
        sh_listen_rebuilt(t);
        m->down = 0;
        break;
    case EV_ACCEPTED:
        sh_listen_accepted(t);
        m->failures_in_a_row = 0;
        break;
    default:
        *now += tick_us[ev - EV_TICK_SMALL];
        break;
    }
}

static int agrees(const sh_listen_t *t, const model_t *m, unsigned long long now)
{
    return sh_listen_is_down(t) == m->down && sh_listen_due(t, now) == (m->down && now >= m->retry_at);
}

static void run_sequence(const int *events, int len)
{
    sh_listen_t t;
    model_t m;
    unsigned long long now = 1000000;
    int i;

    sh_listen_init(&t);
    memset(&m, 0, sizeof(m));
    for (i = 0; i < len; i++) {
        apply(&t, &m, &now, events[i]);
        if (!agrees(&t, &m, now)) {
            printf("FAIL sequence of %d, diverged at step %d\n", len, i);
            failures++;
            return;
        }
    }
}

static void sweep_all(int max_len)
{
    int seq[8];
    int len, i;
    long total, n;

    for (len = 1; len <= max_len; len++) {
        total = 1;
        for (i = 0; i < len; i++) {
            total *= EV_COUNT;
        }
        for (n = 0; n < total; n++) {
            long rest = n;

            for (i = 0; i < len; i++) {
                seq[i] = (int)(rest % EV_COUNT);
                rest /= EV_COUNT;
            }
            run_sequence(seq, len);
        }
    }
}

static void test_fresh_is_up(void)
{
    sh_listen_t t;

    sh_listen_init(&t);
    CHECK(!sh_listen_is_down(&t));
    CHECK(!sh_listen_due(&t, 0));
    CHECK(!sh_listen_due(&t, 1ULL << 60));
}

static void test_first_failure_is_due_at_once(void)
{
    sh_listen_t t;

    sh_listen_init(&t);
    sh_listen_failed(&t, 5000000);
    CHECK(sh_listen_is_down(&t));
    CHECK(sh_listen_due(&t, 5000000));
}

static void test_failed_rebuild_waits(void)
{
    sh_listen_t t;

    sh_listen_init(&t);
    sh_listen_failed(&t, 5000000);
    sh_listen_failed(&t, 5000000);
    CHECK(!sh_listen_due(&t, 5000000 + SH_LISTEN_BACKOFF_FIRST_US - 1));
    CHECK(sh_listen_due(&t, 5000000 + SH_LISTEN_BACKOFF_FIRST_US));
}

static void test_backoff_is_capped(void)
{
    sh_listen_t t;
    int i;

    sh_listen_init(&t);
    for (i = 0; i < 100; i++) {
        sh_listen_failed(&t, 0);
    }
    CHECK(!sh_listen_due(&t, SH_LISTEN_BACKOFF_MAX_US - 1));
    CHECK(sh_listen_due(&t, SH_LISTEN_BACKOFF_MAX_US));
}

static void test_rebuilt_keeps_backoff_until_accepted(void)
{
    sh_listen_t t;

    sh_listen_init(&t);
    sh_listen_failed(&t, 0);
    sh_listen_rebuilt(&t);
    CHECK(!sh_listen_is_down(&t));
    sh_listen_failed(&t, 1000000);
    CHECK(!sh_listen_due(&t, 1000000));
    CHECK(sh_listen_due(&t, 1000000 + SH_LISTEN_BACKOFF_FIRST_US));
    sh_listen_rebuilt(&t);
    sh_listen_accepted(&t);
    sh_listen_failed(&t, 9000000);
    CHECK(sh_listen_due(&t, 9000000));
}

int main(void)
{
    test_fresh_is_up();
    test_first_failure_is_due_at_once();
    test_failed_rebuild_waits();
    test_backoff_is_capped();
    test_rebuilt_keeps_backoff_until_accepted();
    sweep_all(7);
    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("sh_listen: all checks passed\n");
    return 0;
}
