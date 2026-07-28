/*
 * test_apr_cleanup_cycle.c — TELCORE-302
 *
 * Standalone unit test mirroring the fixed traversal logic of
 * fspr_pool_cleanup_kill in libs/apr/memory/unix/fspr_pools.c
 * (Floyd tortoise/hare cycle detection on the pool cleanup list).
 *
 * It is deliberately self-contained (no FreeSWITCH/APR headers) and is NOT
 * part of the automake build. Compile and run with:
 *
 *   gcc -O2 -Wall -o /tmp/tccc tests/unit/test_apr_cleanup_cycle.c && /tmp/tccc
 *
 * Asserts:
 *   (1) on a normal linear list A->B->C the walk finds and removes the target
 *       node and moves it to the freelist;
 *   (2) on a corrupted 3-node cycle A->B->C->A with the target absent, the
 *       walk TERMINATES (Floyd detection fires) instead of spinning forever.
 *
 * A bounded step counter inside the walk acts as a backstop so a regression
 * fails fast (assert) instead of hanging the test.
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

typedef int (*cleanup_fn_t)(void *);

typedef struct cleanup_t {
    struct cleanup_t *next;
    const void *data;
    cleanup_fn_t plain_cleanup_fn;
} cleanup_t;

typedef struct {
    cleanup_t *cleanups;
    cleanup_t *free_cleanups;
} pool_t;

/* Hard backstop: on the test lists (3 nodes) the fixed walk must terminate in
 * a handful of steps. If a regression removes Floyd detection, this bound
 * makes the test fail fast instead of hanging. */
#define MAX_STEPS 64

static int dummy_cleanup(void *d)
{
    (void)d;
    return 0;
}

/*
 * Mirror of the fixed fspr_pool_cleanup_kill traversal
 * (libs/apr/memory/unix/fspr_pools.c). Returns the number of loop
 * iterations taken, so callers can assert termination bounds.
 */
static int pool_cleanup_kill(pool_t *p, const void *data, cleanup_fn_t cleanup_fn)
{
    cleanup_t *c, **lastp;
    cleanup_t *slow;    /* tortoise for Floyd cycle detection (TELCORE-302) */
    int parity = 0;
    int steps = 0;

    if (p == NULL)
        return 0;

    c = p->cleanups;
    lastp = &p->cleanups;
    slow = p->cleanups;
    while (c) {
        /* backstop: a regression must fail fast, not hang */
        assert(++steps < MAX_STEPS);

        if (c->data == data && c->plain_cleanup_fn == cleanup_fn) {
            *lastp = c->next;
            /* move to freelist */
            c->next = p->free_cleanups;
            p->free_cleanups = c;
            break;
        }

        lastp = &c->next;
        c = c->next;

        /* Floyd tortoise/hare: hare (c) advances every step, tortoise (slow)
         * every other step; if they meet, the list is cyclic — break. */
        parity ^= 1;
        if (parity == 0) {
            slow = slow->next;
            if (c == slow) {
                break;
            }
        }
    }

    return steps;
}

static void test_linear_list_removes_target(void)
{
    cleanup_t a, b, c;
    pool_t p;
    int data_a = 1, data_b = 2, data_c = 3;

    a.data = &data_a; a.plain_cleanup_fn = dummy_cleanup; a.next = &b;
    b.data = &data_b; b.plain_cleanup_fn = dummy_cleanup; b.next = &c;
    c.data = &data_c; c.plain_cleanup_fn = dummy_cleanup; c.next = NULL;

    p.cleanups = &a;
    p.free_cleanups = NULL;

    /* remove the middle node B */
    pool_cleanup_kill(&p, &data_b, dummy_cleanup);

    /* B unlinked: list is now A->C */
    assert(p.cleanups == &a);
    assert(a.next == &c);
    assert(c.next == NULL);

    /* B moved to the freelist */
    assert(p.free_cleanups == &b);
    assert(b.next == NULL);

    printf("test_linear_list_removes_target: PASS\n");
}

static void test_cyclic_list_terminates(void)
{
    cleanup_t a, b, c;
    pool_t p;
    int data_a = 1, data_b = 2, data_c = 3, data_absent = 99;
    int steps;

    /* corrupted 3-node cycle: A -> B -> C -> A */
    a.data = &data_a; a.plain_cleanup_fn = dummy_cleanup; a.next = &b;
    b.data = &data_b; b.plain_cleanup_fn = dummy_cleanup; b.next = &c;
    c.data = &data_c; c.plain_cleanup_fn = dummy_cleanup; c.next = &a;

    p.cleanups = &a;
    p.free_cleanups = NULL;

    /* target absent: without Floyd detection this would spin forever */
    steps = pool_cleanup_kill(&p, &data_absent, dummy_cleanup);

    /* the walk terminated well within the backstop bound */
    assert(steps > 0);
    assert(steps < MAX_STEPS);

    /* nothing was removed and the freelist is untouched */
    assert(p.cleanups == &a);
    assert(p.free_cleanups == NULL);

    printf("test_cyclic_list_terminates: PASS (steps=%d)\n", steps);
}

int main(void)
{
    test_linear_list_removes_target();
    test_cyclic_list_terminates();
    printf("ALL TESTS PASSED\n");
    return 0;
}
