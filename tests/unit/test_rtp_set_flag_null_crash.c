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

/*
 * run_in_child: fork, run `body` in the child, return the wait status.
 * The child exits 0 on success (no crash) or with whatever signal/exit
 * the body produces.
 */
static int run_in_child(void (*body)(void))
{
	pid_t pid = fork();
	int status = 0;

	if (pid == 0) {
		body();
		_exit(0);
	}
	if (pid < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		return -1;
	}
	waitpid(pid, &status, 0);
	return status;
}

static int child_segfaulted(int status)
{
	return WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

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
 * Scenario 2 — realistic upstream failure: switch_rtp_new() fails because
 * the local rx_port is already bound (simulates a recovery-time port
 * collision, the most common cause of the NULL return on this path).
 *
 * Mirrors the activate_rtp pattern verbatim:
 *   a_engine->rtp_session = switch_rtp_new(...);
 *   // (no switch_rtp_ready guard — bug)
 *   switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF);
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

	rtp_session = switch_rtp_new("127.0.0.1", busy_port,
	                             "127.0.0.1", busy_port + 2,
	                             /*payload*/ 8, /*samples*/ 8000,
	                             /*ms*/ 20 * 1000, flags, "soft", &err, pool);

	/* Expectation: rtp_session == NULL because of the bind conflict.
	 * Do NOT assert here — we want to continue down the buggy path. */
	fprintf(stderr, "scenario_bind_conflict: switch_rtp_new -> %p, err=%s\n",
	        (void *)rtp_session, err ? err : "(none)");

	/* Buggy pattern from switch_core_media.c — unguarded set_flag. */
	switch_rtp_set_flag(rtp_session, SWITCH_RTP_FLAG_IGNORE_RTP_DURING_DTMF);

	/* If we somehow reach here, clean up and report success. */
	if (rtp_session) {
		switch_rtp_destroy(&rtp_session);
	}
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
		int status = run_in_child(scenario_direct_null_set_flag);
		fst_xcheck(!child_segfaulted(status),
		           "switch_rtp_set_flag(NULL, ...) must not segfault");
		fst_check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(bind_conflict_then_set_flag_must_not_segfault)
	{
		int status = run_in_child(scenario_bind_conflict_then_set_flag);
		fst_xcheck(!child_segfaulted(status),
		           "activate_rtp pattern (switch_rtp_new fail + unguarded set_flag) "
		           "must not segfault");
		fst_check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	FST_TEST_END()
}
FST_SUITE_END()
}
FST_CORE_END()
