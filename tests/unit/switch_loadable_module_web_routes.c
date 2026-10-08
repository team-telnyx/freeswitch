/*
 * The loader's mod_web_server route sweeps must only remove routes that the
 * module being torn down actually owns.
 *
 * A failed LOAD is swept by name, and so is an unload. Two other loads can
 * share that name: a live module whose modname matches the failed load's
 * interface name or filename, and a reload of the same file issued while the
 * old instance is still unloading. Neither may lose its routes to someone
 * else's sweep.
 *
 * Modules are built with switch_loadable_module_build_dynamic(), so no .so is
 * needed. A route's presence is probed with switch_web_server_unregister(),
 * which returns SUCCESS only if the route existed.
 */
#include <switch.h>
#include <switch_web_server.h>
#include <test/switch_test.h>

#define LIVE_MOD "mod_wsr_live"
#define NEW_MOD  "mod_wsr_new"
#define SLOW_MOD "mod_wsr_slow"

static switch_status_t wsr_handler(switch_web_request_t *req, switch_web_response_t *res, void *ud)
{
	(void) req; (void) res; (void) ud;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t wsr_register(const char *module, const char *path)
{
	return switch_web_server_register(module, SWITCH_WEB_METHOD_GET, path, SWITCH_WEB_DISPATCH_LITE, wsr_handler, NULL);
}

/* Destructive probe: removes the route if it was there. */
static switch_bool_t wsr_take(const char *module, const char *path)
{
	return switch_web_server_unregister(module, SWITCH_WEB_METHOD_GET, path) == SWITCH_STATUS_SUCCESS;
}

static switch_status_t live_load(switch_loadable_module_interface_t **mi, switch_memory_pool_t *pool)
{
	*mi = switch_loadable_module_create_module_interface(pool, LIVE_MOD);
	return wsr_register(LIVE_MOD, "/wsr/live");
}

/* Claims the live module's name, registers nothing, fails. */
static switch_status_t collide_empty_load(switch_loadable_module_interface_t **mi, switch_memory_pool_t *pool)
{
	*mi = switch_loadable_module_create_module_interface(pool, LIVE_MOD);
	return SWITCH_STATUS_FALSE;
}

/* Claims the live module's name, registers under it, fails. */
static switch_status_t collide_route_load(switch_loadable_module_interface_t **mi, switch_memory_pool_t *pool)
{
	*mi = switch_loadable_module_create_module_interface(pool, LIVE_MOD);
	wsr_register(LIVE_MOD, "/wsr/late");
	return SWITCH_STATUS_FALSE;
}

/* Fails before creating an interface; only its filename names it. */
static switch_status_t no_interface_load(switch_loadable_module_interface_t **mi, switch_memory_pool_t *pool)
{
	(void) mi; (void) pool;
	return SWITCH_STATUS_FALSE;
}

/* Its own name, registers, fails: the original reason for the sweep. */
static switch_status_t own_route_load(switch_loadable_module_interface_t **mi, switch_memory_pool_t *pool)
{
	*mi = switch_loadable_module_create_module_interface(pool, NEW_MOD);
	wsr_register(NEW_MOD, "/wsr/new");
	return SWITCH_STATUS_FALSE;
}

static switch_status_t slow_load(switch_loadable_module_interface_t **mi, switch_memory_pool_t *pool)
{
	*mi = switch_loadable_module_create_module_interface(pool, SLOW_MOD);
	return SWITCH_STATUS_SUCCESS;
}

static volatile int slow_in_shutdown;
static volatile int slow_release;

static switch_status_t slow_shutdown(void)
{
	slow_in_shutdown = 1;
	while (!slow_release) {
		switch_yield(1000);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t unload_status;
static const char *unload_err;

static void *SWITCH_THREAD_FUNC unload_thread(switch_thread_t *thread, void *obj)
{
	(void) thread; (void) obj;
	unload_status = switch_loadable_module_unload_module(SWITCH_GLOBAL_dirs.mod_dir, SLOW_MOD, SWITCH_FALSE, &unload_err);
	return NULL;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_loadable_module_web_routes)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(failed_load_keeps_live_routes_of_the_same_name)
		{
			fst_check(switch_loadable_module_build_dynamic(LIVE_MOD, live_load, NULL, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);

			/* Same interface name, nothing registered: nothing to sweep. */
			fst_check(switch_loadable_module_build_dynamic("mod_wsr_collide_empty", collide_empty_load, NULL, NULL, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS);
			fst_check(wsr_take(LIVE_MOD, "/wsr/live"));
			fst_check(wsr_register(LIVE_MOD, "/wsr/live") == SWITCH_STATUS_SUCCESS);

			/* Filename equal to the live modname, no interface. */
			fst_check(switch_loadable_module_build_dynamic(LIVE_MOD, no_interface_load, NULL, NULL, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS);
			fst_check(wsr_take(LIVE_MOD, "/wsr/live"));
			fst_check(wsr_register(LIVE_MOD, "/wsr/live") == SWITCH_STATUS_SUCCESS);

			/* Same name and it did register: only its own route goes. */
			fst_check(switch_loadable_module_build_dynamic("mod_wsr_collide_route", collide_route_load, NULL, NULL, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS);
			fst_check(!wsr_take(LIVE_MOD, "/wsr/late"));
			fst_check(wsr_take(LIVE_MOD, "/wsr/live"));

			switch_web_server_unregister_module(LIVE_MOD);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(failed_load_still_sweeps_its_own_routes)
		{
			fst_check(switch_loadable_module_build_dynamic(NEW_MOD, own_route_load, NULL, NULL, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS);
			fst_check(!wsr_take(NEW_MOD, "/wsr/new"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(load_is_refused_while_the_same_module_unloads)
		{
			switch_thread_t *thread;
			switch_threadattr_t *attr;
			switch_status_t st;
			const char *err = NULL;
			int i;

			slow_in_shutdown = 0;
			slow_release = 0;
			fst_check(switch_loadable_module_build_dynamic(SLOW_MOD, slow_load, NULL, slow_shutdown, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);

			switch_threadattr_create(&attr, fst_pool);
			switch_thread_create(&thread, attr, unload_thread, NULL, fst_pool);

			for (i = 0; i < 5000 && !slow_in_shutdown; i++) {
				switch_yield(1000);
			}
			fst_requires(slow_in_shutdown);

			/* The old instance is inside SHUTDOWN and no longer in the module
			   hash. A load of the same name must wait for it to finish. */
			fst_check(switch_loadable_module_load_module(SWITCH_GLOBAL_dirs.mod_dir, SLOW_MOD, SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(err, "Module is still unloading");

			slow_release = 1;
			switch_thread_join(&st, thread);
			fst_check(unload_status == SWITCH_STATUS_SUCCESS);

			/* Unload done: the name is free again. There is no .so behind
			   it, so this fails, but past the unloading check. */
			err = NULL;
			fst_check(switch_loadable_module_load_module(SWITCH_GLOBAL_dirs.mod_dir, SLOW_MOD, SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS);
			fst_check(err && strcmp(err, "Module is still unloading"));
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
