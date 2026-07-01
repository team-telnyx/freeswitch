/*
 * Standalone unit test for rpc_helper.cpp.
 *
 * Includes rpc_helper.cpp directly with stubbed FreeSWITCH symbols, so the
 * test builds without libfreeswitch. g_idle_cpu drives the simulated
 * switch_core_idle_cpu() return so we can exercise the watermark logic
 * deterministically.
 *
 * Build:
 *   g++ -std=c++17 -Wall -O0 -o test_rpc_helper test_rpc_helper.cpp \
 *       -I/usr/include/boost
 * Run:
 *   ./test_rpc_helper
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/* ── Stub the minimal switch.h surface rpc_helper.cpp depends on ───────────── */
typedef enum { SWITCH_FALSE = 0, SWITCH_TRUE = 1 } switch_bool_t;
#define zstr(s) (!(s) || !(s)[0])

static double g_idle_cpu = 100.0;
static inline double switch_core_idle_cpu(void) { return g_idle_cpu; }

/* rpc_helper.cpp does `#include "rpc_helper.h"` which pulls in <switch.h>.
 * Pre-define the header guard so that include is a no-op; then forward-
 * declare the public API manually. */
#define RPC_HELPER_H
extern "C" {
	switch_bool_t is_resource_available(const char *command, const char *api_str);
	switch_bool_t is_api_response_error(const char *response);
	void set_min_idle_cpu_watermark(const char *idle_cpu);
	void set_throttled_api_calls(const char *api);
}

#include "../rpc_helper.cpp"

/* ── Test harness ──────────────────────────────────────────────────────────── */

static int g_failures = 0;
static int g_passes   = 0;

#define CHECK(cond, name) do { \
	if (cond) { \
		++g_passes; \
		printf("  PASS: %s\n", name); \
	} else { \
		++g_failures; \
		printf("  FAIL: %s  (%s:%d)\n", name, __FILE__, __LINE__); \
	} \
} while (0)

/* Reset module-static state before each test. set_throttled_api_calls(NULL)
 * now clears; set_min_idle_cpu_watermark("0") disables the threshold. */
static void reset_state(void)
{
	set_throttled_api_calls(NULL);
	set_min_idle_cpu_watermark("0");
	g_idle_cpu = 100.0;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_no_throttle_configured_allows_all(void)
{
	reset_state();
	/* No throttle list, no watermark → everything allowed regardless of idle. */
	g_idle_cpu = 0.5;
	CHECK(is_resource_available("dynamic_gateway", "list_json") == SWITCH_TRUE,
		"no_throttle: arbitrary cmd allowed even at 0.5% idle");
	CHECK(is_resource_available("bgapi", "originate sofia/foo") == SWITCH_TRUE,
		"no_throttle: bgapi allowed even at 0.5% idle");
}

static void test_watermark_zero_allows_even_with_list(void)
{
	reset_state();
	set_throttled_api_calls("bgapi originate uuid_transfer");
	set_min_idle_cpu_watermark("0");
	g_idle_cpu = 0.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"watermark=0: listed cmd still allowed");
}

static void test_listed_cmd_denied_when_idle_below_threshold(void)
{
	reset_state();
	set_throttled_api_calls("bgapi originate uuid_transfer");
	set_min_idle_cpu_watermark("10");

	g_idle_cpu = 5.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_FALSE,
		"direct call: originate denied at 5%% idle (threshold 10)");

	g_idle_cpu = 50.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"direct call: originate allowed at 50%% idle");
}

/* Regression: pre-fix, ANY cmd not in the throttle list returned SWITCH_FALSE
 * once throttle was configured, because the ternary defaulted to FALSE. */
static void test_unlisted_cmd_allowed_even_when_idle_low(void)
{
	reset_state();
	set_throttled_api_calls("bgapi originate uuid_transfer");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;

	CHECK(is_resource_available("status", "foo") == SWITCH_TRUE,
		"regression: 'status' not in list -> allowed");
	CHECK(is_resource_available("version", "anything") == SWITCH_TRUE,
		"regression: 'version' not in list -> allowed");
	CHECK(is_resource_available("dynamic_gateway", "list_json page=1") == SWITCH_TRUE,
		"regression: 'dynamic_gateway' not in list -> allowed");
}

/* Regression: pre-fix, set_throttled_api_calls stored " name " (padded) and
 * is_throttled_api used substring search, so api_str="originate sofia/foo"
 * never matched " originate " (no leading space). */
static void test_bgapi_subcmd_lookup(void)
{
	reset_state();
	set_throttled_api_calls("originate uuid_transfer");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;

	CHECK(is_resource_available("bgapi", "originate sofia/foo") == SWITCH_FALSE,
		"bgapi: 'originate <args>' denied (subcmd in list)");
	CHECK(is_resource_available("bgapi", "uuid_transfer abc def") == SWITCH_FALSE,
		"bgapi: 'uuid_transfer <args>' denied (subcmd in list)");
	CHECK(is_resource_available("bgapi", "status") == SWITCH_TRUE,
		"bgapi: 'status' allowed (subcmd not in list)");
	CHECK(is_resource_available("bgapi", "") == SWITCH_TRUE,
		"bgapi: empty subcmd allowed");
	CHECK(is_resource_available("bgapi", NULL) == SWITCH_TRUE,
		"bgapi: NULL subcmd allowed");
}

/* Regression: pre-fix, set_throttled_api_calls never cleared the set, so
 * reloading with a new list silently appended. */
static void test_reload_clears_previous_list(void)
{
	reset_state();
	set_throttled_api_calls("originate");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;

	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_FALSE,
		"reload: originate throttled initially");

	/* "Reload" with a different list. originate must no longer be throttled. */
	set_throttled_api_calls("uuid_transfer");
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"reload: originate dropped from list after reset");
	CHECK(is_resource_available("uuid_transfer", "abc") == SWITCH_FALSE,
		"reload: uuid_transfer throttled after reset");
}

/* Regression for the do_config() boundary: do_config() only calls the
 * setters for params that are present and non-empty, so it now resets
 * helper state up front. This mimics that sequence — a reload where
 * throttle-api / throttle-on-idle-cpu have been emptied or removed must
 * end with throttling fully disabled, not with the previous state stuck. */
static void test_config_reload_to_empty_disables_throttle(void)
{
	reset_state();

	/* Initial config load: throttle active. */
	set_throttled_api_calls("bgapi originate uuid_transfer");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_FALSE,
		"config_reload: throttle active after first load");

	/* Reload with the throttle params removed: do_config() resets helper
	 * state, and since the params are absent the setters are not called
	 * again. Modelled here as the bare reset do_config() performs. */
	set_throttled_api_calls(NULL);
	set_min_idle_cpu_watermark("0");
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"config_reload: direct call no longer throttled after params removed");
	CHECK(is_resource_available("bgapi", "originate x") == SWITCH_TRUE,
		"config_reload: bgapi indirection no longer throttled after params removed");
}

/* Same boundary, but the reload supplies an explicitly empty throttle-api
 * value (throttle-api=""). do_config() skips the empty param, so only the
 * up-front reset clears the prior list. */
static void test_config_reload_to_empty_string_disables_throttle(void)
{
	reset_state();
	set_throttled_api_calls("originate");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_FALSE,
		"config_reload_empty: throttle active after first load");

	set_throttled_api_calls(NULL);
	set_min_idle_cpu_watermark("0");
	set_throttled_api_calls("");
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"config_reload_empty: throttle disabled after empty throttle-api");
}

/* Defensive: set_throttled_api_calls must not crash on NULL/empty. */
static void test_set_throttled_handles_null_and_empty(void)
{
	reset_state();
	set_throttled_api_calls(NULL);
	set_throttled_api_calls("");
	set_throttled_api_calls("   ");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;
	/* Empty list means everything is allowed. */
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"null/empty list: nothing throttled");
}

/* Whitespace handling: tabs/multiple spaces in throttle-api param. */
static void test_whitespace_tokenization(void)
{
	reset_state();
	set_throttled_api_calls("  originate\tuuid_transfer   bgapi  ");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_FALSE,
		"tokenization: originate parsed despite leading/trailing whitespace");
	CHECK(is_resource_available("uuid_transfer", "abc") == SWITCH_FALSE,
		"tokenization: uuid_transfer parsed across tabs");
	CHECK(is_resource_available("bgapi", "status") == SWITCH_FALSE,
		"tokenization: bgapi parsed (whole cmd throttled when bgapi in list)");
}

/* Boundary: idle exactly equal to watermark is allowed (strict less-than). */
static void test_idle_exactly_at_watermark_allowed(void)
{
	reset_state();
	set_throttled_api_calls("originate");
	set_min_idle_cpu_watermark("10");

	g_idle_cpu = 10.0;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_TRUE,
		"boundary: idle == watermark allowed");
	g_idle_cpu = 9.999;
	CHECK(is_resource_available("originate", "sofia/foo") == SWITCH_FALSE,
		"boundary: idle just below watermark denied");
}

/* Defensive: NULL/empty cmd should not match an entry. */
static void test_null_or_empty_cmd(void)
{
	reset_state();
	set_throttled_api_calls("originate");
	set_min_idle_cpu_watermark("10");
	g_idle_cpu = 1.0;
	CHECK(is_resource_available(NULL, "originate sofia/foo") == SWITCH_TRUE,
		"null cmd: not throttled (no bgapi indirection match either)");
	CHECK(is_resource_available("", "originate sofia/foo") == SWITCH_TRUE,
		"empty cmd: not throttled");
}

static void test_api_response_error_detection(void)
{
	CHECK(is_api_response_error("-ERR: Not Completed") == SWITCH_TRUE,
		"api_response: -ERR marks failure");
	CHECK(is_api_response_error("  \r\n-ERR invalid argument") == SWITCH_TRUE,
		"api_response: leading whitespace before -ERR marks failure");
	CHECK(is_api_response_error("-USAGE: uuid_media <uuid> [off|on]") == SWITCH_TRUE,
		"api_response: -USAGE marks failure");
	CHECK(is_api_response_error("ERROR!") == SWITCH_TRUE,
		"api_response: switch_api_execute failure marks failure");
	CHECK(is_api_response_error("UNAUTHORIZED!") == SWITCH_TRUE,
		"api_response: authorization failure marks failure");
	CHECK(is_api_response_error("+OK queued") == SWITCH_FALSE,
		"api_response: +OK is success");
	CHECK(is_api_response_error("OK") == SWITCH_FALSE,
		"api_response: plain OK is success");
	CHECK(is_api_response_error(NULL) == SWITCH_FALSE,
		"api_response: NULL response is not classified as API failure");
}

int main(void)
{
	struct { const char *name; void (*fn)(void); } tests[] = {
		{"no_throttle_configured_allows_all",          test_no_throttle_configured_allows_all},
		{"watermark_zero_allows_even_with_list",       test_watermark_zero_allows_even_with_list},
		{"listed_cmd_denied_when_idle_below_threshold",test_listed_cmd_denied_when_idle_below_threshold},
		{"unlisted_cmd_allowed_even_when_idle_low",    test_unlisted_cmd_allowed_even_when_idle_low},
		{"bgapi_subcmd_lookup",                        test_bgapi_subcmd_lookup},
		{"reload_clears_previous_list",                test_reload_clears_previous_list},
		{"config_reload_to_empty_disables_throttle",   test_config_reload_to_empty_disables_throttle},
		{"config_reload_to_empty_string_disables_throttle", test_config_reload_to_empty_string_disables_throttle},
		{"set_throttled_handles_null_and_empty",       test_set_throttled_handles_null_and_empty},
		{"whitespace_tokenization",                    test_whitespace_tokenization},
		{"idle_exactly_at_watermark_allowed",          test_idle_exactly_at_watermark_allowed},
		{"null_or_empty_cmd",                          test_null_or_empty_cmd},
		{"api_response_error_detection",               test_api_response_error_detection},
	};
	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
		printf("[%s]\n", tests[i].name);
		tests[i].fn();
	}
	printf("\n=== Results: %d passed, %d failed ===\n", g_passes, g_failures);
	return g_failures == 0 ? 0 : 1;
}
