/*
 * test_event_socket_kill_listener_fd_reuse.c
 * ------------------------------------------
 *
 * Regression test for a runtime double-close / stale-fd bug in mod_event_socket
 * (src/mod/event_handlers/mod_event_socket/mod_event_socket.c).
 *
 * THE DEFECT
 * ----------
 * mod_event_socket has TWO socket teardown paths:
 *
 *   close_socket()  (mod_event_socket.c:668) -- the CORRECT one:
 *       lock sock_mutex; shutdown(); close(); *sock = NULL; unlock;
 *
 *   kill_listener() (mod_event_socket.c:763) -- the BUGGY one:
 *       shutdown(l->sock); close(l->sock);     <-- closes the fd
 *       ... l->sock is NEVER set to NULL, and NO mutex is held ...
 *
 * kill_listener() is invoked AT RUNTIME (not just at shutdown) from threads
 * other than the one that owns the socket, whenever a slow ESL client's queue
 * overflows:
 *   - the logging thread, socket_logger()      -> mod_event_socket.c:280
 *   - the event-dispatch thread, event_handler -> mod_event_socket.c:504
 *
 * The owning listener thread, listener_run(), later runs its own cleanup:
 *       if (listener->sock) {                   <-- mod_event_socket.c:2922
 *           send_disconnect(...);               <-- write to the dead fd (2923)
 *           close_socket(&listener->sock);      <-- mod_event_socket.c:2924
 *       }
 * Because kill_listener() never nulled listener->sock, that `if` is still true,
 * so the SAME fd number is closed a SECOND time. Between the two closes the
 * kernel can recycle that fd number to an unrelated subsystem (e.g. a ZMQ
 * eventfd, an xmlrpc-c or libcurl socket). The second close() then rips the fd
 * out from under its new owner -- the classic "close a file descriptor you no
 * longer own" pattern behind EBADF aborts deep inside unrelated libraries.
 *
 * Unlike an OOM-only path, the trigger here is mundane: an ESL client that
 * can't drain its log/event queue under load.
 *
 * WHAT THIS TEST DOES
 * -------------------
 * It reproduces the exact teardown interleaving deterministically with real
 * POSIX fds (switch_socket_close() ultimately calls ::close() on the fd):
 *   1. a listener owns a connected socket (fd N),
 *   2. kill_listener() runs (queue overflow on another thread): closes fd N,
 *      leaves l->sock untouched -- exactly as the buggy code does,
 *   3. an unrelated subsystem opens an eventfd; the kernel hands back the just
 *      -freed number N, so the eventfd now == N (asserted, so the test only
 *      "confirms" when the recycle actually happened),
 *   4. listener_run()'s cleanup sees l->sock still set and close_socket()s it,
 *      double-closing N -- which now belongs to the eventfd.
 * Then it checks whether the eventfd still works. On the buggy teardown the
 * eventfd has been closed underneath us (write -> EBADF): bug confirmed.
 *
 * Compile-time switch APPLY_FIX selects the proposed fix (kill_listener does
 * shutdown() only and lets the owning listener_run close once); with the fix
 * the recycle never happens and the eventfd survives.
 *
 * No FreeSWITCH/APR linkage and no sanitizer needed: the double-close is a
 * deterministic logic defect detectable via EBADF.
 */

#include <sys/socket.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirrors listener_t: the real struct holds a switch_socket_t*, whose close
 * path calls ::close() on the underlying fd. We model that fd directly. */
typedef struct { int sock; } listener_t;

/* close_socket(): mod_event_socket.c:668 -- the correct single-owner teardown
 * (nulls the handle). Run by listener_run() at line 2924. */
static void close_socket(int *sock)
{
	if (*sock != -1) {
		shutdown(*sock, SHUT_RDWR);
		close(*sock);
		*sock = -1;
	}
}

/* kill_listener(): mod_event_socket.c:763. Called at runtime from the logging
 * thread (280) and event-dispatch thread (504) on queue overflow. */
static void kill_listener(listener_t *l, int apply_fix)
{
	if (l->sock != -1) {
		shutdown(l->sock, SHUT_RDWR);          /* mod_event_socket.c:772 -- wakes the poll/recv */
		if (!apply_fix) {
			close(l->sock);                    /* mod_event_socket.c:773 -- the offending close */
			/* BUG: real code never does l->sock = NULL here
			 * (contrast close_socket() line 674). */
		}
		/* PROPOSED FIX: shutdown() only. Leave the close to the owning
		 * listener_run() via close_socket(); shutdown() wakes the thread
		 * without freeing the fd number, so it cannot be recycled. */
	}
}

/* Returns 1 if an unrelated subsystem's fd survived the listener teardown,
 * 0 if it was double-closed out from under its owner, -1 if inconclusive
 * (the fd number was not actually recycled, so the bug wasn't exercised). */
static int run_scenario(int apply_fix)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return -1; }

	listener_t l;
	l.sock = sv[0];          /* the accepted ESL client connection */
	int peer = sv[1];        /* the far end, kept open */
	int listener_fd_num = l.sock;

	/* Slow client: its log/event queue overflowed -> another thread kills it. */
	kill_listener(&l, apply_fix);

	/* An unrelated subsystem (model: a ZMQ eventfd) opens an fd. In the buggy
	 * run the listener fd was just freed, so the kernel returns its number. */
	int victim = eventfd(0, EFD_NONBLOCK);
	if (victim < 0) { perror("eventfd"); close(peer); close_socket(&l.sock); return -1; }

	int recycled = (victim == listener_fd_num);

	/* listener_run() cleanup (mod_event_socket.c:2922-2924): l->sock was never
	 * nulled by kill_listener in the buggy build, so this closes it again. */
	close_socket(&l.sock);

	/* Did the unrelated subsystem's fd survive? */
	uint64_t one = 1;
	ssize_t w = write(victim, &one, sizeof(one));
	int alive = (w == (ssize_t)sizeof(one));
	int werr = errno;

	/* cleanup */
	if (alive) close(victim);
	close(peer);

	if (!apply_fix && !recycled) {
		/* The kernel didn't hand the number back this run; we can't claim
		 * either way. (In practice this is rare for a minimal fd table.) */
		return -1;
	}
	if (!alive)
		printf("    victim eventfd write failed: %s (fd %d was closed underneath us)\n",
		       strerror(werr), victim);
	return alive ? 1 : 0;
}

int main(void)
{
	int rc = 0;

	printf("[buggy ] kill_listener closes fd without nulling l->sock:\n");
	int buggy = run_scenario(/*apply_fix=*/0);
	if (buggy == 0) {
		printf("[buggy ] CONFIRMED: unrelated subsystem's fd was double-closed (recycled-fd corruption)\n");
	} else if (buggy == 1) {
		printf("[buggy ] FAIL: expected double-close corruption but victim survived\n");
		rc = 1;
	} else {
		printf("[buggy ] INCONCLUSIVE: fd number was not recycled this run\n");
		rc = 2;
	}

	printf("[fixed ] kill_listener does shutdown() only, listener_run owns the close:\n");
	int fixed = run_scenario(/*apply_fix=*/1);
	if (fixed == 1) {
		printf("[fixed ] PASS: unrelated subsystem's fd untouched (single-owner close)\n");
	} else {
		printf("[fixed ] FAIL: fixed path still corrupted the victim fd (rc=%d)\n", fixed);
		rc = 1;
	}

	if (rc == 0)
		printf("\nRESULT: bug confirmed on current code, fix validated.\n");
	return rc;
}
