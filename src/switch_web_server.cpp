/*
 * mod_web_server registry — compiled into libfreeswitch so that route
 * registrations from any module land in a single process-wide table
 * that mod_web_server's Beast listener can dispatch against.
 *
 * Public ABI:    src/include/switch_web_server.h
 * Mod private:   src/include/switch_web_server_internal.h
 */
#include "switch_web_server.h"
#include "switch_web_server_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <switch.h>

namespace {

using switch_web_server_internal::InFlightTicket;
using switch_web_server_internal::LookupOutcome;
using switch_web_server_internal::LookupResult;
using switch_web_server_internal::ModuleSlot;
using switch_web_server_internal::ResolvedRoute;
using switch_web_server_internal::RouteSnapshot;

struct Entry {
	std::string                  module;
	switch_web_method_t          method;
	switch_web_dispatch_t        mode;
	switch_web_handler_func      handler;
	void                        *user_data;
	std::string                  raw;
	std::shared_ptr<ModuleSlot>  slot;        /* shared with other routes from the same module */
};

struct PatternToken {
	bool        is_param;
	std::string text;
};

struct PatternEntry : public Entry {
	std::vector<PatternToken> tokens;
};

/* The enum values are pinned precisely so a module built against an older
   header keeps meaning what it meant. Reject anything outside the set anyway:
   an out-of-range value overlaps nothing, so it would register successfully,
   never match a request, and never conflict with a legitimate route on the
   same path — a dead endpoint with no diagnostic. method_name() would also
   render it as "?" in the Allow: header. */
bool valid_method(switch_web_method_t m)
{
	switch (m) {
	case SWITCH_WEB_METHOD_GET:
	case SWITCH_WEB_METHOD_POST:
	case SWITCH_WEB_METHOD_PUT:
	case SWITCH_WEB_METHOD_DELETE:
	case SWITCH_WEB_METHOD_PATCH:
	case SWITCH_WEB_METHOD_ANY:
		return true;
	}
	return false;
}

bool method_matches(switch_web_method_t route, switch_web_method_t request)
{
	return route == SWITCH_WEB_METHOD_ANY || route == request;
}

/* True iff two registered methods compete for the same dispatch slot, i.e.
   a request method exists that would match either. ANY overlaps everything. */
bool methods_overlap(switch_web_method_t a, switch_web_method_t b)
{
	return a == b || a == SWITCH_WEB_METHOD_ANY || b == SWITCH_WEB_METHOD_ANY;
}

/* True iff some request path matches both patterns, i.e. their match-sets
   intersect: same segment count and, at every position, at least one side is
   a {capture} or the literals are equal. Param names do not affect matching,
   so "/users/{id}" and "/users/{name}" overlap; "/a/{x}" and "/{y}/b" also
   overlap (both match "/a/b"); "/a/{x}" and "/c/{y}" do not. */
bool patterns_overlap(const std::vector<PatternToken> &a,
                      const std::vector<PatternToken> &b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (a[i].is_param || b[i].is_param) continue;
		if (a[i].text != b[i].text) return false;
	}
	return true;
}

/* True iff every path rooted at `inner` is also matched by prefix `outer`,
   on a segment boundary. "/api" contains "/api/v2" and "/api"; it does not
   contain "/apiv2". A trailing slash in `outer` is itself the boundary. */
bool prefix_segment_contains(const std::string &outer, const std::string &inner)
{
	if (inner.size() < outer.size()) return false;
	if (inner.compare(0, outer.size(), outer) != 0) return false;
	if (inner.size() == outer.size()) return true;
	if (outer.back() == '/') return true;
	return inner[outer.size()] == '/';
}

/* True iff some request path matches both prefixes — i.e. one segment-bounded
   contains the other. "/api" and "/api/v2" overlap; "/api" and "/apiv2" do
   not; "/api" and "/other" do not. */
bool prefixes_overlap(const std::string &a, const std::string &b)
{
	return prefix_segment_contains(a, b) || prefix_segment_contains(b, a);
}

bool is_pattern(const std::string &path)
{
	/* True iff at least one '/'-delimited segment is exactly "{name}". A literal
	   '{' in some other position is not a capture and must not divert the
	   registration into the pattern tier. */
	std::size_t i = 0;
	while (i < path.size()) {
		if (path[i] == '/') { ++i; continue; }
		std::size_t end = path.find('/', i);
		std::size_t seg_len = (end == std::string::npos) ? path.size() - i : end - i;
		if (seg_len >= 2 && path[i] == '{' && path[i + seg_len - 1] == '}') return true;
		i = (end == std::string::npos) ? path.size() : end;
	}
	return false;
}

std::vector<PatternToken> tokenize(const std::string &path)
{
	std::vector<PatternToken> tokens;
	if (path.empty() || path[0] != '/') return tokens;

	std::size_t i = 1;
	while (i <= path.size()) {
		std::size_t slash = path.find('/', i);
		std::string seg = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);

		PatternToken tok;
		if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
			tok.is_param = true;
			tok.text = seg.substr(1, seg.size() - 2);
		} else {
			tok.is_param = false;
			tok.text = seg;
		}
		tokens.push_back(std::move(tok));

		if (slash == std::string::npos) break;
		i = slash + 1;
	}
	return tokens;
}

bool match_pattern(const std::vector<PatternToken> &tokens,
                   const std::string &path,
                   std::map<std::string, std::string> &captures)
{
	if (path.empty() || path[0] != '/') return false;

	std::size_t i = 1;
	std::size_t tidx = 0;
	while (i <= path.size()) {
		if (tidx >= tokens.size()) return false;
		std::size_t slash = path.find('/', i);
		std::string seg = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);

		const PatternToken &t = tokens[tidx];
		if (t.is_param) {
			if (seg.empty()) return false;
			captures[t.text] = seg;
		} else if (t.text != seg) {
			return false;
		}
		++tidx;

		if (slash == std::string::npos) break;
		i = slash + 1;
	}
	return tidx == tokens.size();
}

class Registry {
public:
	static Registry &instance() {
		static Registry r;
		return r;
	}

	switch_status_t add(const std::string &module,
	                    switch_web_method_t method,
	                    const std::string &path,
	                    switch_web_dispatch_t mode,
	                    switch_web_handler_func handler,
	                    void *user_data)
	{
		/* GENERR = malformed call; FALSE = route conflict. Keep them
		   distinct so callers can log "bad input" vs "shadowed route". */
		if (module.empty() || path.empty() || path[0] != '/' || !handler ||
		    !valid_method(method)) {
			return SWITCH_STATUS_GENERR;
		}

		std::unique_lock<std::shared_mutex> lock(mu_);
		if (module_draining_locked(module)) return SWITCH_STATUS_INUSE;
		auto slot = slot_for_locked(module);

		if (is_pattern(path)) {
			auto cand_tokens = tokenize(path);
			for (auto &p : patterns_) {
				if (!methods_overlap(p.method, method)) continue;
				if (!patterns_overlap(p.tokens, cand_tokens)) continue;
				/* Same module re-registering the identical pattern (same raw,
				   same method): update handler/mode in place. */
				if (p.module == module && p.raw == path && p.method == method) {
					p.mode = mode; p.handler = handler; p.user_data = user_data;
					return SWITCH_STATUS_SUCCESS;
				}
				/* Otherwise the match-sets intersect under overlapping methods:
				   reject so dispatch never depends on registration order. */
				return SWITCH_STATUS_FALSE;
			}
			PatternEntry e;
			e.module = module; e.method = method; e.mode = mode;
			e.handler = handler; e.user_data = user_data; e.raw = path; e.slot = slot;
			e.tokens = std::move(cand_tokens);
			patterns_.push_back(std::move(e));
			return SWITCH_STATUS_SUCCESS;
		}

		auto &bucket = exact_[path];
		for (auto &existing : bucket) {
			if (!methods_overlap(existing.method, method)) continue;
			if (existing.module == module && existing.method == method) {
				existing.mode = mode; existing.handler = handler; existing.user_data = user_data;
				return SWITCH_STATUS_SUCCESS;
			}
			return SWITCH_STATUS_FALSE;
		}
		bucket.push_back({module, method, mode, handler, user_data, path, slot});
		return SWITCH_STATUS_SUCCESS;
	}

	switch_status_t add_prefix(const std::string &module,
	                           switch_web_method_t method,
	                           const std::string &prefix,
	                           switch_web_dispatch_t mode,
	                           switch_web_handler_func handler,
	                           void *user_data)
	{
		if (module.empty() || prefix.empty() || prefix[0] != '/' || !handler ||
		    !valid_method(method)) {
			return SWITCH_STATUS_GENERR;
		}

		std::unique_lock<std::shared_mutex> lock(mu_);
		if (module_draining_locked(module)) return SWITCH_STATUS_INUSE;
		auto slot = slot_for_locked(module);

		for (auto &existing : prefixes_) {
			if (!methods_overlap(existing.method, method)) continue;
			if (!prefixes_overlap(existing.raw, prefix)) continue;
			/* Same module re-registering the identical prefix: update in place. */
			if (existing.module == module && existing.raw == prefix && existing.method == method) {
				existing.mode = mode; existing.handler = handler; existing.user_data = user_data;
				return SWITCH_STATUS_SUCCESS;
			}
			/* Overlapping prefixes (one segment-contains the other) under
			   overlapping methods: reject so lookup order cannot decide. */
			return SWITCH_STATUS_FALSE;
		}
		prefixes_.push_back({module, method, mode, handler, user_data, prefix, slot});
		return SWITCH_STATUS_SUCCESS;
	}

	bool remove(const std::string &module,
	            switch_web_method_t method,
	            const std::string &path)
	{
		std::unique_lock<std::shared_mutex> lock(mu_);

		auto eit = exact_.find(path);
		if (eit != exact_.end()) {
			auto &bucket = eit->second;
			auto it = std::find_if(bucket.begin(), bucket.end(), [&](const Entry &e) {
				return e.method == method && e.module == module;
			});
			if (it != bucket.end()) {
				bucket.erase(it);
				if (bucket.empty()) exact_.erase(eit);
				return true;
			}
		}

		auto pit = std::find_if(patterns_.begin(), patterns_.end(), [&](const PatternEntry &p) {
			return p.method == method && p.module == module && p.raw == path;
		});
		if (pit != patterns_.end()) { patterns_.erase(pit); return true; }

		auto prit = std::find_if(prefixes_.begin(), prefixes_.end(), [&](const Entry &e) {
			return e.method == method && e.module == module && e.raw == path;
		});
		if (prit != prefixes_.end()) { prefixes_.erase(prit); return true; }

		return false;
	}

	/* Remove every route registered by `module`, then block until every
	   in-flight handler invocation for that module has returned. The slot
	   is retired from the index but lives on via shared_ptrs held by
	   in-flight tickets; the last ticket release destroys it. */
	/* Loops because two concurrent unregister_module() calls for the same
	   module share one slot: whichever finishes first erases it, the module
	   can then be re-registered, and the loser would otherwise return without
	   draining the NEW slot — with a handler of that module in flight. Going
	   round again re-marks whatever slot is current and drains that too. */
	std::size_t remove_module(const std::string &module)
	{
		std::size_t n = 0;
		for (;;) {
		std::shared_ptr<ModuleSlot> slot;
		{
			std::unique_lock<std::shared_mutex> lock(mu_);

			for (auto it = exact_.begin(); it != exact_.end(); ) {
				auto &bucket = it->second;
				auto before = bucket.size();
				bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
				                            [&](const Entry &e) { return e.module == module; }),
				             bucket.end());
				n += before - bucket.size();
				if (bucket.empty()) it = exact_.erase(it); else ++it;
			}

			auto pbefore = patterns_.size();
			patterns_.erase(std::remove_if(patterns_.begin(), patterns_.end(),
			                               [&](const PatternEntry &p) { return p.module == module; }),
			                patterns_.end());
			n += pbefore - patterns_.size();

			auto rbefore = prefixes_.size();
			prefixes_.erase(std::remove_if(prefixes_.begin(), prefixes_.end(),
			                               [&](const Entry &e) { return e.module == module; }),
			                prefixes_.end());
			n += rbefore - prefixes_.size();

			auto sit = module_slots_.find(module);
			if (sit != module_slots_.end()) {
				slot = sit->second;
				/* Mark, do not erase. Erasing here let a concurrent add() for
				   the same module create a fresh slot: new lookups incremented
				   the new counter while this drain watched the old one, so
				   unregister_module() could return with a handler from that
				   module still running. Leaving the slot in the index and
				   refusing registrations against it closes that. */
				slot->draining.store(true, std::memory_order_release);
			}
		}

		if (!slot) {
			return n;
		}
		{
			/* Scoped so slot->mu is released before mu_ is taken below —
			   otherwise this establishes slot->mu -> mu_ while the ticket
			   paths take slot->mu with no registry lock, an inversion waiting
			   to happen. */
			std::unique_lock<std::mutex> lk(slot->mu);
			/* Predicate-variant wait_for re-checks in_flight on every wake
			   (including spurious ones) and races safely against the notify in
			   InFlightTicket's destructor — a notify fired between our load
			   and the wait is not lost. wait_for returns true when drained,
			   false on the 5s timeout; on timeout we warn and loop. */
			auto drained = [&] {
				return slot->in_flight.load(std::memory_order_acquire) == 0;
			};
			while (!slot->cv.wait_for(lk, std::chrono::seconds(5), drained)) {
				int n_left = slot->in_flight.load(std::memory_order_acquire);
				if (n_left > 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"mod_web_server: still waiting for %d in-flight handler(s) "
						"from module '%s' to return before unload\n",
						n_left, module.c_str());
				}
			}

		}

		/* Drained. Drop the slot so a later re-register starts clean. */
		{
			std::unique_lock<std::shared_mutex> lock(mu_);
			auto sit = module_slots_.find(module);
			if (sit == module_slots_.end()) {
				return n;               /* another drain retired it; nothing live */
			}
			if (sit->second == slot) {
				module_slots_.erase(sit);
				return n;
			}
			/* Our slot was retired by a concurrent drain and the module has
			   been re-slotted since. That new slot may already have tickets,
			   so drain it too rather than returning on a stale guarantee. */
		}
		}
	}

	LookupResult lookup(switch_web_method_t method, const std::string &path) const
	{
		std::shared_lock<std::shared_mutex> lock(mu_);

		LookupResult out;
		out.outcome = LookupOutcome::NotFound;

		auto fill_hit = [&](const Entry &e, const char *kind,
		                    std::shared_ptr<std::map<std::string, std::string>> caps) {
			out.outcome         = LookupOutcome::Hit;
			out.route.method    = e.method;
			out.route.mode      = e.mode;
			out.route.handler   = e.handler;
			out.route.user_data = e.user_data;
			out.route.module    = e.module;
			out.route.raw       = e.raw;
			out.route.kind      = kind;
			out.params          = std::move(caps);
			out.ticket          = InFlightTicket(e.slot);
			out.allowed.clear();
		};

		/* Exact tier — O(1) bucket. Return on first method match. */
		auto eit = exact_.find(path);
		if (eit != exact_.end()) {
			for (const auto &e : eit->second) {
				if (method_matches(e.method, method)) { fill_hit(e, "exact", nullptr); return out; }
				out.allowed.insert(e.method);
			}
		}

		/* Pattern tier — allocate the captures map only when segments actually match. */
		for (const auto &p : patterns_) {
			std::map<std::string, std::string> caps;
			if (!match_pattern(p.tokens, path, caps)) continue;
			if (method_matches(p.method, method)) {
				fill_hit(p, "pattern",
				         std::make_shared<std::map<std::string, std::string>>(std::move(caps)));
				return out;
			}
			out.allowed.insert(p.method);
		}

		/* Prefix tier. Segment-bounded: prefix "/api" matches "/api" and
		   "/api/v2/foo" but NOT "/apiv2"; prefix "/api/" matches "/api/"
		   and "/api/v2/foo". Raw substring matching would silently shadow
		   unrelated routes (e.g. /api would catch /apiv2). */
		for (const auto &pr : prefixes_) {
			if (path.compare(0, pr.raw.size(), pr.raw) != 0) continue;
			if (path.size() > pr.raw.size()
			    && pr.raw.back() != '/'
			    && path[pr.raw.size()] != '/') {
				continue;
			}
			if (method_matches(pr.method, method)) { fill_hit(pr, "prefix", nullptr); return out; }
			out.allowed.insert(pr.method);
		}

		if (!out.allowed.empty()) out.outcome = LookupOutcome::MethodNotAllowed;
		return out;
	}

	std::vector<RouteSnapshot> snapshot() const
	{
		std::shared_lock<std::shared_mutex> lock(mu_);
		std::vector<RouteSnapshot> out;
		out.reserve(exact_.size() + patterns_.size() + prefixes_.size());
		for (const auto &kv : exact_) {
			for (const auto &e : kv.second) {
				out.push_back({e.module, e.method, e.mode, "exact", e.raw});
			}
		}
		for (const auto &p : patterns_)  out.push_back({p.module,  p.method,  p.mode,  "pattern", p.raw});
		for (const auto &pr : prefixes_) out.push_back({pr.module, pr.method, pr.mode, "prefix",  pr.raw});
		return out;
	}

	std::size_t size() const
	{
		std::shared_lock<std::shared_mutex> lock(mu_);
		std::size_t n = patterns_.size() + prefixes_.size();
		for (const auto &kv : exact_) n += kv.second.size();
		return n;
	}

private:
	Registry() = default;

	/* mu_ must be held by the caller. */
	bool module_draining_locked(const std::string &module) const {
		auto it = module_slots_.find(module);
		return it != module_slots_.end() &&
		       it->second->draining.load(std::memory_order_acquire);
	}

	/* mu_ must be held in unique mode by the caller. */
	std::shared_ptr<ModuleSlot> slot_for_locked(const std::string &module) {
		auto it = module_slots_.find(module);
		if (it != module_slots_.end()) return it->second;
		auto s = std::make_shared<ModuleSlot>();
		module_slots_.emplace(module, s);
		return s;
	}

	mutable std::shared_mutex                                       mu_;
	std::unordered_map<std::string, std::vector<Entry>>             exact_;
	std::vector<PatternEntry>                                       patterns_;
	std::vector<Entry>                                              prefixes_;
	std::unordered_map<std::string, std::shared_ptr<ModuleSlot>>    module_slots_;
};

std::atomic<bool> g_listener_present{false};

} /* anonymous namespace */

/* ========================================================================
 * Opaque request/response structs (definitions live in core because
 * accessors are SWITCH_DECLARE'd here)
 * ====================================================================== */

struct switch_web_request_s {
	switch_web_method_t                                 method;
	std::string                                         path;
	std::string                                         query;
	std::string                                         remote_ip;
	std::map<std::string, std::string>                  headers;   /* lower-cased keys */
	std::string                                         body;
	std::shared_ptr<std::map<std::string, std::string>> params;
};

struct switch_web_response_s {
	int                                status = 200;
	std::map<std::string, std::string> headers;
	std::string                        body;
};

/* ========================================================================
 * Internal namespace implementations
 * ====================================================================== */

namespace switch_web_server_internal {

InFlightTicket::InFlightTicket(std::shared_ptr<ModuleSlot> slot) : slot_(std::move(slot))
{
	if (slot_) slot_->in_flight.fetch_add(1, std::memory_order_acq_rel);
}

InFlightTicket::InFlightTicket(InFlightTicket&& o) noexcept : slot_(std::move(o.slot_)) {}

InFlightTicket& InFlightTicket::operator=(InFlightTicket&& o) noexcept
{
	if (this != &o) {
		if (slot_) {
			if (slot_->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				std::lock_guard<std::mutex> lk(slot_->mu);
				slot_->cv.notify_all();
			}
		}
		slot_ = std::move(o.slot_);
	}
	return *this;
}

InFlightTicket::~InFlightTicket()
{
	if (!slot_) return;
	if (slot_->in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		std::lock_guard<std::mutex> lk(slot_->mu);
		slot_->cv.notify_all();
	}
}

LookupResult lookup(switch_web_method_t method, const std::string &path)
{
	return Registry::instance().lookup(method, path);
}

std::vector<RouteSnapshot> snapshot()
{
	return Registry::instance().snapshot();
}

std::size_t route_count()
{
	return Registry::instance().size();
}

void set_listener_present(bool present)
{
	g_listener_present.store(present, std::memory_order_release);
}

switch_web_request_t *make_request(RequestInit init)
{
	auto *r = new switch_web_request_s();
	r->method    = init.method;
	r->path      = std::move(init.path);
	r->query     = std::move(init.query);
	r->remote_ip = std::move(init.remote_ip);
	r->headers   = std::move(init.headers);
	r->body      = std::move(init.body);
	r->params    = std::move(init.params);
	return r;
}

void free_request(switch_web_request_t *req)
{
	delete req;
}

switch_web_response_t *make_response()
{
	return new switch_web_response_s();
}

void free_response(switch_web_response_t *res)
{
	delete res;
}

int response_status(const switch_web_response_t *res)
{
	return res ? res->status : 0;
}

const std::map<std::string, std::string> &response_headers(const switch_web_response_t *res)
{
	static const std::map<std::string, std::string> empty;
	return res ? res->headers : empty;
}

const std::string &response_body(const switch_web_response_t *res)
{
	static const std::string empty;
	return res ? res->body : empty;
}

const char *method_name(switch_web_method_t m)
{
	switch (m) {
	case SWITCH_WEB_METHOD_GET:    return "GET";
	case SWITCH_WEB_METHOD_POST:   return "POST";
	case SWITCH_WEB_METHOD_PUT:    return "PUT";
	case SWITCH_WEB_METHOD_DELETE: return "DELETE";
	case SWITCH_WEB_METHOD_PATCH:  return "PATCH";
	case SWITCH_WEB_METHOD_ANY:    return "ANY";
	}
	return "?";
}

} /* namespace switch_web_server_internal */

/* ========================================================================
 * Public ABI implementations (C linkage)
 * ====================================================================== */

extern "C" {

SWITCH_DECLARE(switch_web_method_t) switch_web_request_method(const switch_web_request_t *req)
{
	return req ? req->method : SWITCH_WEB_METHOD_GET;
}

SWITCH_DECLARE(const char *) switch_web_request_path(const switch_web_request_t *req)
{
	return req ? req->path.c_str() : "";
}

SWITCH_DECLARE(const char *) switch_web_request_query(const switch_web_request_t *req)
{
	return req ? req->query.c_str() : "";
}

SWITCH_DECLARE(const char *) switch_web_request_header(const switch_web_request_t *req, const char *name)
{
	if (!req || !name) return nullptr;
	std::string key(name);
	std::transform(key.begin(), key.end(), key.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	auto it = req->headers.find(key);
	return it == req->headers.end() ? nullptr : it->second.c_str();
}

SWITCH_DECLARE(const char *) switch_web_request_body(const switch_web_request_t *req, size_t *len_out)
{
	if (!req) {
		if (len_out) *len_out = 0;
		return nullptr;
	}
	if (len_out) *len_out = req->body.size();
	return req->body.data();
}

SWITCH_DECLARE(const char *) switch_web_request_remote_ip(const switch_web_request_t *req)
{
	return req ? req->remote_ip.c_str() : "";
}

SWITCH_DECLARE(const char *) switch_web_request_param(const switch_web_request_t *req, const char *name)
{
	if (!req || !name || !req->params) return nullptr;
	auto it = req->params->find(name);
	return it == req->params->end() ? nullptr : it->second.c_str();
}

SWITCH_DECLARE(void) switch_web_response_set_status(switch_web_response_t *res, int code)
{
	if (res) res->status = code;
}

SWITCH_DECLARE(void) switch_web_response_set_header(switch_web_response_t *res, const char *name, const char *value)
{
	if (!res || !name || !value) return;
	/* HTTP header names are case-insensitive; lowercase on insert to dedupe
	   without depending on a case-insensitive map. */
	std::string key(name);
	std::transform(key.begin(), key.end(), key.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	res->headers[std::move(key)] = value;
}

SWITCH_DECLARE(void) switch_web_response_set_body(switch_web_response_t *res, const char *body, size_t len)
{
	if (!res) return;
	if (!body || !len) { res->body.clear(); return; }
	res->body.assign(body, len);
}

SWITCH_DECLARE(void) switch_web_response_printf(switch_web_response_t *res, const char *fmt, ...)
{
	if (!res || !fmt) return;
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int needed = std::vsnprintf(nullptr, 0, fmt, ap);
	va_end(ap);
	if (needed < 0) { va_end(ap2); return; }
	/* Format into an owned char buffer of needed+1 (room for vsnprintf's
	   trailing NUL), then assign exactly `needed` bytes into body. Writing
	   the NUL into std::string's own terminator slot would be relying on a
	   subtle library-contract corner; a vector we own has no such rule. */
	std::vector<char> buf(static_cast<std::size_t>(needed) + 1);
	std::vsnprintf(buf.data(), buf.size(), fmt, ap2);
	va_end(ap2);
	res->body.assign(buf.data(), static_cast<std::size_t>(needed));
}

SWITCH_DECLARE(switch_status_t) switch_web_server_register(const char *module_name,
                                                            switch_web_method_t method,
                                                            const char *path,
                                                            switch_web_dispatch_t mode,
                                                            switch_web_handler_func handler,
                                                            void *user_data)
{
	if (!module_name || !path) return SWITCH_STATUS_GENERR;
	return Registry::instance().add(module_name, method, path, mode, handler, user_data);
}

SWITCH_DECLARE(switch_status_t) switch_web_server_register_prefix(const char *module_name,
                                                                   switch_web_method_t method,
                                                                   const char *prefix,
                                                                   switch_web_dispatch_t mode,
                                                                   switch_web_handler_func handler,
                                                                   void *user_data)
{
	if (!module_name || !prefix) return SWITCH_STATUS_GENERR;
	return Registry::instance().add_prefix(module_name, method, prefix, mode, handler, user_data);
}

SWITCH_DECLARE(switch_status_t) switch_web_server_unregister(const char *module_name,
                                                              switch_web_method_t method,
                                                              const char *path)
{
	if (!module_name || !path) return SWITCH_STATUS_GENERR;
	return Registry::instance().remove(module_name, method, path) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_NOTFOUND;
}

SWITCH_DECLARE(void) switch_web_server_unregister_module(const char *module_name)
{
	if (!module_name) return;
	Registry::instance().remove_module(module_name);
}

SWITCH_DECLARE(switch_bool_t) switch_web_server_available(void)
{
	return g_listener_present.load(std::memory_order_acquire) ? SWITCH_TRUE : SWITCH_FALSE;
}

} /* extern "C" */
