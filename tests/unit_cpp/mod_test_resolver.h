#ifndef MOD_TEST_RESOLVER_H
#define MOD_TEST_RESOLVER_H

#include <switch.h>

SWITCH_BEGIN_EXTERN_C

SWITCH_MOD_DECLARE(switch_status_t) test_resolver_resolve_udp(const char *hostname, const char *port,
                                                               char *result_buf, switch_size_t result_buf_len);

SWITCH_MOD_DECLARE(switch_status_t) test_resolver_resolve_tcp(const char *hostname, const char *port,
                                                               char *result_buf, switch_size_t result_buf_len);

SWITCH_END_EXTERN_C

#endif /* MOD_TEST_RESOLVER_H */
