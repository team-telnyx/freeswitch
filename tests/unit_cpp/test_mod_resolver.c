#include <switch.h>
#include <test/switch_test.h>

#include "mod_test_resolver.h"

FST_CORE_BEGIN(".")

FST_MODULE_BEGIN(mod_test_resolver, test_mod_resolver)

FST_SETUP_BEGIN()
{
	fst_requires_module("mod_test_resolver");
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(test_udp_resolve_numeric_ip)
{
	char buf[64] = {0};
	switch_status_t status;

	status = test_resolver_resolve_udp("8.8.8.8", "53", buf, sizeof(buf));
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_check_string_equals(buf, "8.8.8.8");
}
FST_TEST_END()

FST_TEST_BEGIN(test_tcp_resolve_numeric_ip)
{
	char buf[64] = {0};
	switch_status_t status;

	status = test_resolver_resolve_tcp("1.1.1.1", "443", buf, sizeof(buf));
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_check_string_equals(buf, "1.1.1.1");
}
FST_TEST_END()

FST_TEST_BEGIN(test_udp_resolve_hostname)
{
	char buf[64] = {0};
	switch_status_t status;

	status = test_resolver_resolve_udp("dns.google", "53", buf, sizeof(buf));
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_check(strlen(buf) > 0);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
	                  "dns.google resolved to %s\n", buf);
}
FST_TEST_END()

FST_TEST_BEGIN(test_tcp_resolve_hostname)
{
	char buf[64] = {0};
	switch_status_t status;

	status = test_resolver_resolve_tcp("dns.google", "443", buf, sizeof(buf));
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_check(strlen(buf) > 0);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
	                  "dns.google (TCP) resolved to %s\n", buf);
}
FST_TEST_END()

FST_TEST_BEGIN(test_resolve_invalid_hostname)
{
	char buf[64] = {0};
	switch_status_t status;

	status = test_resolver_resolve_udp("nonexistent-host.invalid", "80", buf, sizeof(buf));
	fst_check(status != SWITCH_STATUS_SUCCESS);
}
FST_TEST_END()

FST_TEST_BEGIN(test_resolve_after_failure)
{
	char buf[64] = {0};
	switch_status_t status;

	/* Fail */
	status = test_resolver_resolve_udp("bogus.invalid", "80", buf, sizeof(buf));
	fst_check(status != SWITCH_STATUS_SUCCESS);

	/* Succeed - shared channel still healthy */
	memset(buf, 0, sizeof(buf));
	status = test_resolver_resolve_udp("8.8.8.8", "53", buf, sizeof(buf));
	fst_check_int_equals(status, SWITCH_STATUS_SUCCESS);
	fst_check_string_equals(buf, "8.8.8.8");
}
FST_TEST_END()

FST_TEST_BEGIN(test_rapid_sequential_resolve_via_module)
{
	int i;
	int success_count = 0;

	for (i = 0; i < 20; i++) {
		char buf[64] = {0};
		switch_status_t status;

		status = test_resolver_resolve_udp("8.8.8.8", "53", buf, sizeof(buf));
		if (status == SWITCH_STATUS_SUCCESS && strlen(buf) > 0) {
			success_count++;
		}
	}

	fst_check_int_equals(success_count, 20);
}
FST_TEST_END()

FST_MODULE_END()

FST_CORE_END()
