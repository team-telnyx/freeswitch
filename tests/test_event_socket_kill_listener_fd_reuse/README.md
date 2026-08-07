# mod_event_socket `kill_listener()` fd-reuse reproducer (TELCORE-236)

Reproduces and verifies the fix for the file-descriptor double-close / stale-fd
bug in `kill_listener()` (`src/mod/event_handlers/mod_event_socket/mod_event_socket.c`).

## The bug

`kill_listener()` (mod_event_socket.c:763) runs at runtime on the **logging**
(`socket_logger`, :280) and **event-dispatch** (`event_handler`, :504) threads
when a slow ESL client's queue overflows. It `close()`s `l->sock` **without
nulling it and without the socket mutex**. The owning listener thread
(`listener_run`) still has `listener->sock` set, so at teardown it closes the
**same fd a second time** via `close_socket()` (:2924 → :673). Between the two
closes the kernel can recycle the fd number to another subsystem (a libzmq
eventfd, a libcurl/c-ares socket, xmlrpc-c, ...), so the second close — or the
listener loop's `recv`/`send` on the stale fd — corrupts an fd it no longer owns.
The crash then surfaces far away, inside that unrelated library.

The fix: `kill_listener()` only `shutdown()`s the socket (to wake the listener's
`poll`/`recv`); the single, mutex-guarded, NULL-setting `close()` stays in
`close_socket()`, owned by `listener_run`. `shutdown()` does not free the fd
number, so it cannot be recycled.

## Files

| File | What it does |
|------|--------------|
| `test_event_socket_kill_listener_fd_reuse.c` | Single-threaded, **deterministic**. Mirrors the teardown logic, forces the fd recycle, and detects corruption via `EBADF` on the recycled fd. Exits 0 = bug confirmed / fix validated. |
| `eslfdrace.c` | **Multithreaded**, models the real listener/killer/victim thread split. Shaped so the fdsentry bpftrace detector fires. `./eslfdrace` = buggy, `./eslfdrace fix` = control. |
| `run_under_fdtrace.sh` | Runs `eslfdrace` under fdsentry's `fdtrace-comm.bt` (needs root). |

Both `.c` files model the bug with real POSIX fds (a `socket()`/`accept()`-style
listener fd vs. a recycled `eventfd`); they do not link FreeSWITCH/APR. The fd
double-close/recycle is a logic defect detectable without a sanitizer — and in
fact ASan and `valgrind --track-fds` are both blind to it (they do not track fd
lifecycles), which is why it evaded detection in production.

## Build & run

```sh
cc -std=c11 -g -O0          test_event_socket_kill_listener_fd_reuse.c -o test_killfd
cc      -g -O0 -pthread     eslfdrace.c                                 -o eslfdrace

./test_killfd     # expect: [buggy] CONFIRMED ... / [fixed] PASS ... ; exit 0
./eslfdrace       # expect: [BUGGY ] victim eventfd DEAD
./eslfdrace fix   # expect: [FIXED ] victim fd ALIVE
```

100% reproducible: stress-tested 200× each — buggy always corrupts the victim fd,
the `fix` control never does.

## Kernel-level confirmation (optional, needs root)

`strace` shows the literal double-close:

```
close(3) = 0          # kill_listener (mod_event_socket.c:773)
close(3) = -1 EBADF   # listener_run -> close_socket           (:2924 -> :673)
```

fdsentry's bpftrace detector (TELCORE-49, `fdtrace-comm.bt`) flags both halves —
`[STALE-CLOSE]` at the listener's recycled-fd close and `[USE-AFTER-CLOSE]` at the
victim subsystem's next op:

```sh
sudo bash run_under_fdtrace.sh        # buggy  -> [STALE-CLOSE] + [USE-AFTER-CLOSE]
sudo bash run_under_fdtrace.sh fix    # fixed  -> detector silent
```
