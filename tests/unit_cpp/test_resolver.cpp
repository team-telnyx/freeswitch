#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "Telnyx/Net/Resolver.h"

using boost::asio::ip::udp;

static std::atomic<int> pass_count{0};
static std::atomic<int> fail_count{0};
static std::mutex print_mutex;

#define TEST_CHECK(expr, msg) do { \
    if (expr) { \
        pass_count.fetch_add(1); \
    } else { \
        fail_count.fetch_add(1); \
        std::lock_guard<std::mutex> lock(print_mutex); \
        std::cerr << "  FAIL: " << msg << " (" << #expr << ")" << std::endl; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// Basic resolve tests
// ---------------------------------------------------------------------------

static void test_numeric_ipv4()
{
    std::cout << "[test] numeric IPv4 resolve" << std::endl;
    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);
    boost::system::error_code ec;

    Telnyx::Net::udp_resolver::query q(udp::v4(), "8.8.8.8", "53");
    auto it = resolver.resolve(q, ec);

    TEST_CHECK(!ec, "numeric IPv4 should not fail");
    TEST_CHECK(it != Telnyx::Net::udp_resolver::iterator(), "should return at least one result");

    if (it != Telnyx::Net::udp_resolver::iterator()) {
        udp::endpoint ep = *it;
        TEST_CHECK(ep.address().to_string() == "8.8.8.8", "address should be 8.8.8.8");
        TEST_CHECK(ep.port() == 53, "port should be 53");
    }
}

static void test_dns_resolve()
{
    std::cout << "[test] DNS hostname resolve (dns.google)" << std::endl;
    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);
    boost::system::error_code ec;

    Telnyx::Net::udp_resolver::query q(udp::v4(), "dns.google", "443");
    auto it = resolver.resolve(q, ec);

    TEST_CHECK(!ec, "dns.google should resolve");
    TEST_CHECK(it != Telnyx::Net::udp_resolver::iterator(), "should return at least one result");

    if (it != Telnyx::Net::udp_resolver::iterator()) {
        udp::endpoint ep = *it;
        TEST_CHECK(ep.port() == 443, "port should be 443");
    }
}

static void test_invalid_hostname()
{
    std::cout << "[test] invalid hostname resolve" << std::endl;
    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);
    boost::system::error_code ec;

    Telnyx::Net::udp_resolver::query q(udp::v4(), "nonexistent-host-xyz.invalid", "80");
    auto it = resolver.resolve(q, ec);

    TEST_CHECK(ec != boost::system::error_code(), "invalid hostname should fail");
}

// ---------------------------------------------------------------------------
// Shared channel: concurrent resolve
// ---------------------------------------------------------------------------

static const int NUM_THREADS = 10;
static const int RESOLVES_PER_THREAD = 5;

struct ThreadResult {
    int successes = 0;
    int failures = 0;
};

static void concurrent_resolve_thread(int /* thread_id */, const std::string& hostname,
                                       ThreadResult* result)
{
    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);

    for (int i = 0; i < RESOLVES_PER_THREAD; i++) {
        boost::system::error_code ec;
        Telnyx::Net::udp_resolver::query q(hostname, "80");
        auto it = resolver.resolve(q, ec);

        if (!ec && it != Telnyx::Net::udp_resolver::iterator()) {
            result->successes++;
        } else {
            result->failures++;
        }
    }
}

static void test_concurrent_resolve_same_host()
{
    std::cout << "[test] concurrent resolve - same host (" << NUM_THREADS
              << " threads x " << RESOLVES_PER_THREAD << " resolves)" << std::endl;

    std::vector<ThreadResult> results(NUM_THREADS);
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(concurrent_resolve_thread, i, "dns.google", &results[i]);
    }

    for (auto& t : threads) {
        t.join();
    }

    int total_success = 0;
    int total_fail = 0;
    for (auto& r : results) {
        total_success += r.successes;
        total_fail += r.failures;
    }

    TEST_CHECK(total_success == NUM_THREADS * RESOLVES_PER_THREAD,
               "all concurrent resolves should succeed");
    TEST_CHECK(total_fail == 0, "no concurrent resolves should fail");

    std::cout << "  resolved: " << total_success << "/" << (NUM_THREADS * RESOLVES_PER_THREAD)
              << std::endl;
}

static void test_concurrent_resolve_different_hosts()
{
    std::cout << "[test] concurrent resolve - different hosts" << std::endl;

    const std::vector<std::string> hostnames = {
        "dns.google", "one.one.one.one", "8.8.8.8", "1.1.1.1",
        "dns.google", "one.one.one.one", "8.8.4.4", "1.0.0.1",
        "dns.google", "one.one.one.one"
    };

    std::vector<ThreadResult> results(NUM_THREADS);
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(concurrent_resolve_thread, i, hostnames[i], &results[i]);
    }

    for (auto& t : threads) {
        t.join();
    }

    int total_success = 0;
    for (auto& r : results) {
        total_success += r.successes;
    }

    TEST_CHECK(total_success == NUM_THREADS * RESOLVES_PER_THREAD,
               "all different-host resolves should succeed");

    std::cout << "  resolved: " << total_success << "/" << (NUM_THREADS * RESOLVES_PER_THREAD)
              << std::endl;
}

static void test_concurrent_resolve_mix_valid_invalid()
{
    std::cout << "[test] concurrent resolve - mixed valid/invalid" << std::endl;

    const std::vector<std::string> hostnames = {
        "dns.google",                          // valid
        "nonexistent-host-abc123.invalid",     // invalid
        "8.8.8.8",                             // valid
        "another-fake-host-xyz789.invalid",    // invalid
        "one.one.one.one",                     // valid
        "no-such-domain-qwerty.invalid",       // invalid
        "1.1.1.1",                             // valid
        "dns.google",                          // valid
        "totally-bogus-host.invalid",          // invalid
        "8.8.4.4"                              // valid
    };
    bool expect_success[] = {true, false, true, false, true, false, true, true, false, true};

    std::vector<ThreadResult> results(NUM_THREADS);
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back([i, &hostnames, &results]() {
            boost::asio::io_service io;
            Telnyx::Net::udp_resolver resolver(io);
            boost::system::error_code ec;

            Telnyx::Net::udp_resolver::query q(hostnames[i], "80");
            auto it = resolver.resolve(q, ec);

            if (!ec && it != Telnyx::Net::udp_resolver::iterator()) {
                results[i].successes = 1;
            } else {
                results[i].failures = 1;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        if (expect_success[i]) {
            TEST_CHECK(results[i].successes == 1,
                       std::string("expected success for ") + hostnames[i]);
        } else {
            TEST_CHECK(results[i].failures == 1,
                       std::string("expected failure for ") + hostnames[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Timeout / deadlock detection
// ---------------------------------------------------------------------------

static void test_timeout_no_deadlock()
{
    std::cout << "[test] invalid hostname completes without deadlock" << std::endl;

    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);
    boost::system::error_code ec;

    auto start = std::chrono::steady_clock::now();

    Telnyx::Net::udp_resolver::query q("timeout-test-host.invalid", "80");
    auto it = resolver.resolve(q, ec);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    TEST_CHECK(ec != boost::system::error_code(), "invalid hostname should fail");
    TEST_CHECK(elapsed.count() < 10000, "should complete within 10 seconds");

    std::cout << "  elapsed: " << elapsed.count() << " ms" << std::endl;
}

// ---------------------------------------------------------------------------
// Channel health after failures
// ---------------------------------------------------------------------------

static void test_resolve_after_failure()
{
    std::cout << "[test] resolve succeeds after prior failure" << std::endl;

    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);
    boost::system::error_code ec;

    // 1. Fail
    {
        Telnyx::Net::udp_resolver::query q("nonexistent-host.invalid", "80");
        resolver.resolve(q, ec);
        TEST_CHECK(ec != boost::system::error_code(), "first resolve should fail");
    }

    // 2. Succeed
    {
        Telnyx::Net::udp_resolver::query q(udp::v4(), "8.8.8.8", "53");
        auto it = resolver.resolve(q, ec);
        TEST_CHECK(!ec, "resolve after failure should succeed");
        TEST_CHECK(it != Telnyx::Net::udp_resolver::iterator(), "should return results");
    }

    // 3. Fail again
    {
        Telnyx::Net::udp_resolver::query q("another-fake.invalid", "80");
        resolver.resolve(q, ec);
        TEST_CHECK(ec != boost::system::error_code(), "second failure should fail");
    }

    // 4. Succeed again
    {
        Telnyx::Net::udp_resolver::query q("dns.google", "443");
        auto it = resolver.resolve(q, ec);
        TEST_CHECK(!ec, "second resolve after failure should succeed");
        TEST_CHECK(it != Telnyx::Net::udp_resolver::iterator(), "should return results");
    }
}

// ---------------------------------------------------------------------------
// Rapid sequential
// ---------------------------------------------------------------------------

static void test_rapid_sequential()
{
    std::cout << "[test] rapid sequential resolve (50 iterations)" << std::endl;

    boost::asio::io_service io;
    Telnyx::Net::udp_resolver resolver(io);
    int success_count = 0;

    for (int i = 0; i < 50; i++) {
        boost::system::error_code ec;
        Telnyx::Net::udp_resolver::query q(udp::v4(), "8.8.8.8", "53");
        auto it = resolver.resolve(q, ec);
        if (!ec && it != Telnyx::Net::udp_resolver::iterator()) {
            success_count++;
        }
    }

    TEST_CHECK(success_count == 50, "all 50 sequential resolves should succeed");
    std::cout << "  resolved: " << success_count << "/50" << std::endl;
}

// ---------------------------------------------------------------------------
// Boost vs c-ares comparison (original test)
// ---------------------------------------------------------------------------

struct ResolveResult {
    std::string resolver_type;
    std::vector<std::string> addresses;
    bool success;
    std::string error_message;
    long long duration_ms;
};

static ResolveResult test_boost_resolver(boost::asio::io_service& io_service,
                                          const std::string& host,
                                          const std::string& port)
{
    ResolveResult result;
    result.resolver_type = "Boost.ASIO";

    auto start = std::chrono::high_resolution_clock::now();

    try {
        udp::resolver resolver(io_service);
        udp::resolver::query query(udp::v4(), host, port);

        udp::resolver::iterator iter = resolver.resolve(query);
        udp::resolver::iterator end;

        while (iter != end) {
            udp::endpoint endpoint = *iter;
            result.addresses.push_back(endpoint.address().to_string());
            ++iter;
        }

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    return result;
}

static ResolveResult test_cares_resolver(boost::asio::io_service& io_service,
                                          const std::string& host,
                                          const std::string& port)
{
    ResolveResult result;
    result.resolver_type = "c-ares";

    auto start = std::chrono::high_resolution_clock::now();

    try {
        Telnyx::Net::udp_resolver resolver(io_service);
        Telnyx::Net::udp_resolver::query query(udp::v4(), host, port);

        boost::system::error_code ec;
        Telnyx::Net::udp_resolver::iterator iter = resolver.resolve(query, ec);
        Telnyx::Net::udp_resolver::iterator end;

        if (ec) {
            throw boost::system::system_error(ec);
        }

        while (iter != end) {
            udp::endpoint endpoint = *iter;
            result.addresses.push_back(endpoint.address().to_string());
            ++iter;
        }

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    return result;
}

static void print_result(const ResolveResult& result)
{
    std::cout << "  Resolver: " << result.resolver_type << std::endl;
    std::cout << "  Duration: " << result.duration_ms << " ms" << std::endl;

    if (result.success) {
        std::cout << "  Status:   SUCCESS" << std::endl;
        std::cout << "  Addresses (" << result.addresses.size() << "):" << std::endl;
        for (const auto& addr : result.addresses) {
            std::cout << "    - " << addr << std::endl;
        }
    } else {
        std::cout << "  Status:   FAILED" << std::endl;
        std::cout << "  Error:    " << result.error_message << std::endl;
    }
}

static bool compare_results(const ResolveResult& boost_result, const ResolveResult& cares_result)
{
    if (boost_result.success != cares_result.success) {
        std::cout << "  WARNING: Different success status!" << std::endl;
        return false;
    }

    if (!boost_result.success) {
        return true;
    }

    if (boost_result.addresses.size() != cares_result.addresses.size()) {
        std::cout << "  WARNING: Different number of addresses!" << std::endl;
        return false;
    }

    for (const auto& addr : boost_result.addresses) {
        if (std::find(cares_result.addresses.begin(),
                      cares_result.addresses.end(),
                      addr) == cares_result.addresses.end()) {
            std::cout << "  WARNING: Address " << addr << " found in Boost but not in c-ares!" << std::endl;
            return false;
        }
    }

    std::cout << "  Results match" << std::endl;
    return true;
}

static void test_comparison(boost::asio::io_service& io_service,
                             const std::string& host,
                             const std::string& port)
{
    std::cout << std::string(70, '-') << std::endl;
    std::cout << "Comparing: " << host << ":" << port << std::endl;

    std::cout << std::endl << "Boost.ASIO Resolver:" << std::endl;
    ResolveResult boost_result = test_boost_resolver(io_service, host, port);
    print_result(boost_result);

    std::cout << std::endl << "c-ares Resolver:" << std::endl;
    ResolveResult cares_result = test_cares_resolver(io_service, host, port);
    print_result(cares_result);

    std::cout << std::endl << "Comparison:" << std::endl;
    compare_results(boost_result, cares_result);
    std::cout << std::endl;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::cout << std::string(70, '=') << std::endl;
    std::cout << "Telnyx::Net::Resolver Test Suite" << std::endl;
    std::cout << "Shared c-ares channel: concurrency, timeouts, recovery" << std::endl;
    std::cout << std::string(70, '=') << std::endl << std::endl;

    // Basic tests
    test_numeric_ipv4();
    test_dns_resolve();
    test_invalid_hostname();

    // Shared channel concurrency tests
    test_concurrent_resolve_same_host();
    test_concurrent_resolve_different_hosts();
    test_concurrent_resolve_mix_valid_invalid();

    // Timeout / deadlock
    test_timeout_no_deadlock();

    // Channel health after failures
    test_resolve_after_failure();

    // Rapid sequential
    test_rapid_sequential();

    // Boost vs c-ares comparison
    std::cout << std::endl << std::string(70, '=') << std::endl;
    std::cout << "Boost.ASIO vs c-ares Comparison" << std::endl;
    std::cout << std::string(70, '=') << std::endl;

    boost::asio::io_service io_service;

    std::vector<std::pair<std::string, std::string>> comparison_cases = {
        {"8.8.8.8", "53"},
        {"127.0.0.1", "9060"},
        {"google.com", "80"},
        {"github.com", "443"},
        {"this-domain-definitely-does-not-exist-12345.com", "80"},
    };

    if (argc >= 3) {
        comparison_cases.clear();
        comparison_cases.push_back({argv[1], argv[2]});
    } else if (argc == 2) {
        comparison_cases.clear();
        comparison_cases.push_back({argv[1], "80"});
    }

    for (const auto& tc : comparison_cases) {
        test_comparison(io_service, tc.first, tc.second);
    }

    // Summary
    std::cout << std::string(70, '=') << std::endl;
    int total = pass_count.load() + fail_count.load();
    std::cout << "Results: " << pass_count.load() << "/" << total << " passed";
    if (fail_count.load() > 0) {
        std::cout << ", " << fail_count.load() << " FAILED";
    }
    std::cout << std::endl;
    std::cout << std::string(70, '=') << std::endl;

    return fail_count.load() > 0 ? 1 : 0;
}
