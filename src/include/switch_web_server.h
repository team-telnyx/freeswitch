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

/* Build the response through the switch_web_response_* setters. The RETURN
   VALUE is advisory: anything other than SWITCH_STATUS_SUCCESS logs a WARNING
   naming the module and route, and the response you built is sent regardless.
   To fail a request, set the status you want — do not rely on the return. */
typedef switch_status_t (*switch_web_handler_func)(switch_web_request_t *req,
                                                   switch_web_response_t *res,
                                                   void *user_data);

/* Request accessors. Strings are valid for the lifetime of the handler call. */
SWITCH_DECLARE(switch_web_method_t) switch_web_request_method(const switch_web_request_t *req);
SWITCH_DECLARE(const char *)        switch_web_request_path(const switch_web_request_t *req);
SWITCH_DECLARE(const char *)        switch_web_request_query(const switch_web_request_t *req);
/* Case-insensitive. If the request repeated a field, only the FIRST value is
   kept — they are not comma-joined as RFC 9110 5.3 would allow. Relevant for
   X-Forwarded-For and Cookie, where a proxy chain may legitimately send
   several; read switch_web_request_remote_ip() if you want the peer. */
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
/*!
 * \brief Set a response header, replacing any previous value for that name.
 *
 * `Connection` is not settable: the framework decides keep-alive from the
 * request and overwrites whatever a handler set, without a warning. Return
 * from the handler and let the framework close the connection instead.
 *
 * Names are emitted lowercased, which is legal (field names are
 * case-insensitive) but visible to anyone diffing raw responses.
 *
 * Silently DROPS the header (with a WARNING to the FreeSWITCH log) when it
 * would corrupt the response:
 *   - `name` is empty or is not an RFC 9110 token — anything with a space,
 *     a ':', a separator character, or a byte outside printable ASCII.
 *   - `value` contains a control character. HTAB is permitted, everything
 *     below 0x20 plus DEL is not; CR and LF in particular would let a
 *     caller-supplied value inject extra headers or a body.
 * A dropped header is not an error the caller can observe, so do not build a
 * security control (an auth challenge, a CSP) out of an unvalidated string
 * and assume it reached the wire.
 *
 * `name` is matched case-insensitively, as HTTP field names are.
 */
SWITCH_DECLARE(void) switch_web_response_set_header(switch_web_response_t *res, const char *name, const char *value);
SWITCH_DECLARE(void) switch_web_response_set_body(switch_web_response_t *res, const char *body, size_t len);
SWITCH_DECLARE(void) switch_web_response_printf(switch_web_response_t *res, const char *fmt, ...);

/*
 * Register a route. `path` is an exact path ("/foo") or a pattern with
 * {name} captures ("/users/{id}"), read back via
 * switch_web_request_param(req, "id").
 *
 * IMPORTANT — request paths are matched as RAW BYTES. The dispatcher splits
 * the request target at '?' and compares what is left literally: it does NOT
 * percent-decode and does NOT normalise. Three consequences a handler author
 * must plan for:
 *   - "/%6fk" does NOT match a route registered as "/ok".
 *   - "/x/../y" reaches a prefix route registered on "/x", and both
 *     switch_web_request_path() and any {capture} hand you the dot-segments
 *     verbatim.
 *   - A {capture} value is likewise raw and undecoded, unlike a query
 *     parameter (see switch_web_request_query_param below, which decodes).
 * So a handler that joins a path or a capture onto a directory MUST reject or
 * normalise dot-segments itself — the framework will not do it for you. This
 * matters most for the prefix tier, whose obvious use is serving files.
 *
 * `module_name` MUST be the name this module was loaded as — the `modname`
 * given to switch_loadable_module_create_module_interface(). Routes are keyed
 * by it, and the core sweeps a module's routes on unload under that same name
 * (switch_loadable_module.c, do_shutdown). Register under anything else and
 * that backstop cannot match, leaving handlers pointing into a dlclose()d .so
 * if the module also forgets its own unregister_module() call.
 *
 * Routes live in three tiers — exact, {capture} pattern, and segment-bounded
 * prefix (switch_web_server_register_prefix) — tried in that fixed order. The
 * precedence is structural, never insertion- or load-order dependent, so an
 * exact "/api/health" always wins over a prefix "/api".
 *
 * WITHIN a tier, a registration is rejected when its match-set would intersect
 * an existing route's under an overlapping method, so no request can match two
 * routes in one tier. Overlap is by what a request could match, not string
 * equality:
 *   - exact paths overlap only when identical;
 *   - patterns overlap at equal segment count when, at every position, one
 *     side is a {capture} or the literals match — "/users/{id}" conflicts
 *     with "/users/{name}" (capture names do not affect matching);
 *   - prefixes overlap when one segment-bounded-contains the other — "/api"
 *     conflicts with "/api/v2" but not with "/apiv2".
 * ANY overlaps every specific method and vice versa. The check is symmetric.
 *
 * ACROSS tiers, overlapping match-sets are NOT a conflict: both register and
 * tier precedence decides. Cross-tier shadowing is intentional.
 *
 * Returns:
 *   SUCCESS  — inserted, or the same module re-registered the identical
 *              (method, raw path); handler and mode are updated in place.
 *   FALSE    — route conflict per the overlap rules above.
 *   INUSE    — switch_web_server_unregister_module() is draining this module;
 *              registrations are refused until it returns.
 *   GENERR   — null/empty module_name, null handler, a `method` or `mode`
 *              outside its enum, or a path that does not start with '/' or
 *              contains a space, control character, '?' or '#'. Both enums are
 *              pinned, so an out-of-range value needs a cast. They are rejected
 *              rather than accepted because the results are silent: an unknown
 *              method matches nothing and conflicts with nothing (a dead
 *              endpoint with no diagnostic), and an unknown MODE is worse — the
 *              dispatcher tests `mode == POOL`, so anything else means LITE and
 *              a handler written to block would run on the shared IO strand.
 *              A path containing '?' can never match either, since the target
 *              is split at '?' before routing.
 *
 * Re-registering an existing (method, raw path) swaps handler and user_data in
 * place under the write lock with NO drain — a dispatcher that already
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
 * For logging and operator surfaces ONLY — do NOT gate registration on it.
 * The registry lives in libfreeswitch, so register() succeeds whether or not
 * the listener module is loaded and the route serves the moment it comes up.
 * Gating on TRUE makes your endpoint depend on module load order and drops it
 * permanently whenever your module loads first.
 */
SWITCH_DECLARE(switch_bool_t) switch_web_server_available(void);

SWITCH_END_EXTERN_C

#endif /* SWITCH_WEB_SERVER_H */
