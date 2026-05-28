/*
 * Standalone tests for the mod_web_server route registry.
 * Exercises both the public C ABI (switch_web_server_*) and the
 * mod-internal lookup namespace.
 */
#include <atomic>
#include <iostream>
#include <string>
#include <vector>

#include "switch.h"
#include "switch_web_server.h"
#include "switch_web_server_internal.h"

namespace internal = switch_web_server_internal;

static std::atomic<int> g_pass{0};
static std::atomic<int> g_fail{0};

#define CHECK(expr) do { \
    if (expr) { g_pass.fetch_add(1); } \
    else { \
        g_fail.fetch_add(1); \
        std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ << "  " << #expr << "\n"; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (_va == _vb) { g_pass.fetch_add(1); } \
    else { \
        g_fail.fetch_add(1); \
        std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__ \
                  << "  expected " << _vb << " got " << _va << "\n"; \
    } \
} while (0)

static std::atomic<int> g_calls{0};

static switch_status_t dummy_handler(switch_web_request_t *req,
                                     switch_web_response_t *res,
                                     void *ud)
{
	(void)req; (void)res; (void)ud;
	g_calls.fetch_add(1);
	return SWITCH_STATUS_SUCCESS;
}

static void wipe()
{
	switch_web_server_unregister_module("modA");
	switch_web_server_unregister_module("modB");
	switch_web_server_unregister_module("modC");
}

/* ----- Exact-route tests ----- */

static void test_exact_basic()
{
	std::cout << "[test] exact route registration + lookup\n";
	wipe();

	auto s = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)s, (int)SWITCH_STATUS_SUCCESS);

	auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/foo");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::Hit);
	CHECK(r.route.handler == dummy_handler);
	CHECK_EQ(r.route.module, std::string("modA"));
	CHECK_EQ(r.route.kind, std::string("exact"));
}

static void test_404()
{
	std::cout << "[test] 404 on unknown path\n";
	wipe();
	auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/nope");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::NotFound);
}

static void test_405()
{
	std::cout << "[test] 405 on known path, wrong method\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET,  "/foo", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_POST, "/foo", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	auto r = internal::lookup(SWITCH_WEB_METHOD_DELETE, "/foo");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::MethodNotAllowed);
	CHECK_EQ(r.allowed.size(), (std::size_t)2);
	CHECK(r.allowed.count(SWITCH_WEB_METHOD_GET) == 1);
	CHECK(r.allowed.count(SWITCH_WEB_METHOD_POST) == 1);
}

static void test_any_method()
{
	std::cout << "[test] ANY method route matches all verbs\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/any", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	for (auto m : { SWITCH_WEB_METHOD_GET, SWITCH_WEB_METHOD_POST, SWITCH_WEB_METHOD_DELETE }) {
		auto r = internal::lookup(m, "/any");
		CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::Hit);
	}
}

/* ----- Pattern-route tests ----- */

static void test_pattern_capture()
{
	std::cout << "[test] pattern route captures {id}\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/users/42");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::Hit);
	CHECK_EQ(r.route.kind, std::string("pattern"));
	CHECK(r.params != nullptr);
	if (r.params) {
		auto it = r.params->find("id");
		CHECK(it != r.params->end());
		if (it != r.params->end()) CHECK_EQ(it->second, std::string("42"));
	}
}

static void test_pattern_multi_segment()
{
	std::cout << "[test] pattern with multiple captures\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/a/{x}/b/{y}",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/a/one/b/two");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::Hit);
	if (r.params) {
		CHECK_EQ(r.params->at("x"), std::string("one"));
		CHECK_EQ(r.params->at("y"), std::string("two"));
	}

	auto miss = internal::lookup(SWITCH_WEB_METHOD_GET, "/a/one/b/two/extra");
	CHECK_EQ((int)miss.outcome, (int)internal::LookupOutcome::NotFound);
}

/* ----- Prefix-route tests ----- */

static void test_prefix()
{
	std::cout << "[test] prefix route matches path with extra suffix\n";
	wipe();
	switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/static/",
	                                  SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/static/css/main.css");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::Hit);
	CHECK_EQ(r.route.kind, std::string("prefix"));

	auto miss = internal::lookup(SWITCH_WEB_METHOD_GET, "/other/x");
	CHECK_EQ((int)miss.outcome, (int)internal::LookupOutcome::NotFound);
}

/* ----- Conflict + idempotence ----- */

static void test_conflict_other_module()
{
	std::cout << "[test] same (method,path) by different module -> conflict\n";
	wipe();
	auto s1 = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/dup",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)s1, (int)SWITCH_STATUS_SUCCESS);

	auto s2 = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/dup",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)s2, (int)SWITCH_STATUS_FALSE);   /* conflict */
}

static void test_idempotent_same_module()
{
	std::cout << "[test] same module re-registers same route -> success\n";
	wipe();
	auto s1 = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/idem",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	auto s2 = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/idem",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)s1, (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)s2, (int)SWITCH_STATUS_SUCCESS);
}

static switch_status_t alt_handler(switch_web_request_t *, switch_web_response_t *, void *)
{
	return SWITCH_STATUS_SUCCESS;
}

static void test_reregister_updates_handler()
{
	std::cout << "[test] re-register by same module updates handler/mode in place\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/swap",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, (void *)0x111);

	auto first = internal::lookup(SWITCH_WEB_METHOD_GET, "/swap");
	CHECK(first.route.handler == dummy_handler);
	CHECK(first.route.user_data == (void *)0x111);
	CHECK_EQ((int)first.route.mode, (int)SWITCH_WEB_DISPATCH_LITE);

	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/swap",
	                           SWITCH_WEB_DISPATCH_POOL, alt_handler, (void *)0x222);

	auto second = internal::lookup(SWITCH_WEB_METHOD_GET, "/swap");
	CHECK(second.route.handler == alt_handler);
	CHECK(second.route.user_data == (void *)0x222);
	CHECK_EQ((int)second.route.mode, (int)SWITCH_WEB_DISPATCH_POOL);
}

/* ----- Sweep on module unregister ----- */

static void test_unregister_module_sweep()
{
	std::cout << "[test] unregister_module drops all of one module's routes only\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET,  "/a1", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET,  "/a2/{x}", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/aPre/", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modB", SWITCH_WEB_METHOD_GET,  "/b1", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	switch_web_server_unregister_module("modA");

	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/a1").outcome,        (int)internal::LookupOutcome::NotFound);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/a2/7").outcome,      (int)internal::LookupOutcome::NotFound);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/aPre/x").outcome,    (int)internal::LookupOutcome::NotFound);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/b1").outcome,        (int)internal::LookupOutcome::Hit);
}

static void test_unregister_specific()
{
	std::cout << "[test] unregister single (method,path)\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET,  "/x", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_POST, "/x", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	auto s = switch_web_server_unregister("modA", SWITCH_WEB_METHOD_GET, "/x");
	CHECK_EQ((int)s, (int)SWITCH_STATUS_SUCCESS);

	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET,  "/x").outcome, (int)internal::LookupOutcome::MethodNotAllowed);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_POST, "/x").outcome, (int)internal::LookupOutcome::Hit);
}

/* ----- Snapshot ----- */

static void test_snapshot()
{
	std::cout << "[test] snapshot lists all registered routes\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/s1", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/s2/{id}", SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL);
	switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_POST, "/p/", SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	auto rows = internal::snapshot();
	CHECK_EQ(rows.size(), (std::size_t)3);
}

int main()
{
	test_exact_basic();
	test_404();
	test_405();
	test_any_method();
	test_pattern_capture();
	test_pattern_multi_segment();
	test_prefix();
	test_conflict_other_module();
	test_idempotent_same_module();
	test_reregister_updates_handler();
	test_unregister_module_sweep();
	test_unregister_specific();
	test_snapshot();

	wipe();
	std::cout << "\n" << g_pass.load() << " passed, " << g_fail.load() << " failed\n";
	return g_fail.load() ? 1 : 0;
}
