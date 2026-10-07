#!/usr/bin/env python3
"""
Integration check for freeswitch.batch against a running FreeSWITCH.

Not part of `make check`: it needs a live mod_xml_rpc endpoint. Point it at
one with FS_XMLRPC_URL, including credentials, e.g.

    FS_XMLRPC_URL=http://user:pass@127.0.0.1:8080/RPC2 ./test_batch_xmlrpc.py

Every case prints PASS/FAIL; the exit status is 0 only if all cases pass.
The global variable named by MARKER is used to prove which commands ran.
"""

import os
import sys
import xmlrpc.client

URL = os.environ.get("FS_XMLRPC_URL")
MARKER = "batch_xmlrpc_test_marker"
BAD = "echonotexisting"
NOT_COMPLETED = "-ERR: Not Completed"
TYPE_ERROR = -501
INDEX_ERROR = -502

failures = 0
passes = 0


def check(cond, name, detail=""):
    global failures, passes
    if cond:
        passes += 1
        print("  PASS: %s" % name)
    else:
        failures += 1
        print("  FAIL: %s %s" % (name, detail))


def proxy():
    return xmlrpc.client.ServerProxy(URL, allow_none=False)


def api(command, arg=""):
    return proxy().freeswitch.api(command, arg)


def batch(*params, method="freeswitch.batch"):
    obj = proxy()
    for part in method.split("."):
        obj = getattr(obj, part)
    return obj(*params)


def strip(results):
    return [r.rstrip("\n") for r in results]


def set_marker(value):
    api("global_setvar", "%s=%s" % (MARKER, value))


def marker():
    return api("global_getvar", MARKER).strip()


def set_marker_cmd(value):
    return ["global_setvar", "%s=%s" % (MARKER, value)]


def expect_result(name, params, expected, method="freeswitch.batch"):
    try:
        got = strip(batch(*params, method=method))
    except xmlrpc.client.Fault as e:
        check(False, name, "(fault %d: %s)" % (e.faultCode, e.faultString))
        return
    check(got == expected, name, "(got %r, expected %r)" % (got, expected))


def fault_matches(fault, code, text):
    return fault.faultCode == code and text in fault.faultString


def expect_fault_nothing_ran(name, params, code, text):
    set_marker("before")
    try:
        got = batch(*params)
        check(False, name, "(no fault, got %r)" % (got,))
        return
    except xmlrpc.client.Fault as e:
        check(fault_matches(e, code, text), name + ": fault code and reason",
              "(fault %d: %s; expected %d containing %r)" % (e.faultCode, e.faultString, code, text))
    check(marker() == "before", name + ": nothing ran", "(marker=%r)" % marker())


def test_default_stops_at_first_failure():
    print("[default_stops_at_first_failure]")
    cmds = [["echo", "1"], [BAD, "2"], ["echo", "3"]]
    expect_result("(a) failure stops the batch", [cmds], ["1", "ERROR!", NOT_COMPLETED])
    expect_result("(a) freeswitch_batch alias", [cmds], ["1", "ERROR!", NOT_COMPLETED], method="freeswitch_batch")

    set_marker("before")
    expect_result("default: skipped command reports Not Completed",
                  [[[BAD, "1"], set_marker_cmd("after")]], ["ERROR!", NOT_COMPLETED])
    check(marker() == "before", "default: skipped command did not run", "(marker=%r)" % marker())


def test_run_all():
    print("[run_all]")
    cmds = [["echo", "1"], [BAD, "2"], ["echo", "3"]]
    expect_result("(d) run_all=true runs every command", [cmds, True], ["1", "ERROR!", "3"])
    expect_result("(d) run_all on freeswitch_batch alias", [cmds, True], ["1", "ERROR!", "3"], method="freeswitch_batch")
    expect_result("(e) run_all=false matches the default", [cmds, False], ["1", "ERROR!", NOT_COMPLETED])
    expect_result("run_all=true with no failures", [[["echo", "1"], ["echo", "2"]], True], ["1", "2"])
    expect_result("run_all=true with consecutive failures", [[[BAD, "1"], [BAD, "2"], ["echo", "3"]], True],
                  ["ERROR!", "ERROR!", "3"])

    set_marker("before")
    expect_result("run_all: command after a failure runs",
                  [[[BAD, "1"], set_marker_cmd("after")], True], ["ERROR!", "+OK"])
    check(marker() == "after", "run_all: marker set by the command after the failure", "(marker=%r)" % marker())


def test_continue_on_fail():
    print("[continue_on_fail]")
    expect_result("(b) marked failure continues",
                  [[["echo", "1"], [BAD, "2", True], ["echo", "3"]]], ["1", "ERROR!", "3"])
    expect_result("(c) later unmarked failure still stops",
                  [[["echo", "1"], [BAD, "2", True], [BAD, "3"], ["echo", "4"]]],
                  ["1", "ERROR!", "ERROR!", NOT_COMPLETED])
    expect_result("(b) freeswitch_batch alias",
                  [[["echo", "1"], [BAD, "2", True], ["echo", "3"]]], ["1", "ERROR!", "3"], method="freeswitch_batch")
    expect_result("consecutive marked failures continue",
                  [[[BAD, "1", True], [BAD, "2", True], ["echo", "3"]]], ["ERROR!", "ERROR!", "3"])
    expect_result("false flag stops like an unmarked command",
                  [[["echo", "1"], [BAD, "2", False], ["echo", "3"]]], ["1", "ERROR!", NOT_COMPLETED])
    expect_result("marked successful command",
                  [[["echo", "1", True], ["echo", "2"]]], ["1", "2"])
    expect_result("run_all overrides a false flag",
                  [[["echo", "1"], [BAD, "2", False], ["echo", "3"]], True], ["1", "ERROR!", "3"])
    expect_result("both flags",
                  [[["echo", "1"], [BAD, "2", True], [BAD, "3"], ["echo", "4"]], True],
                  ["1", "ERROR!", "ERROR!", "4"])

    set_marker("before")
    expect_result("marked failure: next command runs",
                  [[[BAD, "1", True], set_marker_cmd("after")]], ["ERROR!", "+OK"])
    check(marker() == "after", "marked failure: marker set by the next command", "(marker=%r)" % marker())


def test_malformed_commands():
    print("[malformed_commands]")
    expect_fault_prefix_ran("non-boolean flag", [["echo", "x", "yes"]], TYPE_ERROR, "type BOOL was expected")
    expect_fault_prefix_ran("integer flag", [["echo", "x", 1]], TYPE_ERROR, "type BOOL was expected")
    expect_fault_prefix_ran("non-string command in a marked item", [[1, "x", True]], TYPE_ERROR,
                            "string type was expected")
    expect_fault_prefix_ran("four items", [["echo", "x", True, True]], INDEX_ERROR, "requests exactly 2 items")
    expect_fault_prefix_ran("four items under run_all", [["echo", "x", True, True]], INDEX_ERROR,
                            "requests exactly 2 items", run_all=True)

    expect_result("malformed item in a skipped suffix is not parsed",
                  [[[BAD, "1"], ["echo", "x", "yes"], ["echo", "y", True, True]]],
                  ["ERROR!", NOT_COMPLETED, NOT_COMPLETED])


def expect_fault_prefix_ran(name, bad_items, code, text, run_all=False):
    set_marker("before")
    params = [[set_marker_cmd("first")] + bad_items + [set_marker_cmd("last")]]
    if run_all:
        params.append(True)
    try:
        got = batch(*params)
        check(False, name, "(no fault, got %r)" % (got,))
        return
    except xmlrpc.client.Fault as e:
        check(fault_matches(e, code, text), name + ": fault code and reason",
              "(fault %d: %s; expected %d containing %r)" % (e.faultCode, e.faultString, code, text))
    check(marker() == "first", name + ": earlier command ran, later did not", "(marker=%r)" % marker())


def test_empty_batch():
    print("[empty_batch]")
    expect_result("empty batch", [[]], [])
    expect_result("empty batch with run_all", [[], True], [])


def test_invalid_parameters():
    print("[invalid_parameters]")
    cmds = [set_marker_cmd("after")]
    expect_fault_nothing_ran("no parameters", [], INDEX_ERROR, "Expected 1 or 2 parameters")
    expect_fault_nothing_ran("three parameters", [cmds, True, True], INDEX_ERROR, "Expected 1 or 2 parameters")
    expect_fault_nothing_ran("run_all as string", [cmds, "true"], TYPE_ERROR, "type BOOL was expected")
    expect_fault_nothing_ran("run_all as integer", [cmds, 1], TYPE_ERROR, "type BOOL was expected")
    expect_fault_nothing_ran("commands not an array", ["echo"], TYPE_ERROR, "requires type ARRAY")


def test_direct_api_unchanged():
    print("[direct_api_unchanged]")
    check(api("echo", "1").rstrip("\n") == "1", "freeswitch.api pair works")
    try:
        proxy().freeswitch.api("echo", "1", True)
        check(False, "freeswitch.api rejects a third parameter", "(no fault)")
    except xmlrpc.client.Fault as e:
        check(fault_matches(e, INDEX_ERROR, "requests exactly 2 items"), "freeswitch.api rejects a third parameter",
              "(fault %d: %s)" % (e.faultCode, e.faultString))


def main():
    if not URL:
        print("FS_XMLRPC_URL is not set", file=sys.stderr)
        return 2

    for test in (test_default_stops_at_first_failure, test_run_all, test_continue_on_fail,
                 test_malformed_commands, test_empty_batch,
                 test_invalid_parameters, test_direct_api_unchanged):
        test()

    print("\n=== Results: %d passed, %d failed ===" % (passes, failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
