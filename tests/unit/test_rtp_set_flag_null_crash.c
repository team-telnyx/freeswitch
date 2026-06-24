/*
 * Reproducer for TELCORE / mod_call_recovery NULL-deref crash:
 *
 *   #0 switch_rtp_set_flag()                  src/switch_rtp.c:6373
 *   #1 switch_core_media_activate_rtp()       src/switch_core_media.c:~11552
 *   #2 switch_core_media_recover_session()    src/switch_core_media.c:17383
 *   #3 call_recovery::recover()               mod_call_recovery/call_recovery.cpp:752
 *
 * Root cause: switch_core_media_activate_rtp calls switch_rtp_set_flag()
 * unguarded, immediately after switch_rtp_new(). When switch_rtp_new() fails
 * (returns NULL) — typical during recovery: bind EADDRINUSE on the recovered
 * port, EADDRNOTAVAIL on a stale local IP, or empty/missing remote_sdp_ip in
 * the persisted XML — switch_rtp_set_flag dereferences NULL at line 6373
 * (`int old_flag = rtp_session->flags[flag];`).
 *
 * The unguarded call sites:
 *   src/switch_core_media.c
 *     - 11527  switch_rtp_set_flag(... SWITCH_RTP_FLAG_AUTOADJ)
 *               under the "telnyx_voicemail" channel variable
 *     - 11548  switch_rtp_set_flag(... SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF)
 *               under the "ignore_rtp_during_dtmf" channel variable
 *
 * Both lack the `if (switch_rtp_ready(a_engine->rtp_session))` guard that
 * every neighbouring RTP call in the same function uses (cf. lines 11497,
 * 11565, 11570).
 *
 * This test isolates each scenario in a forked child so the SIGSEGV does
 * not abort the rest of the suite. After the fix, every child must exit
 * cleanly (status 0). Pre-fix, the children that exercise the unguarded
 * paths exit with WIFSIGNALED / SIGSEGV.
 *
 * Child exit-code contract (see ASSERT_CHILD_OK()):
 *   0            success: no crash, and — for the bind-conflict child — the
 *                NULL/bind-error precondition was actually reproduced before
 *                the guarded call was made.
 *   2            harness setup failure (socket/bind/getsockname) — an
 *                environment problem, not a pass.
 *   3            precondition NOT met: switch_rtp_new() returned non-NULL, so
 *                the recovery NULL path was never exercised — fail loudly
 *                rather than report a meaningless pass.
 *   4            precondition NOT met: switch_rtp_new() failed for a reason
 *                other than the bind conflict we engineered.
 *   SIGSEGV/BUS  the regression itself — the NULL guard is missing.
 */

#include <switch.h>
#include <test/switch_test.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/*
 * run_in_child: fork, run `body` in the child, and report the child's wait
 * status via *out_status. Returns 1 on success (child reaped cleanly,
 * *out_status valid) or 0 on a harness failure (fork or waitpid). On harness
 * failure *out_status is meaningless and the caller MUST treat it as an error,
 * never a pass — otherwise an interrupted/failed waitpid() with a zeroed status
 * would masquerade as a clean child exit and turn a crash test into a false
 * pass. waitpid() is retried on EINTR; any other failure is reported.
 */
static int run_in_child(void (*body)(void), int *out_status)
{
	pid_t pid;
	int status = 0;
	int rc;

	*out_status = 0;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "run_in_child: fork failed: %s\n", strerror(errno));
		return 0;
	}
	if (pid == 0) {
		body();
		_exit(0);
	}

	do {
		rc = waitpid(pid, &status, 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0) {
		fprintf(stderr, "run_in_child: waitpid failed: %s\n", strerror(errno));
		return 0;
	}

	*out_status = status;
	return 1;
}

static int child_segfaulted(int status)
{
	return WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

/*
 * Run `scenario` in a forked child and translate the result into fst
 * assertions, decoding the exit-code contract documented at the top of the
 * file so a CI failure says *why* (harness failure vs. crash vs. unmet
 * precondition vs. setup), not just "exited nonzero". A harness failure
 * (fork/waitpid) is a hard fail — never silently treated as a clean exit.
 *
 * A macro rather than a function: fst_xcheck/fst_fail expand to fct_* checks
 * that reference the enclosing FST_TEST's context, so they must be used inside
 * a test block.
 */
#define ASSERT_CHILD_OK(scenario) \
	do { \
		int _st = 0; \
		if (!run_in_child((scenario), &_st)) { \
			fst_fail("fork/waitpid harness failure — see stderr"); \
		} else { \
			fst_xcheck(!child_segfaulted(_st), \
			           "guarded NULL switch_rtp_set_flag/clear_flag path must not segfault"); \
			if (!WIFEXITED(_st)) { \
				fst_fail("child did not exit normally"); \
			} else { \
				fst_xcheck(WEXITSTATUS(_st) != 2, \
				           "bind-conflict harness setup failed (socket/bind/getsockname)"); \
				fst_xcheck(WEXITSTATUS(_st) != 3, \
				           "switch_rtp_new() unexpectedly returned non-NULL — recovery NULL path NOT exercised"); \
				fst_xcheck(WEXITSTATUS(_st) != 4, \
				           "switch_rtp_new() failed for a non-bind reason — bind conflict not reproduced"); \
				fst_xcheck(WEXITSTATUS(_st) == 0, "child exited non-zero"); \
			} \
		} \
	} while (0)

/*
 * Scenario 1 — the proximate bug, isolated.
 *
 * Reproduces the exact instruction that crashed in the trace:
 *   switch_rtp.c:6373:  int old_flag = rtp_session->flags[flag];
 *
 * If switch_rtp_set_flag() ever defends against NULL (or callers do), this
 * passes. With current code on master it segfaults.
 */
static void scenario_direct_null_set_flag(void)
{
	switch_rtp_set_flag(NULL, SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF);
}

/*
 * Scenario 1b — the clear_flag side of the same guard.
 *
 * switch_core_media_activate_rtp() / call recovery can also reach
 * switch_rtp_clear_flag() with a NULL session, and the production fix guards
 * both set and clear. Cover the clear path directly too, so a regression in
 * either guard is caught.
 */
static void scenario_direct_null_clear_flag(void)
{
	switch_rtp_clear_flag(NULL, SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF);
}

/*
 * Scenario 2 — realistic upstream failure: switch_rtp_new() fails because
 * the local rx_port is already bound (simulates a recovery-time port
 * collision, the most common cause of the NULL return on this path).
 *
 * Mirrors the activate_rtp pattern verbatim:
 *   a_engine->rtp_session = switch_rtp_new(...);
 *   // (no switch_rtp_ready guard — bug)
 *   switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF);
 *
 * The bind conflict is the whole point of this scenario, so it is enforced as
 * a contract: if switch_rtp_new() does NOT return NULL (or fails for some
 * unrelated reason), the child exits non-zero and the test fails — a "pass"
 * here would otherwise be meaningless, as it would no longer touch the NULL
 * recovery path at all.
 */
static void scenario_bind_conflict_then_set_flag(void)
{
	switch_memory_pool_t *pool = NULL;
	switch_rtp_t *rtp_session = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
	const char *err = NULL;
	int hog;
	struct sockaddr_in sa;
	socklen_t sa_len = sizeof(sa);
	unsigned short busy_port;
	switch_port_t remote_port;

	switch_core_new_memory_pool(&pool);

	/* Hold an OS-assigned rx port so switch_rtp_set_local_address() bind()
	 * will EADDRINUSE. Bind to port 0 and read back the actual port via
	 * getsockname() so the test never relies on a fixed port being free. */
	hog = socket(AF_INET, SOCK_DGRAM, 0);
	if (hog < 0) {
		fprintf(stderr, "scenario_bind_conflict: socket() failed: %s\n", strerror(errno));
		_exit(2);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(0);
	sa.sin_addr.s_addr = inet_addr("127.0.0.1");
	if (bind(hog, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "scenario_bind_conflict: bind() failed: %s\n", strerror(errno));
		_exit(2);
	}
	if (getsockname(hog, (struct sockaddr *)&sa, &sa_len) < 0) {
		fprintf(stderr, "scenario_bind_conflict: getsockname() failed: %s\n", strerror(errno));
		_exit(2);
	}
	busy_port = ntohs(sa.sin_port);

	/* Pick a nonzero remote tx_port without uint16_t wraparound: tx_port is a
	 * switch_port_t (uint16_t), so busy_port + 2 would narrow to 0 when the OS
	 * assigns busy_port == 65534, tripping switch_rtp_new()'s "Missing remote
	 * port" check before the intended local bind conflict. The remote port is
	 * irrelevant to the bind conflict; it only must stay nonzero. */
	remote_port = busy_port >= 65534 ? (switch_port_t)(busy_port - 2) : (switch_port_t)(busy_port + 2);

	rtp_session = switch_rtp_new("127.0.0.1", busy_port,
	                             "127.0.0.1", remote_port,
	                             /*payload*/ 8, /*samples*/ 8000,
	                             /*ms*/ 20 * 1000, flags, "soft", &err, pool);

	fprintf(stderr, "scenario_bind_conflict: switch_rtp_new -> %p, err=%s\n",
	        (void *)rtp_session, err ? err : "(none)");

	/* Contract: the bind conflict MUST drive switch_rtp_new() to fail and
	 * return NULL. If it did not, this child is not exercising the recovery
	 * crash path — fail loudly (exit 3) rather than report a hollow pass. */
	if (rtp_session != NULL) {
		switch_rtp_destroy(&rtp_session);
		close(hog);
		switch_core_destroy_memory_pool(&pool);
		_exit(3);
	}

	/* And it must have failed *because of the bind conflict* (switch_rtp.c
	 * sets "Bind Error! host:port"), not for some unrelated reason that
	 * happens to also return NULL. */
	if (!err || !strstr(err, "Bind Error")) {
		close(hog);
		switch_core_destroy_memory_pool(&pool);
		_exit(4);
	}

	/* The exact buggy pattern from switch_core_media_activate_rtp():
	 * unguarded switch_rtp_set_flag() on the (NULL) session. Pre-fix this
	 * dereferences NULL at switch_rtp.c:6373 and the child takes SIGSEGV;
	 * with the guard in place it must no-op and the child exits 0. */
	switch_rtp_set_flag(rtp_session, SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF);

	close(hog);
	switch_core_destroy_memory_pool(&pool);
}

FST_CORE_BEGIN("./conf")
{
FST_SUITE_BEGIN(rtp_set_flag_null_crash)
{
FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

	FST_TEST_BEGIN(direct_null_set_flag_must_not_segfault)
	{
		ASSERT_CHILD_OK(scenario_direct_null_set_flag);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(direct_null_clear_flag_must_not_segfault)
	{
		ASSERT_CHILD_OK(scenario_direct_null_clear_flag);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(bind_conflict_then_set_flag_must_not_segfault)
	{
		ASSERT_CHILD_OK(scenario_bind_conflict_then_set_flag);
	}
	FST_TEST_END()
}
FST_SUITE_END()
}
FST_CORE_END()
