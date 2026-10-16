/*
 * C-side shims for test_web_server_registry.cpp.
 *
 * The registry must reject a `method` or `mode` outside its enum — the value a
 * module built against a different header would pass. In C++ such a value
 * cannot even be formed without UB (neither enum has a fixed underlying type),
 * so the test cannot construct one itself. In C an enum is just an integer
 * type and any int converts to it, which is exactly what a real C module
 * does. So the out-of-range calls are made from here.
 */
#include "switch.h"
#include "switch_web_server.h"

switch_status_t wsr_register_raw(const char *module_name, int method, const char *path,
                                 int mode, switch_web_handler_func handler)
{
	return switch_web_server_register(module_name, (switch_web_method_t)method, path,
	                                  (switch_web_dispatch_t)mode, handler, NULL);
}

switch_status_t wsr_register_prefix_raw(const char *module_name, int method, const char *prefix,
                                        int mode, switch_web_handler_func handler)
{
	return switch_web_server_register_prefix(module_name, (switch_web_method_t)method, prefix,
	                                         (switch_web_dispatch_t)mode, handler, NULL);
}

switch_status_t wsr_unregister_raw(const char *module_name, int method, const char *path)
{
	return switch_web_server_unregister(module_name, (switch_web_method_t)method, path);
}
