// test_zmqsocket_timeouts.cpp
//
// Regression test for Telnyx::ZMQ::ZMQSocket timeout configuration.
// Asserts that PUSH/PULL sockets have finite SNDTIMEO/RCVTIMEO and
// that sendRequest() returns false on an unresponsive peer instead
// of blocking forever (a watchdog kills the test if it hangs).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "Telnyx/ZMQ/ZMQSocket.h"

namespace {

std::atomic<int> pass_count{0};
std::atomic<int> fail_count{0};

#define TEST_CHECK(expr, msg) do {                                       \
        if (expr) {                                                      \
            pass_count.fetch_add(1);                                     \
        } else {                                                         \
            fail_count.fetch_add(1);                                     \
            std::cerr << "  FAIL: " << msg                               \
                      << "  (" << #expr << ")" << std::endl;             \
        }                                                                \
    } while (0)

// ---------------------------------------------------------------------
// Test 1 — PUSH socket must have ZMQ_SNDTIMEO > 0 set by the wrapper.
// ---------------------------------------------------------------------
void test_push_socket_has_finite_send_timeout()
{
    std::cout << "[test] PUSH socket has finite ZMQ_SNDTIMEO" << std::endl;

    Telnyx::ZMQ::ZMQSocket sender(Telnyx::ZMQ::ZMQSocket::PUSH);
    bool connected = sender.connect("tcp://127.0.0.1:53557");
    TEST_CHECK(connected, "PUSH should connect to a local URL");
    if (!sender.socket()) {
        TEST_CHECK(false, "underlying zmq::socket_t* unavailable");
        return;
    }

    int sndtimeo = -2;
    size_t sz = sizeof(sndtimeo);
    sender.socket()->getsockopt(ZMQ_SNDTIMEO, &sndtimeo, &sz);
    std::cout << "  ZMQ_SNDTIMEO = " << sndtimeo << " ms" << std::endl;

    TEST_CHECK(sndtimeo != -1,
        "ZMQ_SNDTIMEO must not be the libzmq default (-1 = block forever)");
    TEST_CHECK(sndtimeo > 0,
        "ZMQ_SNDTIMEO must be a positive finite value");
    TEST_CHECK(sndtimeo <= 60000,
        "ZMQ_SNDTIMEO should be <= 60s (sanity cap)");
}

// ---------------------------------------------------------------------
// Test 2 — PULL socket must have ZMQ_RCVTIMEO > 0 set by the wrapper.
// ---------------------------------------------------------------------
void test_pull_socket_has_finite_recv_timeout()
{
    std::cout << "[test] PULL socket has finite ZMQ_RCVTIMEO" << std::endl;

    Telnyx::ZMQ::ZMQSocket receiver(Telnyx::ZMQ::ZMQSocket::PULL);
    bool bound = receiver.bind("tcp://127.0.0.1:53558");
    TEST_CHECK(bound, "PULL should bind to a local URL");
    if (!receiver.socket()) {
        TEST_CHECK(false, "underlying zmq::socket_t* unavailable");
        return;
    }

    int rcvtimeo = -2;
    size_t sz = sizeof(rcvtimeo);
    receiver.socket()->getsockopt(ZMQ_RCVTIMEO, &rcvtimeo, &sz);
    std::cout << "  ZMQ_RCVTIMEO = " << rcvtimeo << " ms" << std::endl;

    TEST_CHECK(rcvtimeo != -1,
        "ZMQ_RCVTIMEO must not be the libzmq default (-1 = block forever)");
    TEST_CHECK(rcvtimeo > 0,
        "ZMQ_RCVTIMEO must be a positive finite value");
    TEST_CHECK(rcvtimeo <= 60000,
        "ZMQ_RCVTIMEO should be <= 60s (sanity cap)");
}

// ---------------------------------------------------------------------
// Test 3 — Behavioural: sendRequest() on a PUSH socket connected to an
// unresponsive (binding-but-not-reading) peer must return false in
// bounded time. Without the fix this test hangs; a watchdog kills the
// process with exit 1 so CI catches the regression.
// ---------------------------------------------------------------------
void test_send_does_not_block_on_dead_peer()
{
    std::cout << "[test] sendRequest returns false on unresponsive peer"
              << std::endl;

    const std::string url = "tcp://127.0.0.1:53559";

    Telnyx::ZMQ::ZMQSocket receiver(Telnyx::ZMQ::ZMQSocket::PULL);
    TEST_CHECK(receiver.bind(url), "bind PULL receiver");

    Telnyx::ZMQ::ZMQSocket sender(Telnyx::ZMQ::ZMQSocket::PUSH);
    TEST_CHECK(sender.connect(url), "connect PUSH sender");

    std::atomic<int> failures{0};
    std::atomic<int> successes{0};
    std::atomic<bool> done{false};

    std::thread send_thread([&] {
        std::string payload(1024, 'X');
        while (!done.load(std::memory_order_relaxed)) {
            bool ok = sender.sendRequest(payload);
            if (ok) {
                successes.fetch_add(1, std::memory_order_relaxed);
            } else {
                failures.fetch_add(1, std::memory_order_relaxed);
                done.store(true, std::memory_order_relaxed);
            }
        }
    });

    // Watchdog: give the loop up to 15 s to complete. Without SNDTIMEO
    // it will block forever inside libzmq; with SNDTIMEO it terminates
    // shortly after HWM is reached.
    const int watchdog_secs = 15;
    for (int i = 0; i < watchdog_secs * 10; i++) {
        if (done.load(std::memory_order_relaxed)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!done.load(std::memory_order_relaxed)) {
        TEST_CHECK(false,
            "sendRequest hung past watchdog — wrapper missing SNDTIMEO");
        std::cerr << "  send_thread is stuck inside libzmq; forcing exit."
                  << std::endl;
        send_thread.detach();
        std::cout << "Results so far: " << pass_count.load() << " passed, "
                  << fail_count.load() << " failed" << std::endl;
        std::_Exit(1);
    }

    send_thread.join();

    std::cout << "  successes=" << successes.load()
              << "  failures="  << failures.load() << std::endl;
    TEST_CHECK(failures.load() > 0,
        "must observe at least one send failure within the watchdog window");
    TEST_CHECK(successes.load() > 0,
        "some sends should succeed before the timeout fires (peer is reachable)");
}

}  // namespace

int main()
{
    std::cout << "==============================================================\n"
                 "Telnyx::ZMQ::ZMQSocket — timeout regression test\n"
                 "==============================================================" << std::endl;

    test_push_socket_has_finite_send_timeout();
    test_pull_socket_has_finite_recv_timeout();
    test_send_does_not_block_on_dead_peer();

    std::cout << "--------------------------------------------------------------\n"
              << "Results: " << pass_count.load() << " passed, "
              << fail_count.load() << " failed" << std::endl;
    return fail_count.load() > 0 ? 1 : 0;
}
