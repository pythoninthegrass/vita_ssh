/* Host test for src/sh_lineq.c; driven by scripts/test_sh_lineq.py. */
#include "sh_lineq.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            failures++;                                                             \
        }                                                                           \
    } while (0)

static int push_fails(sh_lineq_t *q, const char *line, int want)
{
    errno = 0;
    return sh_lineq_push(q, line) == -1 && errno == want;
}

static int pop_fails(sh_lineq_t *q, char *buf, size_t len, int want)
{
    errno = 0;
    return sh_lineq_pop(q, buf, len) == -1 && errno == want;
}

static void test_order(void)
{
    sh_lineq_t q;
    char buf[64];

    sh_lineq_init(&q);
    CHECK(sh_lineq_len(&q) == 0);
    CHECK(pop_fails(&q, buf, sizeof(buf), ENOENT));
    CHECK(sh_lineq_push(&q, "cd /ux0/music") == 0);
    CHECK(sh_lineq_push(&q, "printenv PWD") == 0);
    CHECK(sh_lineq_len(&q) == 2);
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, "cd /ux0/music") == 0);
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, "printenv PWD") == 0);
    CHECK(sh_lineq_len(&q) == 0);
    CHECK(pop_fails(&q, buf, sizeof(buf), ENOENT));
    sh_lineq_clear(&q);
}

static void test_empty_line(void)
{
    sh_lineq_t q;
    char buf[8] = "junk";

    sh_lineq_init(&q);
    CHECK(sh_lineq_push(&q, "") == 0);
    CHECK(sh_lineq_len(&q) == 1);
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    sh_lineq_clear(&q);
}

static void test_full_refuses_and_keeps_earlier(void)
{
    sh_lineq_t q;
    char line[16], buf[16];
    int i;

    sh_lineq_init(&q);
    for (i = 0; i < SH_LINEQ_MAX; i++) {
        snprintf(line, sizeof(line), "line%d", i);
        CHECK(sh_lineq_push(&q, line) == 0);
    }
    CHECK(sh_lineq_len(&q) == SH_LINEQ_MAX);
    CHECK(push_fails(&q, "overflow", ENOSPC));
    CHECK(sh_lineq_len(&q) == SH_LINEQ_MAX);
    for (i = 0; i < SH_LINEQ_MAX; i++) {
        snprintf(line, sizeof(line), "line%d", i);
        CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, line) == 0);
    }
    /* A slot freed by a pop is usable again. */
    CHECK(sh_lineq_push(&q, "again") == 0);
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, "again") == 0);
    sh_lineq_clear(&q);
}

static void test_small_buffer_keeps_line(void)
{
    sh_lineq_t q;
    char small[4], buf[32];

    sh_lineq_init(&q);
    CHECK(sh_lineq_push(&q, "longer than four") == 0);
    CHECK(pop_fails(&q, small, sizeof(small), ENAMETOOLONG));
    CHECK(sh_lineq_len(&q) == 1);
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, "longer than four") == 0);
    CHECK(push_fails(&q, NULL, EINVAL));
    CHECK(push_fails(NULL, "x", EINVAL));
    CHECK(pop_fails(&q, NULL, 8, EINVAL));
    CHECK(pop_fails(&q, buf, 0, EINVAL));
    sh_lineq_clear(&q);
}

static void test_copy_is_independent(void)
{
    sh_lineq_t q;
    char src[] = "abc", buf[8];

    sh_lineq_init(&q);
    CHECK(sh_lineq_push(&q, src) == 0);
    src[0] = 'X';
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, "abc") == 0);
    sh_lineq_clear(&q);
}

static void test_clear_drops_everything(void)
{
    sh_lineq_t q;
    char buf[8];

    sh_lineq_init(&q);
    sh_lineq_push(&q, "a");
    sh_lineq_push(&q, "b");
    sh_lineq_clear(&q);
    CHECK(sh_lineq_len(&q) == 0);
    CHECK(pop_fails(&q, buf, sizeof(buf), ENOENT));
    CHECK(sh_lineq_push(&q, "c") == 0);
    CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, "c") == 0);
    sh_lineq_clear(&q);
}

/* Every sequence of push/pop operations up to MODEL_DEPTH long, against a plain array model. */
#define MODEL_DEPTH 12

static void run_sequence(unsigned bits)
{
    sh_lineq_t q;
    int model[MODEL_DEPTH], mlen = 0, next = 0, i;
    char line[16], buf[16];

    sh_lineq_init(&q);
    for (i = 0; i < MODEL_DEPTH; i++) {
        if ((bits >> i) & 1u) {
            snprintf(line, sizeof(line), "n%d", next);
            if (mlen < SH_LINEQ_MAX) {
                CHECK(sh_lineq_push(&q, line) == 0);
                model[mlen++] = next;
            } else {
                CHECK(push_fails(&q, line, ENOSPC));
            }
            next++;
        } else if (mlen > 0) {
            snprintf(line, sizeof(line), "n%d", model[0]);
            CHECK(sh_lineq_pop(&q, buf, sizeof(buf)) == 0 && strcmp(buf, line) == 0);
            memmove(model, model + 1, (size_t)(mlen - 1) * sizeof(model[0]));
            mlen--;
        } else {
            CHECK(pop_fails(&q, buf, sizeof(buf), ENOENT));
        }
        CHECK(sh_lineq_len(&q) == mlen);
    }
    sh_lineq_clear(&q);
}

static void test_model(void)
{
    unsigned bits;

    for (bits = 0; bits < (1u << MODEL_DEPTH); bits++) {
        run_sequence(bits);
    }
    /* All pushes: forces the full path past SH_LINEQ_MAX with the 12-step depth. */
    CHECK(MODEL_DEPTH > SH_LINEQ_MAX);
}

int main(void)
{
    test_order();
    test_empty_line();
    test_full_refuses_and_keeps_earlier();
    test_small_buffer_keeps_line();
    test_copy_is_independent();
    test_clear_drops_everything();
    test_model();
    if (failures == 0) {
        printf("sh_lineq: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
