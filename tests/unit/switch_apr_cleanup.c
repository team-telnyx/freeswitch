/*
 * TELCORE-302 -- unit coverage for the APR pool cleanup-list fixes.
 *
 * Exercises the REAL exported fspr_pool_cleanup_* functions (not a copy) so a
 * regression in the Floyd-guarded fspr_pool_cleanup_kill / run_cleanups is
 * caught, plus the pool-userdata sharing that scopes the RTP socket mutex to the
 * (shared) session pool -- the mechanism behind the Layer-A fix in switch_rtp.c.
 *
 * The same assertions run without the full FreeSWITCH toolchain via
 * libs/apr/test/testpools.c (that file additionally hosts the guarded cyclic-list
 * test, which needs APR-internal access). This one is wired into
 * tests/unit/Makefile.am so it runs in the FreeSWITCH unit-test CI.
 */

#include <switch.h>
#include <test/switch_test.h>
#include <string.h>

static int cleanup_marks[8];

static fspr_status_t counting_cleanup(void *data)
{
	cleanup_marks[*(int *) data]++;
	return APR_SUCCESS;
}

FST_MINCORE_BEGIN("./conf")

FST_SUITE_BEGIN(switch_apr_cleanup)

FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

/* Real fspr_pool_cleanup_kill removes head/middle/tail correctly and is a no-op
 * for an unregistered entry; real run_cleanups (via pool destroy) then runs each
 * survivor exactly once. Fails if the Floyd rewrite corrupted the removal walk. */
FST_TEST_BEGIN(test_cleanup_kill_and_run)
{
	static int idx[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	fspr_pool_t *pool = NULL;
	int i;

	fst_requires(fspr_pool_create(&pool, NULL) == APR_SUCCESS);
	fst_requires(pool != NULL);

	memset(cleanup_marks, 0, sizeof(cleanup_marks));

	/* register prepends: list head -> idx4,idx3,idx2,idx1,idx0 */
	for (i = 0; i < 5; i++) {
		fspr_pool_cleanup_register(pool, &idx[i], counting_cleanup, counting_cleanup);
	}

	fspr_pool_cleanup_kill(pool, &idx[2], counting_cleanup); /* middle */
	fspr_pool_cleanup_kill(pool, &idx[4], counting_cleanup); /* head   */
	fspr_pool_cleanup_kill(pool, &idx[0], counting_cleanup); /* tail   */
	fspr_pool_cleanup_kill(pool, &idx[7], counting_cleanup); /* absent -> no-op */

	fspr_pool_destroy(pool); /* runs survivors idx1, idx3 exactly once */

	fst_check(cleanup_marks[0] == 0); /* killed (tail)   */
	fst_check(cleanup_marks[1] == 1); /* survived, once  */
	fst_check(cleanup_marks[2] == 0); /* killed (middle) */
	fst_check(cleanup_marks[3] == 1); /* survived, once  */
	fst_check(cleanup_marks[4] == 0); /* killed (head)   */
	fst_check(cleanup_marks[7] == 0); /* never registered*/
}
FST_TEST_END()

/* Mirrors rtp_get_pool_sock_mutex: a mutex attached to a pool via userdata is
 * shared -- a later lookup on the SAME pool returns the identical object. This is
 * what makes the TELCORE-302 socket lock pool-scoped (one lock shared by the
 * audio/video/T.38 rtp_sessions that share the session pool). */
FST_TEST_BEGIN(test_pool_sock_mutex_shared)
{
	fspr_pool_t *pool = NULL;
	switch_mutex_t *m1 = NULL, *m2 = NULL;

	fst_requires(fspr_pool_create(&pool, NULL) == APR_SUCCESS);
	fst_requires(pool != NULL);

	fst_check(switch_core_memory_pool_get_data(pool, "_rtp_sock_mutex") == NULL);

	switch_mutex_init(&m1, SWITCH_MUTEX_NESTED, pool);
	switch_core_memory_pool_set_data(pool, "_rtp_sock_mutex", m1);

	m2 = switch_core_memory_pool_get_data(pool, "_rtp_sock_mutex");
	fst_check(m2 != NULL);
	fst_check(m2 == m1); /* every caller on this pool gets the same mutex */

	fspr_pool_destroy(pool);
}
FST_TEST_END()

FST_SUITE_END()

FST_MINCORE_END()

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
