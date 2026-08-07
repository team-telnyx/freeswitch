/*
 * eslfdrace.c — multithreaded reproducer of the mod_event_socket kill_listener
 * fd-reuse bug, shaped so fdsentry's bpftrace detector (fdtrace-comm.bt) fires.
 *
 * Models the real cross-thread teardown:
 *   - listener thread (listener_run)  : owns the accepted socket; later closes
 *                                       it via close_socket() at cleanup (2924)
 *   - killer thread  (socket_logger / event_handler on queue overflow)
 *                                     : calls kill_listener() (763) which
 *                                       close()s the fd but never nulls l->sock
 *   - victim subsystem (e.g. a ZMQ eventfd): opens an fd between the two closes
 *                                       and the kernel hands back the freed
 *                                       number.
 *
 * Forced interleaving (condvar state machine) so the recycle is deterministic:
 *   L: socketpair -> l.sock (fd N)            [gen N = 1, L has seen gen 1]
 *   K: kill_listener -> close(N)              [gen N -> 2]   (l.sock NOT nulled)
 *   V: eventfd() -> reuses N                  [gen N -> 3]   (V has seen gen 3)
 *   L: close_socket(&l.sock) -> close(N)      L last saw gen 1, now gen 3
 *                                             => bpftrace [STALE-CLOSE] gap=2,
 *                                             closing the victim's live fd.
 *
 * That close SUCCEEDS at the syscall level (the number is live), so it is the
 * SILENT corruption ASan and valgrind --track-fds both miss; the .bt detector
 * catches it via the per-thread generation gap. The victim eventfd is left
 * destroyed (write -> EBADF), which we also assert directly.
 *
 * With APPLY_FIX=1 (kill_listener does shutdown() only) the fd is never freed
 * between the two points, no recycle happens, the lone close is single-owner,
 * and the detector stays silent.
 *
 *   build: cc -O0 -g -pthread eslfdrace.c -o eslfdrace
 *   run  : in one shell  ->  sudo bpftrace fdtrace-comm.bt eslfdrace
 *          in another    ->  ./eslfdrace            (buggy)
 *                            ./eslfdrace fix        (fixed - detector silent)
 */
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

typedef struct { int sock; } listener_t;

static int apply_fix = 0;

/* shared state machine */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
enum { INIT = 0, CREATED, KILLED, REUSED };
static int state = INIT;
static listener_t L;
static int victim = -1;

static void set_state(int s) {
	pthread_mutex_lock(&mtx);
	state = s;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mtx);
}
static void wait_state(int s) {
	pthread_mutex_lock(&mtx);
	while (state < s) pthread_cond_wait(&cv, &mtx);
	pthread_mutex_unlock(&mtx);
}

/* close_socket(): mod_event_socket.c:668 — correct single-owner teardown. */
static void close_socket(int *sock) {
	if (*sock != -1) { shutdown(*sock, SHUT_RDWR); close(*sock); *sock = -1; }
}

/* kill_listener(): mod_event_socket.c:763 — runs on a non-owning thread. */
static void kill_listener(listener_t *l) {
	if (l->sock != -1) {
		shutdown(l->sock, SHUT_RDWR);          /* :772 */
		if (!apply_fix) {
			close(l->sock);                    /* :773 — frees the fd number */
			/* BUG: l->sock not nulled (contrast close_socket :674) */
		}
		/* FIX: shutdown only; owner closes once via close_socket(). */
	}
}

static void *listener_thread(void *arg) {     /* listener_run() */
	(void)arg;
	/* Real code's fd comes from accept(); use socket() here — both are tracked
	 * by fdtrace-comm.bt (sys_exit_socket/accept), so the listener thread is
	 * recorded as the fd's opener (seen-gen = 1). socketpair() would NOT be
	 * tracked and the [STALE-CLOSE] generation check would never engage. */
	L.sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (L.sock < 0) { perror("socket"); exit(2); }
	set_state(CREATED);

	wait_state(REUSED);
	/* listener_run cleanup, mod_event_socket.c:2922-2924: l->sock still set */
	close_socket(&L.sock);                      /* closes the RECYCLED fd */
	return NULL;
}

static void *killer_thread(void *arg) {        /* socket_logger / event_handler */
	(void)arg;
	wait_state(CREATED);
	kill_listener(&L);                          /* queue overflow -> kill */
	set_state(KILLED);
	return NULL;
}

int main(int argc, char **argv) {
	apply_fix = (argc > 1 && strcmp(argv[1], "fix") == 0);
	printf("eslfdrace pid=%d mode=%s — run `sudo bpftrace fdtrace-comm.bt eslfdrace`\n",
	       getpid(), apply_fix ? "FIXED" : "BUGGY");

	pthread_t lt, kt;
	pthread_create(&lt, NULL, listener_thread, NULL);
	pthread_create(&kt, NULL, killer_thread, NULL);

	/* victim subsystem grabs an fd after the kill, before the owner's close */
	wait_state(KILLED);
	victim = eventfd(0, EFD_NONBLOCK);          /* models a ZMQ eventfd */
	if (victim < 0) { perror("eventfd"); return 2; }
	int recycled = (victim == L.sock);
	set_state(REUSED);

	pthread_join(lt, NULL);
	pthread_join(kt, NULL);

	uint64_t one = 1;
	ssize_t w = write(victim, &one, sizeof(one));
	int alive = (w == (ssize_t)sizeof(one));
	int werr = errno;
	if (alive) close(victim);

	if (apply_fix) {
		printf("[FIXED ] victim fd %s (expected alive)\n", alive ? "ALIVE" : "DEAD");
		return alive ? 0 : 1;
	}
	if (!recycled) { printf("[BUGGY ] INCONCLUSIVE: fd not recycled this run\n"); return 3; }
	printf("[BUGGY ] victim eventfd %s%s\n", alive ? "ALIVE (unexpected)" : "DEAD",
	       alive ? "" : strerror(werr) ? " — write EBADF, double-closed by recycle" : "");
	return alive ? 1 : 0;
}
