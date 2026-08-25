/*
 * Standalone tests for the mod_web_server route registry.
 * Exercises both the public C ABI (switch_web_server_*) and the
 * mod-internal lookup namespace.
 */
#include <cstring>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
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

/* Must name EVERY module any test registers under. A module missing here
   leaks its routes into later tests, where they silently turn a fresh
   registration into a conflict rejection. */
static void wipe()
{
	switch_web_server_unregister_module("modA");
	switch_web_server_unregister_module("modB");
	switch_web_server_unregister_module("modC");
	switch_web_server_unregister_module("modOK");
	switch_web_server_unregister_module("modZ");
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

/* Cross-tier overlap is allowed (not a conflict) and the more specific tier
   wins regardless of registration order. Proves each tier pair — exact vs
   pattern, exact vs prefix, pattern vs prefix — in BOTH insertion orders,
   so the documented precedence does not secretly depend on load order. */
static void test_cross_tier_no_conflict_both_orders()
{
	std::cout << "[test] cross-tier: overlapping tiers coexist; precedence is order-independent\n";

	/* --- exact vs pattern: exact "/users/me" shadows pattern "/users/{id}" --- */
	for (int order = 0; order < 2; ++order) {
		wipe();
		switch_status_t se, sp;
		if (order == 0) {
			sp = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
			                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
			se = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/users/me",
			                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
		} else {
			se = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/users/me",
			                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
			sp = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/users/{id}",
			                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
		}
		CHECK_EQ((int)se, (int)SWITCH_STATUS_SUCCESS);
		CHECK_EQ((int)sp, (int)SWITCH_STATUS_SUCCESS);
		/* exact match wins, the rest still falls through to the pattern */
		CHECK_EQ(internal::lookup(SWITCH_WEB_METHOD_GET, "/users/me").route.kind, std::string("exact"));
		CHECK_EQ(internal::lookup(SWITCH_WEB_METHOD_GET, "/users/42").route.kind, std::string("pattern"));
	}

	/* --- exact vs prefix: exact "/api/health" shadows prefix "/api" --- */
	for (int order = 0; order < 2; ++order) {
		wipe();
		switch_status_t se, spr;
		if (order == 0) {
			spr = switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
			                                        SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
			se  = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/api/health",
			                                 SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
		} else {
			se  = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/api/health",
			                                 SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
			spr = switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
			                                        SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
		}
		CHECK_EQ((int)se,  (int)SWITCH_STATUS_SUCCESS);
		CHECK_EQ((int)spr, (int)SWITCH_STATUS_SUCCESS);
		CHECK_EQ(internal::lookup(SWITCH_WEB_METHOD_GET, "/api/health").route.kind, std::string("exact"));
		CHECK_EQ(internal::lookup(SWITCH_WEB_METHOD_GET, "/api/v2/x").route.kind,   std::string("prefix"));
	}

	/* --- pattern vs prefix: pattern "/api/{id}" shadows prefix "/api" --- */
	for (int order = 0; order < 2; ++order) {
		wipe();
		switch_status_t sp, spr;
		if (order == 0) {
			spr = switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
			                                        SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
			sp  = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/api/{id}",
			                                 SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
		} else {
			sp  = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/api/{id}",
			                                 SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
			spr = switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/api",
			                                        SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
		}
		CHECK_EQ((int)sp,  (int)SWITCH_STATUS_SUCCESS);
		CHECK_EQ((int)spr, (int)SWITCH_STATUS_SUCCESS);
		/* single trailing segment hits the pattern; deeper paths fall to prefix */
		CHECK_EQ(internal::lookup(SWITCH_WEB_METHOD_GET, "/api/42").route.kind,   std::string("pattern"));
		CHECK_EQ(internal::lookup(SWITCH_WEB_METHOD_GET, "/api/v2/x").route.kind, std::string("prefix"));
	}
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

/* ----- Response printf ----- */

/* Regression for the long-output path of switch_web_response_printf():
   formatted output far larger than any common stack/small buffer must land
   in the body intact, with the exact byte length and no truncation or
   trailing NUL. Also proves last-call-wins fully replaces a longer body. */
static void test_response_printf_long_output()
{
	std::cout << "[test] response printf: large formatted output is stored intact\n";

	internal::ResponsePtr res(internal::make_response());

	/* 10000 bytes of payload — well past 256/1024/4096 thresholds. */
	const int n = 10000;
	std::string filler(static_cast<std::size_t>(n), 'x');
	switch_web_response_printf(res.get(), "PRE[%s]POST=%d", filler.c_str(), n);

	std::string expected = "PRE[" + filler + "]POST=" + std::to_string(n);
	{
		const std::string &body = internal::response_body(res.get());
		CHECK_EQ(body.size(), expected.size());
		CHECK(body == expected);
		/* exactly `needed` bytes — no embedded/trailing NUL from vsnprintf */
		CHECK(body.find('\0') == std::string::npos);
	}

	/* last call wins: a short printf must fully replace the long body. */
	switch_web_response_printf(res.get(), "short:%d", 7);
	{
		const std::string &body = internal::response_body(res.get());
		CHECK_EQ(body, std::string("short:7"));
	}
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

/* ----- Listener independence (module load order) ----- */

static void test_register_without_listener()
{
	std::cout << "[test] registration is independent of the listener / load order\n";
	wipe();
	internal::set_listener_present(false);

	/* The consumer-loads-first case. mod_web_server is not up, so
	   switch_web_server_available() is FALSE — but the registry lives in
	   libfreeswitch, so registration must still land and the route must
	   resolve. If this ever stopped holding, gating registration on
	   availability would become the only correct pattern for consumers. */
	CHECK(switch_web_server_available() == SWITCH_FALSE);

	auto s = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/late",
	                                    SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL);
	CHECK_EQ((int)s, (int)SWITCH_STATUS_SUCCESS);

	auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/late");
	CHECK_EQ((int)r.outcome, (int)internal::LookupOutcome::Hit);
	CHECK(r.route.handler == dummy_handler);

	/* Listener comes up afterwards: the same route serves, with no
	   re-registration and no reload of the consumer module. */
	internal::set_listener_present(true);
	CHECK(switch_web_server_available() == SWITCH_TRUE);

	auto r2 = internal::lookup(SWITCH_WEB_METHOD_GET, "/late");
	CHECK_EQ((int)r2.outcome, (int)internal::LookupOutcome::Hit);
	CHECK(r2.route.handler == dummy_handler);

	/* Reverse order — listener already up when the consumer registers. */
	auto s2 = switch_web_server_register("modB", SWITCH_WEB_METHOD_GET, "/early",
	                                     SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK_EQ((int)s2, (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/early").outcome,
	         (int)internal::LookupOutcome::Hit);

	internal::set_listener_present(false);
}

/* ----- Drain contract ----- */

/* Spin until `path` stops resolving, which proves remove_module()'s write-lock
   section has completed and `draining` is set. Deterministic where a sleep is
   a guess — on a loaded box a sleep can fire before the sweeper has the lock,
   which would fail the test spuriously. */
static void wait_until_unrouted(const char *path)
{
	for (int i = 0; i < 20000; ++i) {
		if (internal::lookup(SWITCH_WEB_METHOD_GET, path).outcome ==
		    internal::LookupOutcome::NotFound) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	CHECK(!"timed out waiting for the route to be swept");
}

/*
 * The property the whole design exists for: unregister_module() must not
 * return while a handler of that module is still running. Nothing exercised
 * it before — no test constructed an InFlightTicket or started a thread.
 */
static void test_drain_waits_for_inflight()
{
	std::cout << "[test] unregister_module blocks until the in-flight ticket drops\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/drain",
	                           SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL);

	/* Hold a ticket, exactly as a dispatched-but-unfinished handler does. */
	auto held = internal::lookup(SWITCH_WEB_METHOD_GET, "/drain");
	CHECK_EQ((int)held.outcome, (int)internal::LookupOutcome::Hit);

	std::atomic<bool> returned{false};
	std::thread sweeper([&] {
		switch_web_server_unregister_module("modA");
		returned.store(true);
	});

	wait_until_unrouted("/drain");
	CHECK(returned.load() == false);          /* still blocked, ticket outstanding */

	{ auto drop = std::move(held); }          /* release it */
	sweeper.join();
	CHECK(returned.load() == true);

	/* And the route really is gone afterwards. */
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/drain").outcome,
	         (int)internal::LookupOutcome::NotFound);
}

/* A registration landing mid-drain must not get a fresh slot: new lookups
 * would count against it while the drain watched the old one, and
 * unregister_module() would return with a handler still in flight. */
static void test_register_during_drain_is_refused()
{
	std::cout << "[test] registration during a drain is refused, not silently re-slotted\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/d1",
	                           SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL);

	auto held = internal::lookup(SWITCH_WEB_METHOD_GET, "/d1");
	CHECK_EQ((int)held.outcome, (int)internal::LookupOutcome::Hit);

	std::atomic<bool> returned{false};
	std::thread sweeper([&] {
		switch_web_server_unregister_module("modA");
		returned.store(true);
	});
	wait_until_unrouted("/d1");

	auto s = switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/d2",
	                                    SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL);
	CHECK_EQ((int)s, (int)SWITCH_STATUS_INUSE);
	CHECK(returned.load() == false);           /* the drain did not get satisfied */

	/* The property, not just the status code: the refused registration must
	   not have produced a reachable route on a second slot. A fix that changed
	   the return value but still re-slotted would pass the check above. */
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/d2").outcome,
	         (int)internal::LookupOutcome::NotFound);

	{ auto drop = std::move(held); }
	sweeper.join();

	/* Once drained, the module can register again against a fresh slot. */
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/d2",
	                                         SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
}

/* Concurrent lookup / unregister / re-register must not corrupt the registry
   or lose the drain guarantee. */
static void test_drain_under_concurrency()
{
	std::cout << "[test] concurrent lookup + unregister_module stress\n";
	wipe();
	std::atomic<bool> stop{false};
	std::atomic<int>  hits{0};

	std::atomic<int> registered{0};

	std::thread reader([&] {
		while (!stop.load()) {
			auto r = internal::lookup(SWITCH_WEB_METHOD_GET, "/race");
			if (r.outcome == internal::LookupOutcome::Hit) hits.fetch_add(1);
		}
	});

	int resolved = 0;
	for (int i = 0; i < 200; ++i) {
		if (switch_web_server_register("modC", SWITCH_WEB_METHOD_GET, "/race",
		                               SWITCH_WEB_DISPATCH_LITE, dummy_handler,
		                               NULL) == SWITCH_STATUS_SUCCESS) {
			registered.fetch_add(1);
		}
		/* Checked from this thread, so it is deterministic. The temporary
		   LookupResult (and its ticket) dies at the end of the full expression,
		   before the unregister below, so this cannot self-block. */
		if (internal::lookup(SWITCH_WEB_METHOD_GET, "/race").outcome ==
		    internal::LookupOutcome::Hit) {
			++resolved;
		}
		switch_web_server_unregister_module("modC");
	}
	stop.store(true);
	reader.join();

	/* Survived without deadlock or corruption, and the sweep left nothing. */
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/race").outcome,
	         (int)internal::LookupOutcome::NotFound);
	/* Without these the test would "pass" with every register refused and
	   every lookup missing — i.e. proving nothing. Both are deterministic:
	   the reader thread's hit count is reported but NOT asserted, because
	   whether it catches one of the 200 short windows depends on scheduling
	   and asserting it makes the test flaky rather than strict. */
	CHECK_EQ(registered.load(), 200);
	CHECK_EQ(resolved, 200);
	std::cout << "        (" << hits.load() << " concurrent hits observed by the reader)\n";
}

/* Two unregister_module() calls for the same module overlapping. Whichever
   finishes first retires the shared slot; the other must not return on that
   stale guarantee if the module has been re-slotted since. */
static void test_overlapping_drains()
{
	std::cout << "[test] overlapping unregister_module calls both drain\n";
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/ov",
	                           SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL);

	auto held = internal::lookup(SWITCH_WEB_METHOD_GET, "/ov");
	CHECK_EQ((int)held.outcome, (int)internal::LookupOutcome::Hit);

	std::atomic<int> done{0};
	std::thread a([&] { switch_web_server_unregister_module("modA"); done.fetch_add(1); });
	std::thread b([&] { switch_web_server_unregister_module("modA"); done.fetch_add(1); });

	wait_until_unrouted("/ov");
	CHECK_EQ(done.load(), 0);                 /* both blocked on the live ticket */

	{ auto drop = std::move(held); }
	a.join(); b.join();
	CHECK_EQ(done.load(), 2);

	/* Slot fully retired: the module registers cleanly again. */
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/ov",
	                                         SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
}

/* An out-of-range method (e.g. a module built against a header where ANY was
   5) must be rejected, not silently accepted as an unreachable route. */
static void test_invalid_method_rejected()
{
	std::cout << "[test] out-of-range method is rejected\n";
	wipe();
	CHECK_EQ((int)switch_web_server_register("modA", (switch_web_method_t)5, "/bad",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)switch_web_server_register("modA", (switch_web_method_t)99999, "/bad2",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)switch_web_server_register_prefix("modA", (switch_web_method_t)5, "/bad3",
	                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/bad").outcome,
	         (int)internal::LookupOutcome::NotFound);
}

/* An unrecognised dispatch mode is worse than an unrecognised method: the
   dispatcher tests `mode == POOL`, so anything else silently means LITE, and a
   handler written to block would then run on the shared IO strand. */
static void test_invalid_mode_rejected()
{
	std::cout << "[test] out-of-range dispatch mode is rejected\n";
	wipe();
	/* Values outside an unfixed enum's value range are UB to form, so these go
	   through an int and a memcpy rather than a direct cast — a compiler is
	   entitled to assume a directly-cast out-of-range enum cannot exist, which
	   would make the whole test vacuous. What is exercised here is what a
	   module built against a different header would actually pass in. */
	auto as_mode = [](int v) {
		switch_web_dispatch_t m;
		std::memcpy(&m, &v, sizeof m);
		return m;
	};
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/m1",
	                                         as_mode(42), dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/m2",
	                                         as_mode(-1), dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, "/m3",
	                                                as_mode(9), dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/m1").outcome,
	         (int)internal::LookupOutcome::NotFound);

	/* Both legitimate modes still register. */
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/lite",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/pool",
	                                         SWITCH_WEB_DISPATCH_POOL, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
}

/* A path the dispatcher can never match is a dead endpoint with no
   diagnostic — the same failure the method check exists to prevent. The target
   is split at '?' before routing, so a registered '?' can never match. */
static void test_invalid_registration_path_rejected()
{
	std::cout << "[test] unmatchable registration paths are rejected\n";
	wipe();
	/* NB the string splits: "\x7fb" would be read as the hex escape 0x7fb,
	   not DEL followed by 'b'. Same for the UTF-8 case below. */
	const char *bad[] = { "/a?x=1", "/a b", "/a\r\nb", "/a#frag", "rel", "", "/a\x7f" "b", "/a\tb" };
	for (const char *p : bad) {
		CHECK_EQ((int)switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, p,
		                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
		         (int)SWITCH_STATUS_GENERR);
		CHECK_EQ((int)switch_web_server_register_prefix("modA", SWITCH_WEB_METHOD_GET, p,
		                                                SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
		         (int)SWITCH_STATUS_GENERR);
	}

	/* Nothing legitimate became newly unregistrable. These are the shapes the
	   real consumers use, plus percent-escapes and non-ASCII, which are valid
	   path bytes even though this layer does not decode them. */
	const char *good[] = {
		"/", "/ok", "/a/", "/a%20b", "/caf\xc3" "\xa9", "/users/{id}",
		"/metrics", "/health", "/healthz", "/readyz",
		"/dg/pair/{pair_id}/status", "/dg/list_json"
	};
	for (const char *p : good) {
		CHECK_EQ((int)switch_web_server_register("modOK", SWITCH_WEB_METHOD_GET, p,
		                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
		         (int)SWITCH_STATUS_SUCCESS);
	}

	/* A rejected registration must not leave a usable route behind, whichever
	   way it was rejected. (The stronger claim — that a GENERR also creates no
	   ModuleSlot, unlike a conflict — is not observable through the public
	   ABI, since slots are keyed by module name and reused; it is asserted in
	   the comment at Registry::add and not here.) */
	wipe();
	CHECK_EQ((int)switch_web_server_register("modZ", SWITCH_WEB_METHOD_GET, "/a b",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_GENERR);
	CHECK_EQ((int)internal::lookup(SWITCH_WEB_METHOD_GET, "/a b").outcome,
	         (int)internal::LookupOutcome::NotFound);
	CHECK_EQ((int)switch_web_server_register("modZ", SWITCH_WEB_METHOD_GET, "/fine",
	                                         SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL),
	         (int)SWITCH_STATUS_SUCCESS);
	wipe();
}

/* ----- Query-string parsing (the ABI helper) ----- */

static switch_web_request_t *mk_req(const char *query)
{
	internal::RequestInit init;
	init.method = SWITCH_WEB_METHOD_GET;
	init.path   = "/q";
	init.query  = query;
	return internal::make_request(std::move(init));
}

static void check_qp(const char *query, const char *key, const char *expect)
{
	internal::RequestPtr r(mk_req(query));
	const char *got = switch_web_request_query_param(r.get(), key);
	if (!expect) {
		CHECK(got == NULL);
		if (got) std::cerr << "    query=[" << query << "] key=" << key
		                   << " expected NULL got [" << got << "]\n";
	} else {
		CHECK(got != NULL && std::string(got) == expect);
		if (!got || std::string(got) != expect) {
			std::cerr << "    query=[" << query << "] key=" << key << " expected ["
			          << expect << "] got [" << (got ? got : "(null)") << "]\n";
		}
	}
}

static void test_query_params()
{
	std::cout << "[test] query parameters decode per the documented rules\n";

	check_qp("a=1&b=2", "a", "1");
	check_qp("a=1&b=2", "b", "2");
	check_qp("a=1&b=2", "c", NULL);            /* absent */
	check_qp("a=", "a", "");                   /* present but empty */
	check_qp("", "a", NULL);
	check_qp("a", "a", NULL);                  /* no '=' is not a pair */
	check_qp("a=1&a=2", "a", "1");             /* first occurrence wins */
	check_qp("a=x+y", "a", "x y");             /* '+' is a space */
	check_qp("a=x%20y", "a", "x y");
	check_qp("a=foo%2Fbar", "a", "foo/bar");
	check_qp("a=%2f", "a", "/");               /* lowercase hex */
	check_qp("a=100%", "a", "100%");           /* stray % is literal */
	check_qp("a=%zz", "a", "%zz");             /* invalid escape is literal */
	check_qp("a=b%00c", "a", NULL);            /* %00 drops the pair, not truncates */
	check_qp("a=b&x%00=1&c=d", "c", "d");      /* a bad pair does not eat the rest */
	check_qp("a=b%3Dc", "a", "b=c");           /* decoded '=' stays in the value */
	check_qp("a=b=c", "a", "b=c");             /* split on the FIRST '=' only */
	check_qp("&&a=1&&", "a", "1");             /* empty pairs ignored */
	check_qp("a%2Bb=1", "a+b", "1");           /* keys decode too */
}

/* ----- allowed_methods (what OPTIONS answers from) ----- */

static void test_allowed_methods()
{
	std::cout << "[test] allowed_methods collects across all three tiers\n";
	wipe();

	CHECK(internal::allowed_methods("/nothing").empty());

	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/am",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modA", SWITCH_WEB_METHOD_POST, "/am",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	auto m = internal::allowed_methods("/am");
	CHECK_EQ(m.size(), (std::size_t)2);
	CHECK(m.count(SWITCH_WEB_METHOD_GET) == 1);
	CHECK(m.count(SWITCH_WEB_METHOD_POST) == 1);

	/* pattern tier */
	switch_web_server_register("modB", SWITCH_WEB_METHOD_DELETE, "/am/{id}",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	auto mp = internal::allowed_methods("/am/7");
	CHECK_EQ(mp.size(), (std::size_t)1);
	CHECK(mp.count(SWITCH_WEB_METHOD_DELETE) == 1);

	/* An ANY route is reported as the concrete verbs it serves: "ANY" is not
	   an HTTP method token, so Allow: ANY is invalid per RFC 9110. */
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_ANY, "/any",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	auto ma = internal::allowed_methods("/any");
	CHECK(ma.count(SWITCH_WEB_METHOD_ANY) == 0);
	CHECK_EQ(ma.size(), (std::size_t)5);
	for (auto verb : { SWITCH_WEB_METHOD_GET, SWITCH_WEB_METHOD_POST, SWITCH_WEB_METHOD_PUT,
	                   SWITCH_WEB_METHOD_DELETE, SWITCH_WEB_METHOD_PATCH }) {
		CHECK(ma.count(verb) == 1);
	}
	/* and it really is served for each of them */
	for (auto verb : { SWITCH_WEB_METHOD_GET, SWITCH_WEB_METHOD_POST, SWITCH_WEB_METHOD_DELETE }) {
		CHECK_EQ((int)internal::lookup(verb, "/any").outcome, (int)internal::LookupOutcome::Hit);
	}
	wipe();
	switch_web_server_register("modA", SWITCH_WEB_METHOD_GET, "/am",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	switch_web_server_register("modB", SWITCH_WEB_METHOD_DELETE, "/am/{id}",
	                           SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);

	/* prefix tier, and a path that matches nothing under it */
	switch_web_server_register_prefix("modC", SWITCH_WEB_METHOD_PUT, "/pre",
	                                  SWITCH_WEB_DISPATCH_LITE, dummy_handler, NULL);
	CHECK(internal::allowed_methods("/pre/deep/path").count(SWITCH_WEB_METHOD_PUT) == 1);
	CHECK(internal::allowed_methods("/prefix").empty());   /* segment-bounded */
	wipe();
}

/* ----- Response header validation ----- */

static void test_header_validation()
{
	std::cout << "[test] response headers reject injection and invalid names\n";
	internal::ResponsePtr res(internal::make_response());

	/* A CR or LF in a value is response splitting. */
	switch_web_response_set_header(res.get(), "x-echo", "ok\r\nX-Injected: yes");
	CHECK(internal::response_headers(res.get()).count("x-echo") == 0);

	/* A name must be an RFC 9110 token: not empty, no space, no colon. */
	switch_web_response_set_header(res.get(), "", "v");
	switch_web_response_set_header(res.get(), "x space", "v");
	switch_web_response_set_header(res.get(), "x-evil: injected", "v");
	switch_web_response_set_header(res.get(), "utf8\xc3\xa9", "v");
	CHECK_EQ(internal::response_headers(res.get()).size(), (std::size_t)0);

	/* Legitimate headers still go through, and the name is lower-cased. */
	switch_web_response_set_header(res.get(), "Content-Type", "application/json");
	switch_web_response_set_header(res.get(), "X-Request-Id", "abc-123");
	CHECK_EQ(internal::response_headers(res.get()).size(), (std::size_t)2);
	CHECK(internal::response_headers(res.get()).count("content-type") == 1);
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
	test_cross_tier_no_conflict_both_orders();
	test_unregister_module_sweep();
	test_unregister_specific();
	test_response_printf_long_output();
	test_snapshot();
	test_register_without_listener();
	test_drain_waits_for_inflight();
	test_register_during_drain_is_refused();
	test_drain_under_concurrency();
	test_overlapping_drains();
	test_invalid_method_rejected();
	test_invalid_mode_rejected();
	test_invalid_registration_path_rejected();
	test_query_params();
	test_allowed_methods();
	test_header_validation();

	wipe();
	std::cout << "\n" << g_pass.load() << " passed, " << g_fail.load() << " failed\n";
	return g_fail.load() ? 1 : 0;
}
