/*
 * Internal interface between the mod_web_server listener and the route
 * registry that lives in libfreeswitch (src/switch_web_server.cpp).
 *
 * Do not include this from anywhere except mod_web_server itself or unit
 * tests for the registry. Public consumers use <switch_web_server.h>.
 *
 * The registry sits in libfreeswitch because APR loads modules with
 * RTLD_LOCAL: a registry inside a module would not be visible to
 * consumers. mod_web_server brings up the Boost.Beast listener and
 * dispatches against this registry.
 */
#ifndef SWITCH_WEB_SERVER_INTERNAL_H
#define SWITCH_WEB_SERVER_INTERNAL_H

#include "switch_web_server.h"

#ifdef __cplusplus

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace switch_web_server_internal {

enum class LookupOutcome {
	Hit,
	MethodNotAllowed,
	NotFound
};

/*
 * ModuleSlot tracks in-flight handler invocations for a single owning
 * module. unregister_module() blocks on its cv until in_flight reaches 0
 * so handler pointers cannot outlive the .so they live in.
 */
struct ModuleSlot {
	std::atomic<int>        in_flight{0};
	/* Set by remove_module() before it waits. The slot stays in the index
	   while draining so a concurrent registration for the same module is
	   rejected rather than silently minting a second slot that the in-flight
	   drain would not be watching. */
	std::atomic<bool>       draining{false};
	std::mutex              mu;
	std::condition_variable cv;
};

/*
 * RAII ticket held by an in-flight dispatch. Constructed on lookup hit;
 * its destructor decrements ModuleSlot::in_flight and notifies the cv
 * when the count reaches zero. Move-only.
 */
class InFlightTicket {
public:
	InFlightTicket() = default;
	explicit InFlightTicket(std::shared_ptr<ModuleSlot> slot);
	~InFlightTicket();
	InFlightTicket(const InFlightTicket&)            = delete;
	InFlightTicket& operator=(const InFlightTicket&) = delete;
	InFlightTicket(InFlightTicket&& o) noexcept;
	InFlightTicket& operator=(InFlightTicket&& o) noexcept;
private:
	std::shared_ptr<ModuleSlot> slot_;
};

/* Scalars carry initializers deliberately: on a NotFound/MethodNotAllowed
   result these members are documented as unread, and an indeterminate handler
   pointer that crosses a module boundary is one stray `if (r.route.handler)`
   away from a jump to garbage. */
struct ResolvedRoute {
	switch_web_method_t     method    = SWITCH_WEB_METHOD_GET;
	switch_web_dispatch_t   mode      = SWITCH_WEB_DISPATCH_LITE;
	switch_web_handler_func handler   = nullptr;
	void                   *user_data = nullptr;
	std::string             module;
	std::string             raw;        /* original path/pattern/prefix */
	std::string             kind;       /* "exact" | "pattern" | "prefix" */
};

struct LookupResult {
	LookupOutcome                                       outcome = LookupOutcome::NotFound;
	ResolvedRoute                                       route;     /* valid only on Hit */
	std::shared_ptr<std::map<std::string, std::string>> params;    /* valid only on Hit, may be null */
	std::set<switch_web_method_t>                       allowed;   /* valid only on MethodNotAllowed */
	InFlightTicket                                      ticket;    /* valid only on Hit; held until handler completes */
};

struct RouteSnapshot {
	std::string           module;
	switch_web_method_t   method;
	switch_web_dispatch_t mode;
	std::string           kind;
	std::string           path;
};

LookupResult                lookup(switch_web_method_t method, const std::string &path);
std::vector<RouteSnapshot>  snapshot();
std::size_t                 route_count();
void                        set_listener_present(bool present);

/* Request / response object factories. mod_web_server constructs these
   on each incoming request; the registry knows how to deallocate them. */

struct RequestInit {
	switch_web_method_t                                 method;
	std::string                                         path;
	std::string                                         query;
	std::string                                         remote_ip;
	std::map<std::string, std::string>                  headers;  /* lower-cased keys */
	std::string                                         body;
	std::shared_ptr<std::map<std::string, std::string>> params;
};

switch_web_request_t  *make_request(RequestInit init);
void                   free_request(switch_web_request_t *req);

switch_web_response_t *make_response();
void                   free_response(switch_web_response_t *res);

/* Custom deleters so request/response can be owned by std::unique_ptr in lambdas. */
struct RequestDeleter  { void operator()(switch_web_request_t  *r) const noexcept { free_request(r);  } };
struct ResponseDeleter { void operator()(switch_web_response_t *r) const noexcept { free_response(r); } };
using RequestPtr  = std::unique_ptr<switch_web_request_t,  RequestDeleter>;
using ResponsePtr = std::unique_ptr<switch_web_response_t, ResponseDeleter>;

/* Read finalized response state after the handler has returned. */
int                                       response_status(const switch_web_response_t *res);
const std::map<std::string, std::string> &response_headers(const switch_web_response_t *res);
const std::string                        &response_body(const switch_web_response_t *res);

const char *method_name(switch_web_method_t m);

} /* namespace switch_web_server_internal */

#endif /* __cplusplus */

#endif /* SWITCH_WEB_SERVER_INTERNAL_H */
