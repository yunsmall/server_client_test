# server_client_test

A TCP server/client library based on **standalone asio** (not the Boost version), written in C++23, demonstrating the correct way to write a "synchronous I/O + multithreaded network server".

中文版：[README.md](README.md)

## Features

| Feature | Description |
|---|---|
| Multiple clients | Each connection gets an independent Session |
| Synchronous API | `read_until` / `write`, no async callbacks |
| Coroutine accept | Single acceptor thread runs an `io_context`; `accept_loop` is an asio coroutine |
| Read/write threads | One read thread + one write thread per connection |
| Graceful stop | `stop()` interrupts blocking I/O and BLOCK waits, waits for all sessions to be reaped, idempotent |
| Restartable | `start()` can be called again after `stop()` (with different port/bind address) |
| Interruptible BLOCK | Server blocks for a given number of seconds per command; `stop()` can interrupt it |
| Crash stack printing | Prints its own stack trace on crash/assertion failure (`<stacktrace>`), stress testing needs no debugger |
| Dual platform | Passed stress testing on both Windows (clang + MSVC STL) and Linux (g++-14) |

## Threading model

```
acceptor thread ── io_context.run(): coroutine accept_loop (async_accept, cancellable across threads)
      │
      └── each connection: Session (owns its io_context and socket)
              ├── read thread: read_until parses lines (may block on BLOCK), responses go to a send queue
              └── write thread: pops responses from the queue, sends with synchronous write
```

Thread-safety notes (per asio documentation):

- Synchronous send/receive operations on the same socket are thread-safe with respect to each other, so the read and write threads can safely live in separate threads;
- `shutdown` is thread-safe with respect to send/receive and is used by `stop()` to interrupt blocked I/O; since Windows `shutdown` does not interrupt pending I/O, `cancel` (which posts a cancellation completion packet on IOCP) is combined with it for reliability;
- `close` is the portable cancellation mechanism recommended by the asio docs (it immediately cancels outstanding async operations); the read thread's cleanup uses it to break a blocked write;
- Sessions manage their own lifetime (shared_ptr snapshots); `stop()` waits for all sessions to be destroyed via an `active_sessions_` atomic counter, guaranteeing every Session is reaped before the Server is destroyed.

A more detailed thread-safety discussion: [THREAD_SAFETY.md](THREAD_SAFETY.md)

## Protocol (text lines, `\n` separated)

| Client sends | Server responds | Description |
|---|---|---|
| `PING` | `PONG` | Heartbeat |
| `ECHO <text>` | `<text>` | Echo (empty line if no argument) |
| `ADD <a> <b>` | `<a+b>` | Integer addition (with overflow check) |
| `BLOCK <n>` | `OK` (after n seconds) | Server blocks in the connection's read thread for n seconds; interruptible by `stop()` |
| `QUIT` | none (connection closed) | Disconnect; client perceives it as EOF |
| anything else | `ERROR unknown command` | Unknown command |
| invalid arguments | `ERROR invalid ...` | Argument error |

Command names are split on any whitespace (space/tab). Maximum line length is 64 KB; longer lines disconnect the connection.

## Directory layout

```
include/server_client/
├── protocol.hpp    # protocol constants and command parsing
├── server.hpp      # Server: start/stop/port/active_connections
└── client.hpp      # Client: sync send/receive, timed line reads, disconnect
src/
├── protocol.cpp
├── server.cpp      # coroutine accept_loop + session reaping
├── session.hpp/.cpp # per-connection session (read/write threads + cleanup)
└── client.cpp
test/
├── crash_dump.hpp      # cross-platform crash stack printing (Windows/Linux)
├── test_main.cpp       # crash handler installation + gtest entry
├── test_stress.cpp     # stress cases (concurrency/races/connection lifecycle)
├── test_slow.cpp       # slow cases (BLOCK/timeouts/connect retries)
└── test_protocol.cpp   # protocol cases
stress_test.sh          # parameterized multi-process stress script
```

## Build and test

Dependencies: standalone `asio` and `gtest` installed via vcpkg; a C++23 compiler (clang-cl + MSVC STL on Windows, g++-13+ on Linux).

### Windows

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
./build/server_client_test          # 26 test cases
```

### Linux

```bash
cmake -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-linux
./build-linux/server_client_test
```

On Linux, crash stack printing requires `-lstdc++exp` (linked automatically by CMakeLists on UNIX).

## Stress testing

`server_client_stress_test` is the stress-only binary (fast cases only, no slow cases), run in parallel via `stress_test.sh`:

```bash
# usage: stress_test.sh <executable> <total iterations> <number of processes>
./stress_test.sh ./build/server_client_stress_test.exe 2400 12   # Windows
./stress_test.sh ./build-linux/server_client_stress_test 2400 12 # Linux
```

- Per-process iterations = `ceil(total / processes)`, computed automatically;
- If any process crashes (`!!! CRASH`) or an assertion fails (`FAILED`), all other processes are terminated immediately and the crash site is printed (the process prints its own stack trace, no gdb needed);
- Stress cases run in a loop via `--gtest_repeat` with `--gtest_break_on_failure`;
- Keep the process count within the CPU core count; when the connection storm rate exceeds the OS ephemeral port pool (TIME_WAIT recycling), client connect failures are a system limit.

Passed 2400 iterations on Windows (clang) and 2400 iterations on Linux (g++-14).

## Key implementation notes

- **Coroutine accept**: `accept_loop` is an `asio::awaitable` coroutine; `acceptor_.cancel()` cancels a pending async_accept across threads (the official asio cancellation interface); `io_context_.stop()` covers the window where cancel lands between registrations; after closing the acceptor, `stop()` calls `restart()+poll()` so residual coroutines resume and release their session references before the Server is destroyed (avoids use-after-free).
- **Session reaping**: `stop()` waits for all sessions to finish destruction via the `active_sessions_` atomic counter (incremented on creation, decremented in the destruction callback) — `weak_ptr::expired()` cannot be used (it becomes true as soon as destruction begins, while the destruction callback is still running; returning early would let the callback touch an already-destroyed Server).
- **Write-thread cleanup**: the read thread first waits for the write thread to drain the send queue (`write_done_` flag), then forces `shutdown+close` after a short timeout to break a potentially blocked write (a synchronous write blocks forever when the peer stops reading and the TCP window fills).
- **Crash diagnostics**: `crash_dump.hpp` uses C++23 `<stacktrace>` — `SetUnhandledExceptionFilter` on Windows, `sigaction` (SIGSEGV/SIGABRT, etc.) on Linux — the process prints its own stack trace on crash/assertion failure and exits; stress testing needs no debugger wrapper.

## Quality assurance

Before release, this project went through many rounds of long AI-driven code review: every concurrency/lifetime claim was verified or refuted against the asio source code and official documentation, and every issue found (races, use-after-free, deadlock risks, protocol edge cases) was fixed with justifying comments left in the code. Combined with dual-platform loop stress testing (2400 full iterations on both Windows and Linux) and self-printing crash-stack diagnostics, **no known bugs remain** — used as described in this document and [THREAD_SAFETY.md](THREAD_SAFETY.md), there are no known crash, race, or resource-leak paths.

## License

[MIT](LICENSE)
