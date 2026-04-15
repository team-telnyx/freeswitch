#pragma once

#include <boost/asio.hpp>

#ifdef HAVE_CARES

// c-ares available - use async DNS resolution
#include <ares.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <boost/system/error_code.hpp>

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Defined in switch_core.c — returns whether shared c-ares channel mode is enabled.
// When linked against libfreeswitch (and switch_core.h is included), the declaration
// comes from switch_core.h. For standalone use without FS headers, provide a weak
// default that enables shared channel mode.
#ifndef SWITCH_CORE_H
extern "C" __attribute__((weak)) int switch_core_ares_shared_channel_enabled(void);

int __attribute__((weak)) switch_core_ares_shared_channel_enabled(void)
{
    return 1; // default: shared channel enabled
}
#endif

namespace Telnyx {
namespace Net {

/**
 * Global c-ares library and channel initialization.
 * Ensures ares_library_init() is called exactly once per process.
 * When shared channel mode is enabled (ares-shared-channel=true, the default),
 * maintains a single shared channel for all resolvers.
 * When disabled, only initializes the library; channels are created per-query.
 */
class ares_library_initializer
{
public:
    static ares_library_initializer& instance()
    {
        static ares_library_initializer init;
        return init;
    }

    bool is_initialized() const { return initialized_; }
    ares_channel channel() const { return channel_; }

    static const int DNS_TIMEOUT_MS = 1000;
    static const int DNS_TRIES = 2;

private:

    ares_library_initializer()
        : initialized_(false), channel_(nullptr)
    {
        int status = ares_library_init(ARES_LIB_INIT_ALL);
        if (status != ARES_SUCCESS) {
            return;
        }

        if (switch_core_ares_shared_channel_enabled()) {
            ares_options options;
            memset(&options, 0, sizeof(options));
            options.evsys = ARES_EVSYS_DEFAULT;
            options.timeout = DNS_TIMEOUT_MS;
            options.tries = DNS_TRIES;

            status = ares_init_options(&channel_, &options,
                ARES_OPT_EVENT_THREAD | ARES_OPT_TIMEOUT | ARES_OPT_TRIES);
            if (status != ARES_SUCCESS) {
                channel_ = nullptr;
                // Still usable in per-query mode
            }
        }

        initialized_ = true;
    }

    ~ares_library_initializer()
    {
        if (channel_) {
            ares_cancel(channel_);
            while (ares_queue_wait_empty(channel_, 10000) != ARES_SUCCESS) {
                fprintf(stderr, "c-ares channel %p takes time to cancel\n",
                        static_cast<void*>(channel_));
            }
            ares_destroy(channel_);
            channel_ = nullptr;
        }
    }

    ares_library_initializer(const ares_library_initializer&) = delete;
    ares_library_initializer& operator=(const ares_library_initializer&) = delete;

    bool initialized_;
    ares_channel channel_;
};

/**
 * Generic c-ares based resolver that mimics boost::asio resolver interface.
 * Template parameter InternetProtocol should be one of:
 *   - boost::asio::ip::tcp
 *   - boost::asio::ip::udp
 *   - boost::asio::ip::icmp
 *
 * Usage:
 *   Telnyx::Net::resolver<boost::asio::ip::tcp> tcp_resolver(io_service);
 *   Telnyx::Net::resolver<boost::asio::ip::udp> udp_resolver(io_service);
 *   Telnyx::Net::resolver<boost::asio::ip::icmp> icmp_resolver(io_service);
 */
template <typename InternetProtocol>
class resolver
{
public:
    using protocol_type = InternetProtocol;
    using endpoint_type = typename InternetProtocol::endpoint;

    class query
    {
    public:
        query(const std::string& host, const std::string& service)
            : protocol_(protocol_type::v4()), host_(host), service_(service)
        {
        }

        query(const protocol_type& protocol, const std::string& host, const std::string& service)
            : protocol_(protocol), host_(host), service_(service)
        {
        }

        const protocol_type& protocol() const { return protocol_; }
        const std::string& host_name() const { return host_; }
        const std::string& service_name() const { return service_; }

    private:
        protocol_type protocol_;
        std::string host_;
        std::string service_;
    };

    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = endpoint_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const endpoint_type*;
        using reference = const endpoint_type&;

        // Default constructor creates an "end" iterator
        iterator() : endpoints_(), index_(0) {}

        // Construct from vector - shares ownership via shared_ptr
        iterator(const std::vector<endpoint_type>& endpoints, size_t index = 0)
            : endpoints_(std::make_shared<std::vector<endpoint_type>>(endpoints)), index_(index)
        {
        }

        const endpoint_type& operator*() const
        {
            return (*endpoints_)[index_];
        }

        const endpoint_type* operator->() const
        {
            return &(*endpoints_)[index_];
        }

        iterator& operator++()
        {
            ++index_;
            return *this;
        }

        iterator operator++(int)
        {
            iterator tmp = *this;
            ++index_;
            return tmp;
        }

        bool operator==(const iterator& other) const
        {
            bool this_at_end = !endpoints_ || index_ >= endpoints_->size();
            bool other_at_end = !other.endpoints_ || other.index_ >= other.endpoints_->size();

            if (this_at_end && other_at_end) {
                return true;
            }
            if (this_at_end || other_at_end) {
                return false;
            }
            return endpoints_.get() == other.endpoints_.get() && index_ == other.index_;
        }

        bool operator!=(const iterator& other) const
        {
            return !(*this == other);
        }

        // EndpointSequence support: begin()/end() allow this iterator
        // to be passed directly to boost::asio::connect/async_connect
        iterator begin() const
        {
            if (!endpoints_ || endpoints_->empty()) {
                return iterator();
            }
            return iterator(*endpoints_, 0);
        }

        iterator end() const
        {
            return iterator();
        }

    private:
        std::shared_ptr<std::vector<endpoint_type>> endpoints_;
        size_t index_;
    };

    explicit resolver(boost::asio::io_service& /* io_service */)
    {
        ares_library_initializer::instance();
    }

    ~resolver() = default;

    iterator resolve(const query& q)
    {
        boost::system::error_code ec;
        iterator result = resolve(q, ec);
        if (ec) {
            throw boost::system::system_error(ec);
        }
        return result;
    }

    iterator resolve(const query& q, boost::system::error_code& ec)
    {
        auto& init = ares_library_initializer::instance();
        if (!init.is_initialized()) {
            ec = boost::asio::error::not_connected;
            return iterator();
        }

        std::vector<endpoint_type> endpoints;
        unsigned short port = static_cast<unsigned short>(atoi(q.service_name().c_str()));

        // Fast path: numeric IP addresses don't need DNS resolution
        boost::system::error_code parse_ec;
        boost::asio::ip::address addr = boost::asio::ip::address::from_string(q.host_name(), parse_ec);

        if (!parse_ec) {
            endpoints.push_back(endpoint_type(addr, port));
            ec.clear();
            return iterator(endpoints, 0);
        }

        // Setup hints
        ares_addrinfo_hints hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = get_socktype();
        hints.ai_protocol = get_protocol();

        resolve_result result_ctx;
        result_ctx.endpoints = &endpoints;
        result_ctx.port = port;
        result_ctx.status = ARES_SUCCESS;
        result_ctx.done = false;
        result_ctx.use_condvar = false;

        if (init.channel()) {
            // Shared channel mode: use condvar for per-query synchronization
            result_ctx.use_condvar = true;

            ares_getaddrinfo(init.channel(), q.host_name().c_str(), nullptr, &hints,
                addrinfo_callback, &result_ctx);

            // Block until the callback signals completion.
            // c-ares enforces timeout x tries internally and always fires the
            // callback, so we can wait indefinitely - state lives on the stack.
            {
                std::unique_lock<std::mutex> lock(result_ctx.mtx);
                result_ctx.cv.wait(lock, [&] { return result_ctx.done; });
            }
        } else {
            // Per-query channel mode (legacy)
            ares_channel channel = nullptr;
            ares_options options;
            memset(&options, 0, sizeof(options));
            options.evsys = ARES_EVSYS_DEFAULT;
            options.timeout = ares_library_initializer::DNS_TIMEOUT_MS;
            options.tries = ares_library_initializer::DNS_TRIES;

            int init_status = ares_init_options(&channel, &options,
                ARES_OPT_EVENT_THREAD | ARES_OPT_TIMEOUT | ARES_OPT_TRIES);

            if (init_status != ARES_SUCCESS) {
                ec = boost::system::error_code(init_status, boost::system::system_category());
                return iterator();
            }

            ares_getaddrinfo(channel, q.host_name().c_str(), nullptr, &hints,
                addrinfo_callback, &result_ctx);

            static const int DNS_WAIT_MS =
                ares_library_initializer::DNS_TIMEOUT_MS * ares_library_initializer::DNS_TRIES + 500;
            ares_status_t wait_status = ares_queue_wait_empty(channel, DNS_WAIT_MS);

            if (wait_status == ARES_ETIMEOUT) {
                ares_cancel(channel);
                while (ares_queue_wait_empty(channel, 10000) != ARES_SUCCESS) {
                    fprintf(stderr, "c-ares channel %p takes time to cancel\n",
                            static_cast<void*>(channel));
                }
            }

            ares_destroy(channel);

            if (wait_status == ARES_ETIMEOUT) {
                ec = boost::asio::error::timed_out;
                return iterator();
            }
        }

        // Map result status
        if (result_ctx.status != ARES_SUCCESS) {
            switch (result_ctx.status) {
                case ARES_ENOTFOUND:
                case ARES_ENODATA:
                    ec = boost::asio::error::host_not_found;
                    break;
                case ARES_ETIMEOUT:
                case ARES_ECANCELLED:
                    ec = boost::asio::error::timed_out;
                    break;
                default:
                    ec = boost::system::error_code(result_ctx.status, boost::system::system_category());
                    break;
            }
            return iterator();
        }

        if (endpoints.empty()) {
            ec = boost::asio::error::host_not_found;
            return iterator();
        }

        ec.clear();
        return iterator(endpoints, 0);
    }

    void cancel()
    {
        // No-op: channel lifecycle is managed internally.
    }

private:
    struct resolve_result {
        std::vector<endpoint_type>* endpoints;
        unsigned short port;
        int status;
        std::mutex mtx;
        std::condition_variable cv;
        bool done;
        bool use_condvar;
    };

    static int get_socktype()
    {
        return SOCK_DGRAM;
    }

    static int get_protocol()
    {
        return IPPROTO_UDP;
    }

    static void store_results(resolve_result* ctx, int status, ares_addrinfo* result)
    {
        ctx->status = status;

        if (status == ARES_SUCCESS && result) {
            for (ares_addrinfo_node* node = result->nodes; node != nullptr; node = node->ai_next) {
                if (node->ai_family == AF_INET && node->ai_addr) {
                    sockaddr_in* addr_in = reinterpret_cast<sockaddr_in*>(node->ai_addr);

                    boost::asio::ip::address_v4::bytes_type bytes;
                    memcpy(bytes.data(), &addr_in->sin_addr, sizeof(in_addr));

                    ctx->endpoints->push_back(
                        endpoint_type(boost::asio::ip::address_v4(bytes), ctx->port)
                    );
                }
                else if (node->ai_family == AF_INET6 && node->ai_addr) {
                    sockaddr_in6* addr_in6 = reinterpret_cast<sockaddr_in6*>(node->ai_addr);

                    boost::asio::ip::address_v6::bytes_type bytes;
                    memcpy(bytes.data(), &addr_in6->sin6_addr, sizeof(in6_addr));

                    ctx->endpoints->push_back(
                        endpoint_type(boost::asio::ip::address_v6(bytes), ctx->port)
                    );
                }
            }
            ares_freeaddrinfo(result);
        }
    }

    static void addrinfo_callback(void* arg, int status, int /* timeouts */, ares_addrinfo* result)
    {
        resolve_result* ctx = static_cast<resolve_result*>(arg);

        if (ctx->use_condvar) {
            // Shared channel mode: lock, store, signal
            {
                std::lock_guard<std::mutex> lock(ctx->mtx);
                store_results(ctx, status, result);
                ctx->done = true;
            }
            ctx->cv.notify_one();
        } else {
            // Per-query channel mode: just store results
            store_results(ctx, status, result);
        }
    }

};

// Template specializations for protocol-specific socket types

template <>
inline int resolver<boost::asio::ip::tcp>::get_socktype()
{
    return SOCK_STREAM;
}

template <>
inline int resolver<boost::asio::ip::tcp>::get_protocol()
{
    return IPPROTO_TCP;
}

template <>
inline int resolver<boost::asio::ip::udp>::get_socktype()
{
    return SOCK_DGRAM;
}

template <>
inline int resolver<boost::asio::ip::udp>::get_protocol()
{
    return IPPROTO_UDP;
}

template <>
inline int resolver<boost::asio::ip::icmp>::get_socktype()
{
    return SOCK_RAW;
}

template <>
inline int resolver<boost::asio::ip::icmp>::get_protocol()
{
    return IPPROTO_ICMP;
}

// Convenient type aliases matching boost::asio naming convention
using tcp_resolver = resolver<boost::asio::ip::tcp>;
using udp_resolver = resolver<boost::asio::ip::udp>;
using icmp_resolver = resolver<boost::asio::ip::icmp>;

} // namespace Net
} // namespace Telnyx

#else // !HAVE_CARES

// c-ares not available - fall back to boost::asio resolvers
namespace Telnyx {
namespace Net {

/**
 * Fallback resolver when c-ares is not available.
 * Simply aliases to boost::asio's native resolver.
 */
template <typename InternetProtocol>
using resolver = typename InternetProtocol::resolver;

// Convenient type aliases matching boost::asio naming convention
using tcp_resolver = boost::asio::ip::tcp::resolver;
using udp_resolver = boost::asio::ip::udp::resolver;
using icmp_resolver = boost::asio::ip::icmp::resolver;

} // namespace Net
} // namespace Telnyx

#endif // HAVE_CARES
