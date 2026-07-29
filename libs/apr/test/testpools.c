/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "fspr_general.h"
#include "fspr_pools.h"
#include "fspr_errno.h"
#include "fspr_file_io.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif
#include "testutil.h"

#define ALLOC_BYTES 1024

static fspr_pool_t *pmain = NULL;
static fspr_pool_t *pchild = NULL;

static void alloc_bytes(abts_case *tc, void *data)
{
    int i;
    char *alloc;
    
    alloc = fspr_palloc(pmain, ALLOC_BYTES);
    ABTS_PTR_NOTNULL(tc, alloc);

    for (i=0;i<ALLOC_BYTES;i++) {
        char *ptr = alloc + i;
        *ptr = 0xa;
    }
    /* This is just added to get the positive.  If this test fails, the
     * suite will seg fault.
     */
    ABTS_TRUE(tc, 1);
}

static void calloc_bytes(abts_case *tc, void *data)
{
    int i;
    char *alloc;
    
    alloc = fspr_pcalloc(pmain, ALLOC_BYTES);
    ABTS_PTR_NOTNULL(tc, alloc);

    for (i=0;i<ALLOC_BYTES;i++) {
        char *ptr = alloc + i;
        ABTS_TRUE(tc, *ptr == '\0');
    }
}

static void parent_pool(abts_case *tc, void *data)
{
    fspr_status_t rv;

    rv = fspr_pool_create(&pmain, NULL);
    ABTS_INT_EQUAL(tc, rv, APR_SUCCESS);
    ABTS_PTR_NOTNULL(tc, pmain);
}

static void child_pool(abts_case *tc, void *data)
{
    fspr_status_t rv;

    rv = fspr_pool_create(&pchild, pmain);
    ABTS_INT_EQUAL(tc, rv, APR_SUCCESS);
    ABTS_PTR_NOTNULL(tc, pchild);
}

static void test_ancestor(abts_case *tc, void *data)
{
    ABTS_INT_EQUAL(tc, 1, fspr_pool_is_ancestor(pmain, pchild));
}

static void test_notancestor(abts_case *tc, void *data)
{
    ABTS_INT_EQUAL(tc, 0, fspr_pool_is_ancestor(pchild, pmain));
}

static fspr_status_t success_cleanup(void *data)
{
    return APR_SUCCESS;
}

static char *checker_data = "Hello, world.";

static fspr_status_t checker_cleanup(void *data)
{
    return data == checker_data ? APR_SUCCESS : APR_EGENERAL;
}

static void test_cleanups(abts_case *tc, void *data)
{
    fspr_status_t rv;
    int n;

    /* do this several times to test the cleanup freelist handling. */
    for (n = 0; n < 5; n++) {
        fspr_pool_cleanup_register(pchild, NULL, success_cleanup,
                                  success_cleanup);
        fspr_pool_cleanup_register(pchild, checker_data, checker_cleanup,
                                  success_cleanup);
        fspr_pool_cleanup_register(pchild, NULL, checker_cleanup, 
                                  success_cleanup);

        rv = fspr_pool_cleanup_run(p, NULL, success_cleanup);
        ABTS_ASSERT(tc, "nullop cleanup run OK", rv == APR_SUCCESS);
        rv = fspr_pool_cleanup_run(p, checker_data, checker_cleanup);
        ABTS_ASSERT(tc, "cleanup passed correct data", rv == APR_SUCCESS);
        rv = fspr_pool_cleanup_run(p, NULL, checker_cleanup);
        ABTS_ASSERT(tc, "cleanup passed correct data", rv == APR_EGENERAL);

        if (n == 2) {
            /* clear the pool to check that works */
            fspr_pool_clear(pchild);
        }

        if (n % 2 == 0) {
            /* throw another random cleanup into the mix */
            fspr_pool_cleanup_register(pchild, NULL,
                                      fspr_pool_cleanup_null,
                                      fspr_pool_cleanup_null);
        }
    }
}

/* TELCORE-302 cleanup-list tests -- exercise the REAL fspr_pool_cleanup_*
 * functions (not a reimplemented copy). */

static int cleanup_marks[8];

static fspr_status_t counting_cleanup(void *data)
{
    cleanup_marks[*(int *) data]++;
    return APR_SUCCESS;
}

static fspr_status_t noop_cleanup(void *data)
{
    (void) data;
    return APR_SUCCESS;
}

/* Real fspr_pool_cleanup_kill removes head/middle/tail correctly and is a no-op
 * for an unregistered entry; real run_cleanups (via pool destroy) then runs each
 * survivor exactly once. Guards against drift in the Floyd-modified kill walk. */
static void test_cleanup_kill_and_run(abts_case *tc, void *data)
{
    static int idx[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    fspr_pool_t *pcln = NULL;
    fspr_status_t rv;
    int i;

    rv = fspr_pool_create(&pcln, pmain);
    ABTS_INT_EQUAL(tc, APR_SUCCESS, rv);

    memset(cleanup_marks, 0, sizeof(cleanup_marks));

    /* register prepends: list head -> idx4,idx3,idx2,idx1,idx0 */
    for (i = 0; i < 5; i++) {
        fspr_pool_cleanup_register(pcln, &idx[i], counting_cleanup, counting_cleanup);
    }

    fspr_pool_cleanup_kill(pcln, &idx[2], counting_cleanup); /* middle */
    fspr_pool_cleanup_kill(pcln, &idx[4], counting_cleanup); /* head   */
    fspr_pool_cleanup_kill(pcln, &idx[0], counting_cleanup); /* tail   */
    fspr_pool_cleanup_kill(pcln, &idx[7], counting_cleanup); /* absent -> no-op */

    fspr_pool_destroy(pcln); /* runs survivors idx1, idx3 exactly once */

    ABTS_INT_EQUAL(tc, 0, cleanup_marks[0]); /* killed (tail)   */
    ABTS_INT_EQUAL(tc, 1, cleanup_marks[1]); /* survived, once  */
    ABTS_INT_EQUAL(tc, 0, cleanup_marks[2]); /* killed (middle) */
    ABTS_INT_EQUAL(tc, 1, cleanup_marks[3]); /* survived, once  */
    ABTS_INT_EQUAL(tc, 0, cleanup_marks[4]); /* killed (head)   */
    ABTS_INT_EQUAL(tc, 0, cleanup_marks[7]); /* never registered*/
}

/* Mirrors rtp_get_pool_sock_mutex: userdata attached to a pool is shared -- a
 * later lookup on the SAME pool returns the identical object. This is what makes
 * the TELCORE-302 socket lock pool-scoped (shared across audio/video/T.38 RTP
 * sessions that share the session pool), not per-rtp_session. */
static void test_pool_userdata_shared(abts_case *tc, void *data)
{
    fspr_pool_t *pu = NULL;
    void *got1 = NULL, *got2 = NULL;
    int *slot;
    fspr_status_t rv;

    rv = fspr_pool_create(&pu, pmain);
    ABTS_INT_EQUAL(tc, APR_SUCCESS, rv);

    fspr_pool_userdata_get(&got1, "_rtp_sock_mutex", pu);
    ABTS_PTR_EQUAL(tc, NULL, got1);              /* absent before first create */

    slot = fspr_palloc(pu, sizeof(*slot));
    fspr_pool_userdata_set(slot, "_rtp_sock_mutex", NULL, pu);

    fspr_pool_userdata_get(&got1, "_rtp_sock_mutex", pu);
    fspr_pool_userdata_get(&got2, "_rtp_sock_mutex", pu);
    ABTS_PTR_NOTNULL(tc, got1);
    ABTS_PTR_EQUAL(tc, slot, got1);              /* returns exactly what was set */
    ABTS_PTR_EQUAL(tc, got1, got2);              /* every caller gets the same one */

    fspr_pool_destroy(pu);
}

#ifdef APR_POOL_CLEANUP_CYCLE_TEST
/* With the list corrupted into a cycle, real fspr_pool_cleanup_kill must TERMINATE
 * (Floyd) and real run_cleanups (via destroy) must detect + skip -- not spin and
 * not double-free. A regression that drops the guards hangs here and CI times out.
 * Built only with -DAPR_POOL_CLEANUP_CYCLE_TEST. */
static void test_cleanup_cycle(abts_case *tc, void *data)
{
    static int absent = 99;
    fspr_pool_t *pcyc = NULL;
    fspr_status_t rv;

    rv = fspr_pool_create(&pcyc, pmain);
    ABTS_INT_EQUAL(tc, APR_SUCCESS, rv);

    fspr_pool_make_cleanup_cycle_for_testing(pcyc, 3, noop_cleanup);

    fspr_pool_cleanup_kill(pcyc, &absent, noop_cleanup); /* must return, not spin */
    fspr_pool_destroy(pcyc);                             /* must skip, not UAF/spin */

    ABTS_TRUE(tc, 1); /* reached only if neither call hung/crashed */
}
#endif

abts_suite *testpool(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, parent_pool, NULL);
    abts_run_test(suite, child_pool, NULL);
    abts_run_test(suite, test_ancestor, NULL);
    abts_run_test(suite, test_notancestor, NULL);
    abts_run_test(suite, alloc_bytes, NULL);
    abts_run_test(suite, calloc_bytes, NULL);
    abts_run_test(suite, test_cleanups, NULL);
    abts_run_test(suite, test_cleanup_kill_and_run, NULL);
    abts_run_test(suite, test_pool_userdata_shared, NULL);
#ifdef APR_POOL_CLEANUP_CYCLE_TEST
    abts_run_test(suite, test_cleanup_cycle, NULL);
#endif

    return suite;
}

