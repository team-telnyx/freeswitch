#include <switch.h>
#include <Telnyx/Net/Resolver.h>

#include <string>

#include "mod_test_resolver.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_test_resolver_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_test_resolver_shutdown);
SWITCH_MODULE_DEFINITION(mod_test_resolver, mod_test_resolver_load, mod_test_resolver_shutdown, 0);

template <typename ResolverType>
static switch_status_t do_resolve(const char *hostname, const char *port,
                                   char *result_buf, switch_size_t result_buf_len)
{
	boost::asio::io_service io;
	ResolverType resolver(io);
	boost::system::error_code ec;

	typename ResolverType::query q(hostname, port);
	typename ResolverType::iterator it = resolver.resolve(q, ec);

	if (ec) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
		                  "mod_test_resolver: resolve failed for %s:%s - %s\n",
		                  hostname, port, ec.message().c_str());
		return SWITCH_STATUS_NOTFOUND;
	}

	typename ResolverType::iterator end;
	if (it == end) {
		return SWITCH_STATUS_NOTFOUND;
	}

	std::string addr = it->address().to_string();
	if (addr.length() >= result_buf_len) {
		return SWITCH_STATUS_MEMERR;
	}

	strncpy(result_buf, addr.c_str(), result_buf_len - 1);
	result_buf[result_buf_len - 1] = '\0';

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MOD_DECLARE(switch_status_t) test_resolver_resolve_udp(const char *hostname, const char *port,
                                                               char *result_buf, switch_size_t result_buf_len)
{
	return do_resolve<Telnyx::Net::udp_resolver>(hostname, port, result_buf, result_buf_len);
}

SWITCH_MOD_DECLARE(switch_status_t) test_resolver_resolve_tcp(const char *hostname, const char *port,
                                                               char *result_buf, switch_size_t result_buf_len)
{
	return do_resolve<Telnyx::Net::tcp_resolver>(hostname, port, result_buf, result_buf_len);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_test_resolver_load)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_test_resolver loaded\n");
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_test_resolver_shutdown)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_test_resolver shutdown\n");
	return SWITCH_STATUS_SUCCESS;
}
