/*
 * mod_web_server public ABI.
 *
 * Lets any FreeSWITCH module register HTTP route handlers on a shared
 * Boost.Beast listener owned by mod_web_server. Two dispatch modes:
 *
 *   LITE: handler runs inline on the connection's IO strand.
 *         MUST NOT block. Use for in-memory probes (/health, /metrics).
 *
 *   POOL: handler runs on a worker thread pool. May block on I/O.
 *         Response is delivered back to the connection asynchronously.
 *
 * Callers MUST call switch_web_server_unregister_module() from their
 * SWITCH_MODULE_SHUTDOWN_FUNCTION so routes do not outlive the code
 * that backs them.
 */
#ifndef SWITCH_WEB_SERVER_H
#define SWITCH_WEB_SERVER_H

#include <switch.h>

SWITCH_BEGIN_EXTERN_C

typedef enum {
	SWITCH_WEB_METHOD_GET,
	SWITCH_WEB_METHOD_POST,
	SWITCH_WEB_METHOD_PUT,
	SWITCH_WEB_METHOD_DELETE,
	SWITCH_WEB_METHOD_PATCH,
	SWITCH_WEB_METHOD_ANY
} switch_web_method_t;

typedef enum {
	SWITCH_WEB_DISPATCH_LITE,
	SWITCH_WEB_DISPATCH_POOL
} switch_web_dispatch_t;

typedef struct switch_web_request_s  switch_web_request_t;
typedef struct switch_web_response_s switch_web_response_t;

typedef switch_status_t (*switch_web_handler_func)(switch_web_request_t *req,
                                                   switch_web_response_t *res,
                                                   void *user_data);

/* Request accessors. Strings are valid for the lifetime of the handler call. */
SWITCH_DECLARE(switch_web_method_t) switch_web_request_method(const switch_web_request_t *req);
SWITCH_DECLARE(const char *)        switch_web_request_path(const switch_web_request_t *req);
SWITCH_DECLARE(const char *)        switch_web_request_query(const switch_web_request_t *req);
SWITCH_DECLARE(const char *)        switch_web_request_header(const switch_web_request_t *req, const char *name);
SWITCH_DECLARE(const char *)        switch_web_request_body(const switch_web_request_t *req, size_t *len_out);
SWITCH_DECLARE(const char *)        switch_web_request_remote_ip(const switch_web_request_t *req);
SWITCH_DECLARE(const char *)        switch_web_request_param(const switch_web_request_t *req, const char *name);

/* Response setters. set_body / printf may be called multiple times; the last call wins. */
SWITCH_DECLARE(void) switch_web_response_set_status(switch_web_response_t *res, int code);
SWITCH_DECLARE(void) switch_web_response_set_header(switch_web_response_t *res, const char *name, const char *value);
SWITCH_DECLARE(void) switch_web_response_set_body(switch_web_response_t *res, const char *body, size_t len);
SWITCH_DECLARE(void) switch_web_response_printf(switch_web_response_t *res, const char *fmt, ...);

/*
 * Register a route. `path` is an exact path ("/foo") or a pattern with
 * {name} captures ("/users/{id}"). Captures are read via
 * switch_web_request_param(req, "id").
 *
 * Routes live in three tiers — exact paths, {capture} patterns, and
 * segment-bounded prefixes (switch_web_server_register_prefix) — and a
 * lookup tries them in that fixed order: exact, then pattern, then prefix.
 * That precedence is structural (decided by tier, never by insertion or
 * module-load order), so a more specific route intentionally and
 * deterministically shadows a less specific one. Example: an exact
 * "/api/health" still wins even if a prefix "/api" is also registered.
 *
 * Contract: WITHIN a single tier, a registration is rejected whenever its
 * match-set would intersect an existing route's under an overlapping
 * method — so no request can match two routes in the same tier, and the
 * winner never depends on registration order. Overlap is decided by what a
 * request could match, not by raw string equality:
 *   - exact paths overlap only when identical;
 *   - patterns overlap when they have the same segment count and, at every
 *     position, one side is a {capture} or the literals match — so
 *     "/users/{id}" conflicts with "/users/{name}" (param names do not
 *     affect matching);
 *   - prefixes overlap when one segment-bounded-contains the other — so
 *     "/api" conflicts with "/api/v2" but not with "/apiv2".
 * ANY overlaps every specific method (and vice versa). The check is
 * symmetric, so the outcome is the same in either registration order.
 * ACROSS tiers, overlapping match-sets are NOT a conflict: both routes
 * register and the tier precedence above decides which one a request
 * reaches. Cross-tier shadowing is allowed and intentional.
 *
 * Returns:
 *   SUCCESS  — inserted, or the same module re-registered the identical
 *              (method, raw path) — handler and mode are updated in place.
 *   FALSE    — route conflict per the overlap rules above.
 *   GENERR   — invalid arguments: null/empty module_name, null/empty path,
 *              path that does not start with '/', or null handler.
 */
SWITCH_DECLARE(switch_status_t) switch_web_server_register(const char *module_name,
                                                           switch_web_method_t method,
                                                           const char *path,
                                                           switch_web_dispatch_t mode,
                                                           switch_web_handler_func handler,
                                                           void *user_data);

/* Prefix route: matches request paths bounded on a path segment. Prefix
   "/api" matches "/api" and "/api/v2/foo" but NOT "/apiv2"; prefix "/api/"
   matches "/api/" and "/api/v2/foo". Walked after exact + pattern misses.
   Conflict and return semantics match switch_web_server_register above. */
SWITCH_DECLARE(switch_status_t) switch_web_server_register_prefix(const char *module_name,
                                                                  switch_web_method_t method,
                                                                  const char *prefix,
                                                                  switch_web_dispatch_t mode,
                                                                  switch_web_handler_func handler,
                                                                  void *user_data);

SWITCH_DECLARE(switch_status_t) switch_web_server_unregister(const char *module_name,
                                                             switch_web_method_t method,
                                                             const char *path);

/* Drop every route registered by module_name. Call from your module's SHUTDOWN. */
SWITCH_DECLARE(void) switch_web_server_unregister_module(const char *module_name);

/*
 * Reports whether a mod_web_server listener is currently up.
 *
 * This is for logging and operator surfaces ONLY. Do NOT gate registration on
 * it. The registry lives in libfreeswitch, not in mod_web_server, so
 * switch_web_server_register() succeeds whether or not the listener module is
 * loaded, and the route starts being served the moment mod_web_server comes
 * up. Registering only when this returns TRUE makes your endpoint depend on
 * module load order and silently drops it forever whenever your module happens
 * to load first.
 */
SWITCH_DECLARE(switch_bool_t) switch_web_server_available(void);

SWITCH_END_EXTERN_C

#endif /* SWITCH_WEB_SERVER_H */
