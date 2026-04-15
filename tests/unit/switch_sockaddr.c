#include <switch.h>
#include <test/switch_test.h>

#ifdef HAVE_CARES

#define CARES_NUM_THREADS 10

struct resolve_thread_data {
	const char *hostname;
	int32_t family;
	switch_port_t port;
	switch_status_t result;
	switch_memory_pool_t *pool;
	switch_sockaddr_t *sa;
};

static void *SWITCH_THREAD_FUNC resolve_thread(switch_thread_t *thread, void *obj)
{
	struct resolve_thread_data *data = (struct resolve_thread_data *)obj;

	data->result = switch_sockaddr_info_get(&data->sa, data->hostname, data->family, data->port, 0, data->pool);

	return NULL;
}

#endif /* HAVE_CARES */

FST_MINCORE_BEGIN("./conf")

FST_SUITE_BEGIN(switch_sockaddr)

FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(test_null_hostname_wildcard)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// NULL hostname should bind to wildcard address (0.0.0.0 or ::)
	status = switch_sockaddr_info_get(&sa, NULL, SWITCH_UNSPEC, 5060, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 5060);

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()
/*
FST_TEST_BEGIN(test_numeric_ipv4)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	char ip_str[50] = {0};

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Test numeric IPv4 - should not perform DNS lookup
	status = switch_sockaddr_info_get(&sa, "127.0.0.1", SWITCH_UNSPEC, 8080, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_family(sa), AF_INET);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 8080);

	// Verify the address is correct
	switch_get_addr(ip_str, sizeof(ip_str), sa);
	fst_check_string_equals(ip_str, "127.0.0.1");

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()
*/
FST_TEST_BEGIN(test_numeric_ipv4_public)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	char ip_str[50] = {0};

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Test public DNS server IP (Google DNS)
	status = switch_sockaddr_info_get(&sa, "8.8.8.8", SWITCH_UNSPEC, 53, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_family(sa), AF_INET);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 53);

	// Verify the address
	switch_get_addr(ip_str, sizeof(ip_str), sa);
	fst_check_string_equals(ip_str, "8.8.8.8");

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()
/*
FST_TEST_BEGIN(test_numeric_ipv6_loopback)
{
	switch_memory_pool_t *pool = NULL;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

#if APR_HAVE_IPV6
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	// Test IPv6 loopback - should not perform DNS lookup
	status = switch_sockaddr_info_get(&sa, "::1", SWITCH_INET6, 5060, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_family(sa), AF_INET6);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 5060);
#else
	fst_check(1); // Skip test if IPv6 not supported
#endif

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_numeric_ipv6_with_brackets)
{
	switch_memory_pool_t *pool = NULL;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

#if APR_HAVE_IPV6
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	// Test IPv6 with brackets (URL format) - should not perform DNS lookup
	status = switch_sockaddr_info_get(&sa, "[::1]", SWITCH_INET6, 5060, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_family(sa), AF_INET6);
#else
	fst_check(1); // Skip test if IPv6 not supported
#endif

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_hostname_resolution_localhost)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Test hostname resolution - "localhost" should resolve
	status = switch_sockaddr_info_get(&sa, "localhost", SWITCH_UNSPEC, 9000, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 9000);

	// localhost should resolve to either 127.0.0.1 (IPv4) or ::1 (IPv6)
	fst_check(switch_sockaddr_get_family(sa) == AF_INET || switch_sockaddr_get_family(sa) == AF_INET6);

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_hostname_resolution_google_dns)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	int32_t family;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Test real DNS resolution - dns.google should resolve
	// This tests c-ares when available, APR otherwise
	status = switch_sockaddr_info_get(&sa, "dns.google", SWITCH_UNSPEC, 443, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 443);

	// Verify we got a valid address family (IPv4 or IPv6)
	family = switch_sockaddr_get_family(sa);
	fst_check(family == AF_INET || family == AF_INET6);

	// Log the resolver being used
#ifdef HAVE_CARES
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "DNS resolver: c-ares (async)\n");
#else
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "DNS resolver: APR (blocking)\n");
#endif

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_invalid_hostname)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Test invalid/non-existent hostname - should fail
	status = switch_sockaddr_info_get(&sa, "this-hostname-definitely-does-not-exist-12345.invalid",
	                                   SWITCH_UNSPEC, 80, 0, pool);

	// Should return an error status (not SUCCESS)
	fst_check(status != SWITCH_STATUS_SUCCESS);

	// sa might be NULL or might point to error result depending on implementation
	// Either way is valid

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_family_preference_ipv4)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Request IPv4 specifically for localhost
	status = switch_sockaddr_info_get(&sa, "localhost", SWITCH_INET, 8080, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);
	fst_check_int_equals(switch_sockaddr_get_family(sa), AF_INET);
	fst_check_int_equals(switch_sockaddr_get_port(sa), 8080);

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_family_preference_ipv6)
{
	switch_memory_pool_t *pool = NULL;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

#if APR_HAVE_IPV6
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	// Request IPv6 specifically for localhost
	status = switch_sockaddr_info_get(&sa, "localhost", SWITCH_INET6, 8080, 0, pool);

	// Some systems might not have IPv6 localhost configured
	if (status == SWITCH_STATUS_SUCCESS) {
		fst_requires(sa != NULL);
		fst_check_int_equals(switch_sockaddr_get_family(sa), AF_INET6);
		fst_check_int_equals(switch_sockaddr_get_port(sa), 8080);
	}
#else
	fst_check(1); // Skip test if IPv6 not supported
#endif

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_port_variations)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	const switch_port_t test_ports[] = {0, 1, 80, 443, 5060, 8080, 65535};
	int i;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	for (i = 0; i < sizeof(test_ports) / sizeof(test_ports[0]); i++) {
		status = switch_sockaddr_info_get(&sa, "127.0.0.1", SWITCH_UNSPEC,
		                                   test_ports[i], 0, pool);
		fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
		fst_requires(sa != NULL);
		fst_check_int_equals(switch_sockaddr_get_port(sa), test_ports[i]);
	}

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_multiple_addresses)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	int32_t family;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// google.com typically returns multiple addresses
	// Test that we can at least get the first one
	status = switch_sockaddr_info_get(&sa, "google.com", SWITCH_UNSPEC, 80, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);

	// Verify the port is correct
	fst_check_int_equals(switch_sockaddr_get_port(sa), 80);

	// Verify we got a valid address family
	family = switch_sockaddr_get_family(sa);
	fst_check(family == AF_INET || family == AF_INET6);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
	                 "google.com resolved successfully\n");

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_performance_numeric_vs_dns)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	switch_time_t start, end;
	uint64_t numeric_time, dns_time;
	int i, iterations = 100;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	// Benchmark numeric IP (should be very fast - no DNS)
	start = switch_time_now();
	for (i = 0; i < iterations; i++) {
		status = switch_sockaddr_info_get(&sa, "8.8.8.8", SWITCH_UNSPEC, 53, 0, pool);
		fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	}
	end = switch_time_now();
	numeric_time = end - start;

	// Benchmark DNS resolution (slower - involves DNS lookup)
	start = switch_time_now();
	for (i = 0; i < iterations; i++) {
		status = switch_sockaddr_info_get(&sa, "localhost", SWITCH_UNSPEC, 80, 0, pool);
		fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	}
	end = switch_time_now();
	dns_time = end - start;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
	                 "Performance (%d iterations): numeric IP = %" SWITCH_UINT64_T_FMT "us, "
	                 "DNS lookup = %" SWITCH_UINT64_T_FMT "us\n",
	                 iterations, numeric_time, dns_time);

	// Numeric IP resolution should be significantly faster than DNS
	// (This is informational - we don't enforce strict timing requirements)

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()
*/

#ifdef HAVE_CARES

FST_TEST_BEGIN(test_cares_concurrent_resolve_same_host)
{
	switch_memory_pool_t *pool = NULL;
	switch_thread_t *threads[CARES_NUM_THREADS];
	switch_threadattr_t *thd_attr = NULL;
	struct resolve_thread_data thread_data[CARES_NUM_THREADS];
	int i;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	switch_threadattr_create(&thd_attr, pool);

	/* Launch threads that all resolve the same hostname concurrently */
	for (i = 0; i < CARES_NUM_THREADS; i++) {
		switch_memory_pool_t *tpool = NULL;
		switch_core_new_memory_pool(&tpool);
		fst_requires(tpool != NULL);

		thread_data[i].hostname = "dns.google";
		thread_data[i].family = SWITCH_UNSPEC;
		thread_data[i].port = (switch_port_t)(5060 + i);
		thread_data[i].result = SWITCH_STATUS_FALSE;
		thread_data[i].pool = tpool;
		thread_data[i].sa = NULL;

		switch_thread_create(&threads[i], thd_attr, resolve_thread, &thread_data[i], pool);
	}

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		switch_status_t retval;
		switch_thread_join(&retval, threads[i]);
	}

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		fst_xcheck(thread_data[i].result == SWITCH_STATUS_SUCCESS,
		           "Concurrent resolve thread failed");
		fst_requires(thread_data[i].sa != NULL);
		fst_check_int_equals(switch_sockaddr_get_port(thread_data[i].sa), 5060 + i);
		fst_check(switch_sockaddr_get_family(thread_data[i].sa) == AF_INET ||
		          switch_sockaddr_get_family(thread_data[i].sa) == AF_INET6);

		switch_core_destroy_memory_pool(&thread_data[i].pool);
	}

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_cares_concurrent_resolve_different_hosts)
{
	switch_memory_pool_t *pool = NULL;
	switch_thread_t *threads[CARES_NUM_THREADS];
	switch_threadattr_t *thd_attr = NULL;
	struct resolve_thread_data thread_data[CARES_NUM_THREADS];
	const char *hostnames[] = {
		"dns.google",
		"one.one.one.one",
		"8.8.8.8",
		"1.1.1.1",
		"dns.google",
		"one.one.one.one",
		"8.8.4.4",
		"1.0.0.1",
		"dns.google",
		"one.one.one.one"
	};
	int i;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	switch_threadattr_create(&thd_attr, pool);

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		switch_memory_pool_t *tpool = NULL;
		switch_core_new_memory_pool(&tpool);
		fst_requires(tpool != NULL);

		thread_data[i].hostname = hostnames[i];
		thread_data[i].family = SWITCH_UNSPEC;
		thread_data[i].port = (switch_port_t)(6000 + i);
		thread_data[i].result = SWITCH_STATUS_FALSE;
		thread_data[i].pool = tpool;
		thread_data[i].sa = NULL;

		switch_thread_create(&threads[i], thd_attr, resolve_thread, &thread_data[i], pool);
	}

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		switch_status_t retval;
		switch_thread_join(&retval, threads[i]);
	}

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		fst_xcheck(thread_data[i].result == SWITCH_STATUS_SUCCESS,
		           "Concurrent different-host resolve failed");
		fst_requires(thread_data[i].sa != NULL);
		fst_check_int_equals(switch_sockaddr_get_port(thread_data[i].sa), 6000 + i);

		switch_core_destroy_memory_pool(&thread_data[i].pool);
	}

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_cares_concurrent_resolve_mix_valid_invalid)
{
	switch_memory_pool_t *pool = NULL;
	switch_thread_t *threads[CARES_NUM_THREADS];
	switch_threadattr_t *thd_attr = NULL;
	struct resolve_thread_data thread_data[CARES_NUM_THREADS];
	const char *hostnames[] = {
		"dns.google",
		"nonexistent-host-abc123.invalid",
		"8.8.8.8",
		"another-fake-host-xyz789.invalid",
		"one.one.one.one",
		"no-such-domain-qwerty.invalid",
		"1.1.1.1",
		"dns.google",
		"totally-bogus-host.invalid",
		"8.8.4.4"
	};
	/* Expected: indices 0,2,4,6,7,9 succeed; 1,3,5,8 fail */
	int expect_success[] = {1, 0, 1, 0, 1, 0, 1, 1, 0, 1};
	int i;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	switch_threadattr_create(&thd_attr, pool);

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		switch_memory_pool_t *tpool = NULL;
		switch_core_new_memory_pool(&tpool);
		fst_requires(tpool != NULL);

		thread_data[i].hostname = hostnames[i];
		thread_data[i].family = SWITCH_UNSPEC;
		thread_data[i].port = (switch_port_t)(7000 + i);
		thread_data[i].result = SWITCH_STATUS_FALSE;
		thread_data[i].pool = tpool;
		thread_data[i].sa = NULL;

		switch_thread_create(&threads[i], thd_attr, resolve_thread, &thread_data[i], pool);
	}

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		switch_status_t retval;
		switch_thread_join(&retval, threads[i]);
	}

	for (i = 0; i < CARES_NUM_THREADS; i++) {
		if (expect_success[i]) {
			fst_xcheck(thread_data[i].result == SWITCH_STATUS_SUCCESS,
			           "Expected resolve to succeed");
			fst_requires(thread_data[i].sa != NULL);
			fst_check_int_equals(switch_sockaddr_get_port(thread_data[i].sa), 7000 + i);
		} else {
			fst_xcheck(thread_data[i].result != SWITCH_STATUS_SUCCESS,
			           "Expected resolve to fail for invalid hostname");
		}

		switch_core_destroy_memory_pool(&thread_data[i].pool);
	}

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_cares_rapid_sequential_resolve)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	int i;
	int success_count = 0;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	/* Rapidly resolve the same host many times sequentially.
	 * Exercises reuse of the shared channel without concurrency. */
	for (i = 0; i < 50; i++) {
		sa = NULL;
		status = switch_sockaddr_info_get(&sa, "8.8.8.8", SWITCH_UNSPEC, (switch_port_t)(8000 + i), 0, pool);
		if (status == SWITCH_STATUS_SUCCESS && sa != NULL) {
			success_count++;
			fst_check_int_equals(switch_sockaddr_get_port(sa), 8000 + i);
		}
	}

	fst_check_int_equals(success_count, 50);

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_cares_timeout_invalid_dns)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;
	switch_time_t start, elapsed;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	/* Resolve a hostname that will fail via DNS.
	 * Using .invalid TLD which should return NXDOMAIN quickly,
	 * but verifies the callback fires and unblocks the caller. */
	start = switch_time_now();
	status = switch_sockaddr_info_get(&sa, "timeout-test-host.invalid",
	                                   SWITCH_UNSPEC, 80, 0, pool);
	elapsed = switch_time_now() - start;

	fst_check(status != SWITCH_STATUS_SUCCESS);

	/* Should complete within c-ares timeout (default 1s * 2 tries + margin).
	 * If we're stuck for more than 10 seconds, the condvar wait is broken. */
	fst_xcheck(elapsed < 10000000, "DNS resolve took too long - possible condvar deadlock");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
	                 "Invalid hostname resolve took %" SWITCH_INT64_T_FMT "us\n", (int64_t)elapsed);

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(test_cares_resolve_after_failure)
{
	switch_memory_pool_t *pool = NULL;
	switch_sockaddr_t *sa = NULL;
	switch_status_t status;

	switch_core_new_memory_pool(&pool);
	fst_requires(pool != NULL);

	/* First: fail */
	status = switch_sockaddr_info_get(&sa, "nonexistent-host.invalid",
	                                   SWITCH_UNSPEC, 80, 0, pool);
	fst_check(status != SWITCH_STATUS_SUCCESS);

	/* Second: succeed - verifies the shared channel is still healthy after a failure */
	sa = NULL;
	status = switch_sockaddr_info_get(&sa, "8.8.8.8", SWITCH_UNSPEC, 53, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);

	/* Third: fail again */
	sa = NULL;
	status = switch_sockaddr_info_get(&sa, "another-fake.invalid",
	                                   SWITCH_UNSPEC, 80, 0, pool);
	fst_check(status != SWITCH_STATUS_SUCCESS);

	/* Fourth: succeed again */
	sa = NULL;
	status = switch_sockaddr_info_get(&sa, "dns.google", SWITCH_UNSPEC, 443, 0, pool);
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_requires(sa != NULL);

	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

#endif /* HAVE_CARES */

FST_SUITE_END()

FST_MINCORE_END()
