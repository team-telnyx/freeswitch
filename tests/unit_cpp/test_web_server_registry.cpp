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

static void test_prefix_segment_boundary()
{
	std::cout << "[test] prefix matches only on path-segment boundary, not raw string\n";

	/* No trailing slash: must match "/api" exactly and "/api/anything",
	   must NOT match "/apiv2..." since that would shadow unrelated routes. */
	wipe();
	switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
	                                  SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/api").outcome,
	         (int)internal::LookupOutcome::Hit);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/api/v2/users").outcome,
	         (int)internal::LookupOutcome::Hit);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/apiv2/users").outcome,
	         (int)internal::LookupOutcome::NotFound);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/apix").outcome,
	         (int)internal::LookupOutcome::NotFound);

	/* Trailing slash: the slash itself is the boundary. Must NOT match
	   "/api" (shorter than prefix), must match "/api/" and below. */
	wipe();
	switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api/",
	                                  SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/api/").outcome,
	         (int)internal::LookupOutcome::Hit);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/api/v2/users").outcome,
	         (int)internal::LookupOutcome::Hit);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/api").outcome,
	         (int)internal::LookupOutcome::NotFound);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/apiv2/users").outcome,
	         (int)internal::LookupOutcome::NotFound);
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

/* ----- ANY-vs-specific conflict, both insertion orders, all three tiers ----- */

static void test_exact_any_blocks_specific()
{
	std::cout << "[test] exact: ANY registered first blocks later specific from another module\n";
	wipe();
	auto a = switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a, (int)SWITCH_STATUS_SUCCESS);
	auto b = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b, (int)SWITCH_STATUS_FALSE);
}

static void test_exact_specific_blocks_any()
{
	std::cout << "[test] exact: specific registered first blocks later ANY from another module\n";
	wipe();
	auto a = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a, (int)SWITCH_STATUS_SUCCESS);
	auto b = switch_web_server_register("modB", SWITCH_WEB_METHOD_ANY, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b, (int)SWITCH_STATUS_FALSE);
}

static void test_pattern_any_blocks_specific_both_orders()
{
	std::cout << "[test] pattern: ANY-vs-specific conflict in both insertion orders\n";
	wipe();
	auto a1 = switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/users/{id}",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a1, (int)SWITCH_STATUS_SUCCESS);
	auto b1 = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b1, (int)SWITCH_STATUS_FALSE);

	wipe();
	auto a2 = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a2, (int)SWITCH_STATUS_SUCCESS);
	auto b2 = switch_web_server_register("modB", SWITCH_WEB_METHOD_ANY, "/users/{id}",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b2, (int)SWITCH_STATUS_FALSE);
}

static void test_prefix_any_blocks_specific_both_orders()
{
	std::cout << "[test] prefix: ANY-vs-specific conflict in both insertion orders\n";
	wipe();
	auto a1 = switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_ANY, "/static/",
	                                            SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a1, (int)SWITCH_STATUS_SUCCESS);
	auto b1 = switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_GET, "/static/",
	                                            SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b1, (int)SWITCH_STATUS_FALSE);

	wipe();
	auto a2 = switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/static/",
	                                            SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a2, (int)SWITCH_STATUS_SUCCESS);
	auto b2 = switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_ANY, "/static/",
	                                            SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b2, (int)SWITCH_STATUS_FALSE);
}

static void test_any_vs_any_cross_module_blocked()
{
	std::cout << "[test] ANY-vs-ANY across modules is a conflict\n";
	wipe();
	auto a = switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)a, (int)SWITCH_STATUS_SUCCESS);
	auto b = switch_web_server_register("modB", SWITCH_WEB_METHOD_ANY, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)b, (int)SWITCH_STATUS_FALSE);
}

static void test_same_module_any_reregister_updates()
{
	std::cout << "[test] same module re-registering identical ANY route updates in place\n";
	wipe();
	auto a = switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/foo",
	                                    SWITCH_WEB_DISPATCH_LITE, dummy_handler, (void *)0x1);
	CHECK_EQ((int)a, (int)SWITCH_STATUS_SUCCESS);
	auto b = switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/foo",
	                                    SWITCH_WEB_DISPATCH_POOL, alt_handler, (void *)0x2);
	CHECK_EQ((int)b, (int)SWITCH_STATUS_SUCCESS);

	/* ANY must still match every verb after the update — that's the whole
	   point of ANY. Walk all the methods we expose, not just GET. */
	for (auto m : { SWITCH_WEB_METHOD_GET, SWITCH_WEB_METHOD_POST, SWITCH_WEB_METHOD_PUT,
	                SWITCH_WEB_METHOD_DELETE, SWITCH_WEB_METHOD_PATCH }) {
		auto hit = internal::lookup(m, "/foo");
		CHECK_EQ((int)hit.outcome, (int)internal::LookupOutcome::Hit);
		CHECK(hit.route.handler == alt_handler);
		CHECK(hit.route.user_data == (void *)0x2);
		CHECK_EQ((int)hit.route.mode, (int)SWITCH_WEB_DISPATCH_POOL);
	}
}

/* ----- Semantic (match-set) overlap conflicts ----- */

/* Two patterns with the same shape but different capture names have identical
   match-sets; they must conflict in both orders, cross- and same-module, and
   when one side is ANY. */
static void test_pattern_equivalent_shape_conflict()
{
	std::cout << "[test] pattern: /users/{id} vs /users/{name} conflict (same match-set)\n";

	/* cross-module, order A->B */
	wipe();
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/users/{name}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);

	/* cross-module, order B->A (symmetry) */
	wipe();
	CHECK_EQ((int)switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/users/{name}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);

	/* same-module, different capture name on same shape — still ambiguous */
	wipe();
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{name}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);

	/* ANY vs specific on equivalent shape */
	wipe();
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/users/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/users/{name}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);
}

/* Different capture name but identical raw on a different method must NOT
   conflict (methods disjoint), and distinct shapes must coexist. */
static void test_pattern_distinct_shapes_coexist()
{
	std::cout << "[test] pattern: distinct shapes / disjoint methods coexist\n";
	wipe();
	/* same shape, disjoint methods -> allowed */
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_POST, "/users/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	/* different literal skeleton -> different match-sets -> allowed */
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/teams/{id}",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
}

/* /api and /api/v2 segment-overlap; reject in both orders, cross-module,
   and ANY vs specific. */
static void test_prefix_overlap_conflict()
{
	std::cout << "[test] prefix: /api vs /api/v2 conflict in both orders\n";

	/* /api before /api/v2 */
	wipe();
	CHECK_EQ((int)switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_GET, "/api/v2",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);

	/* /api/v2 before /api (symmetry) */
	wipe();
	CHECK_EQ((int)switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api/v2",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_GET, "/api",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);

	/* ANY vs specific overlap */
	wipe();
	CHECK_EQ((int)switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_ANY, "/api",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_GET, "/api/v2",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_FALSE);
}

/* Prefixes that do not segment-contain each other must coexist. */
static void test_prefix_disjoint_coexist()
{
	std::cout << "[test] prefix: /api vs /apiv2 vs /other coexist (no segment overlap)\n";
	wipe();
	CHECK_EQ((int)switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register_prefix("modB", SWITCH_WEB_METHOD_GET, "/apiv2",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register_prefix("modC", SWITCH_WEB_METHOD_GET, "/other",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
}

/* Different tiers may coexist on overlapping paths; lookup precedence is
   exact > pattern > prefix and is therefore order-independent. */
static void test_cross_tier_precedence()
{
	std::cout << "[test] cross-tier: exact > pattern > prefix on overlapping paths\n";
	wipe();
	/* Register prefix first, then pattern, then exact — reverse of precedence
	   to prove lookup order does not follow insertion order. */
	switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
	                                  SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/api/{id}",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/api/health",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	/* exact wins over both pattern and prefix */
	auto e = internal::lookup(SWITCH_WEB_METHOD_GET, "/api/health");
	CHECK_EQ((int)e.outcome, (int)internal::LookupOutcome::Hit);
	CHECK_EQ(e.route.kind, std::string("exact"));

	/* pattern wins over prefix */
	auto p = internal::lookup(SWITCH_WEB_METHOD_GET, "/api/42");
	CHECK_EQ((int)p.outcome, (int)internal::LookupOutcome::Hit);
	CHECK_EQ(p.route.kind, std::string("pattern"));

	/* prefix catches what neither exact nor pattern match */
	auto pr = internal::lookup(SWITCH_WEB_METHOD_GET, "/api/v2/users");
	CHECK_EQ((int)pr.outcome, (int)internal::LookupOutcome::Hit);
	CHECK_EQ(pr.route.kind, std::string("prefix"));
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
	test_prefix_segment_boundary();
	test_conflict_other_module();
	test_idempotent_same_module();
	test_reregister_updates_handler();
	test_exact_any_blocks_specific();
	test_exact_specific_blocks_any();
	test_pattern_any_blocks_specific_both_orders();
	test_prefix_any_blocks_specific_both_orders();
	test_any_vs_any_cross_module_blocked();
	test_same_module_any_reregister_updates();
	test_pattern_equivalent_shape_conflict();
	test_pattern_distinct_shapes_coexist();
	test_prefix_overlap_conflict();
	test_prefix_disjoint_coexist();
	test_cross_tier_precedence();
	test_unregister_module_sweep();
	test_unregister_specific();
	test_snapshot();

	wipe();
	std::cout << "\n" << g_pass.load() << " passed, " << g_fail.load() << " failed\n";
	return g_fail.load() ? 1 : 0;
}
