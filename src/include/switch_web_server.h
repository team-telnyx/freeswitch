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

/*
 * Values are pinned. Modules are routinely built separately from the core, so
 * inserting a verb in the natural place would renumber SWITCH_WEB_METHOD_ANY
 * and a module compiled against the old header would keep passing the old
 * number — silently meaning something else. Append new verbs at 5, 6, ...;
 * ANY sits far above so it never has to move.
 *
 * HEAD is deliberately absent: mod_web_server routes a HEAD request as GET
 * (RFC 9110 §9.3.2) and suppresses the body at write time, so every GET route
 * answers HEAD without registering anything.
 */
typedef enum {
	SWITCH_WEB_METHOD_GET    = 0,
	SWITCH_WEB_METHOD_POST   = 1,
	SWITCH_WEB_METHOD_PUT    = 2,
	SWITCH_WEB_METHOD_DELETE = 3,
	SWITCH_WEB_METHOD_PATCH  = 4,
	SWITCH_WEB_METHOD_ANY    = 1000
} switch_web_method_t;

typedef enum {
	SWITCH_WEB_DISPATCH_LITE = 0,
	SWITCH_WEB_DISPATCH_POOL = 1
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

/*
 * Look up a query-string parameter by name, percent-decoded.
 *
 * Provided so every consumer does not re-derive query parsing, badly: the
 * first two both hand-rolled it and both got it wrong in the same ways —
 * splitting on '&' and decoding the whole piece lets a decoded '=' or '"'
 * change how the rest parses, '+' is not a space unless you map it, and %00
 * silently truncates.
 *
 * Rules: '&' separates pairs, the FIRST '=' separates key from value, both
 * halves are decoded independently, '+' decodes to space, and %XX decodes
 * case-insensitively. A pair whose key or value contains %00 is dropped
 * rather than truncated. A repeated key keeps the first occurrence. A pair
 * with no '=' is ignored.
 *
 * Returns NULL when absent, "" for a present-but-empty value — so a caller
 * can tell "?x=" from no x at all. The pointer is valid for the handler call.
 */
SWITCH_DECLARE(const char *)        switch_web_request_query_param(const switch_web_request_t *req, const char *name);

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
 *   INUSE    — switch_web_server_unregister_module() is currently draining
 *              this module; registrations are refused until it returns.
 *   GENERR   — invalid arguments: null/empty module_name, null/empty path,
 *              path that does not start with '/', or null handler.
 *
 * Re-registering an existing (method, raw path) swaps handler and user_data
 * in place under the write lock, with NO drain — a dispatcher that already
 * resolved the route may still invoke the OLD handler with the OLD user_data.
 * Do not free the previous user_data on the strength of a re-register.
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

/*
 * Remove a single route.
 *
 * WARNING: unlike switch_web_server_unregister_module() below, this does NOT
 * drain. It returns as soon as the route is out of the index, while a handler
 * resolved a moment earlier may still be executing against your user_data.
 * Never use it as the last step before unloading, or before freeing anything a
 * handler touches — use switch_web_server_unregister_module() for that.
 *
 * Returns SUCCESS if a route was removed, NOTFOUND if none matched.
 */
SWITCH_DECLARE(switch_status_t) switch_web_server_unregister(const char *module_name,
                                                             switch_web_method_t method,
                                                             const char *path);

/*
 * Drop every route registered by module_name, then block until every handler
 * of this module that was already in flight has returned. Call it from your
 * SHUTDOWN function, BEFORE freeing anything your handlers dereference
 * (including whatever you passed as user_data) — the drain is what makes that
 * safe.
 *
 * The wait is unbounded; a handler that never returns hangs unload, with a
 * warning every 5s naming the count still outstanding. Two consequences:
 *
 *   - NEVER call this from inside a web handler, including for your own
 *     module. The calling thread holds the in-flight count it is waiting on,
 *     so it deadlocks against itself with no way out but a restart.
 *   - NEVER call your OWN module's API from a handler (switch_api_execute()
 *     on a command your module registered, or anything else taking
 *     PROTECT_INTERFACE on your module_interface). PROTECT_INTERFACE
 *     read-locks that interface's rwlock, and do_shutdown() holds the WRITE
 *     lock on it across your SHUTDOWN function — so the handler blocks on the
 *     lock while this drain waits for the handler. Deadlock, unload only.
 *     Call your implementation function directly instead.
 *   - Calling ANOTHER module's API is not a deadlock — it takes that module's
 *     rwlock, not yours — but it is a shutdown-latency hazard: your drain is
 *     then bounded by how long that module takes to answer, including while
 *     it is itself tearing down. If a handler fans out across modules, give it
 *     a way to abandon the fan-out once teardown starts.
 *
 * Registrations for this module are refused with SWITCH_STATUS_INUSE while
 * the drain is in progress.
 */
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
