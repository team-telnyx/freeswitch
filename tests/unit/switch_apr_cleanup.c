/*
 * Unit coverage for the APR pool cleanup-list handling, against the real exported
 * fspr_pool_cleanup_* symbols rather than a copy.
 *
 * <fspr_pools.h> is not on the tests/unit include path (APR includes live in
 * CORE_CFLAGS, not the shared test CFLAGS) and switch.h exposes only the opaque
 * switch_memory_pool_t (== fspr_pool_t), so the symbols we call are forward-declared
 * here and resolve at link against the APR in libfreeswitch. fspr_pool_create is
 * just a macro over fspr_pool_create_ex; fspr_pool_destroy runs cleanups
 * synchronously, unlike switch_core_destroy_memory_pool.
 */

#include <switch.h>
#include <test/switch_test.h>
#include <string.h>

/* Real, exported APR symbols (see file header for why they are declared here). */
extern int  fspr_pool_create_ex(switch_memory_pool_t **newpool, switch_memory_pool_t *parent,
								void *abort_fn, void *allocator);
extern void fspr_pool_destroy(switch_memory_pool_t *pool);
extern void fspr_pool_cleanup_register(switch_memory_pool_t *pool, const void *data,
									   int (*plain_cleanup)(void *), int (*child_cleanup)(void *));
extern void fspr_pool_cleanup_kill(switch_memory_pool_t *pool, const void *data,
								   int (*cleanup)(void *));

static int cleanup_marks[8];

static int counting_cleanup(void *data)
{
	cleanup_marks[*(int *) data]++;
	return 0; /* APR_SUCCESS */
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
 * for an unregistered entry; real run_cleanups (via fspr_pool_destroy) then runs
 * each survivor exactly once. Fails if the Floyd rewrite corrupted the removal
 * walk or if run_cleanups wrongly skips a non-cyclic list. */
FST_TEST_BEGIN(test_cleanup_kill_and_run)
{
	static int idx[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	switch_memory_pool_t *pool = NULL;
	int i;

	fst_requires(fspr_pool_create_ex(&pool, NULL, NULL, NULL) == 0);
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

	fspr_pool_destroy(pool); /* synchronous: runs survivors idx1, idx3 exactly once */

	fst_check(cleanup_marks[0] == 0); /* killed (tail)    */
	fst_check(cleanup_marks[1] == 1); /* survived, once   */
	fst_check(cleanup_marks[2] == 0); /* killed (middle)  */
	fst_check(cleanup_marks[3] == 1); /* survived, once   */
	fst_check(cleanup_marks[4] == 0); /* killed (head)    */
	fst_check(cleanup_marks[7] == 0); /* never registered */
}
FST_TEST_END()

/* Mirrors rtp_get_pool_sock_mutex: a mutex attached to a pool via userdata is
 * shared -- a later lookup on the same pool returns the identical object. */
FST_TEST_BEGIN(test_pool_sock_mutex_shared)
{
	switch_memory_pool_t *pool = NULL;
	switch_mutex_t *m1 = NULL, *m2 = NULL;

	fst_requires(fspr_pool_create_ex(&pool, NULL, NULL, NULL) == 0);
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
