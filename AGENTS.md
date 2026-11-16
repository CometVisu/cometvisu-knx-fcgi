# AGENTS.md — Instructions for LLM Coding Agents

This file provides guidelines for AI coding agents working on the
`cometvisu-knx-fcgi` project.

**Roadmap:** [ROADMAP.md](ROADMAP.md) lists the deferred work (the `/f`
endpoint, KNX IP Secure, hardware interop).  The transport layer (phases P0–P8
of the former `PLAN-KNXNETIP.md`) shipped in 2.0.0; this file is the design
record, the git history holds the implementation plan.

**Development container:** `.devcontainer/` provides the full toolchain
(g++, clang, cmake, ninja, clang-format, clang-tidy, gdb, `libfcgi-dev`,
`knxd`/`knxd-dev`, `tcpdump`, `socat`).  Use it for reproducible builds.

## Git and release actions — permission required

**An agent must never commit, push, tag, create or delete branches, open or
modify a pull request, or otherwise change repository state on the
maintainer's behalf.**  Those are the maintainer's decisions and are made case
by case.

- Stop at the working tree: edit files, run builds and tests, then report what
  changed.  `git add`, `git commit` and everything after it belong to the human.
- This also covers sibling repositories (`CometVisu/Docker`) and anything other
  people can see: pushes, PR comments, issue edits, releases.
- Only an explicit instruction in the *current* request ("commit this", "push
  that branch", "open a PR") grants permission, and only for that single
  action.  A task like "implement feature X" or "clean up the docs" does **not**
  include committing the result, and permission never carries over from an
  earlier turn or an earlier session.
- If a commit is needed for the work to make sense (e.g. it spans several
  steps), say so and let the human run the commands.

## Development Methodology

**Test-Driven Development (TDD) is mandatory.** The cycle is:

1. **Write a failing test** — define the expected behavior.
2. **Write the minimum code** to make the test pass.
3. **Refactor** — clean up while keeping tests green.
4. **Repeat.**

Never write implementation code before the corresponding test exists.

## Build & Test Commands

```bash
# Configure (once)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific test
ctest --test-dir build -R test_query_string --output-on-failure

# Run with verbose output
ctest --test-dir build -V
```

## Code Coverage

Every executable line and every branch arm in `src/` must be reached by a
test.  The gate is `tools/check_coverage.py`; `tools/coverage.sh` builds an
instrumented tree, runs the whole suite and then gates.

- `tools/coverage.sh` — ratchet mode (default).  A file listed in
  `tools/coverage-baseline.json` must not drop below its recorded numbers; any
  file **not** listed (i.e. everything new) must be at 100 % lines *and*
  100 % branch arms.
- `tools/coverage.sh --strict` — the target state: 100 % everywhere.  Use it
  to prove a gap is closed.
- `tools/coverage.sh --list-uncovered` — prints the exact line numbers and
  branch arms that are still missing.

Rules:

1. **New code is never added to the baseline.**  Only legacy files are listed
   there.  If a change adds a file, it must be fully covered in the same PR.
2. When a legacy gap is closed, re-record with
   `python3 tools/check_coverage.py --write-baseline tools/coverage-baseline.json`
   — never lower an existing entry by hand.
3. **CI enforces the gate on every pull request** (`.github/workflows/ci.yml`,
   job `coverage`): it builds with `ENABLE_COVERAGE=ON`, runs the suite, runs
   `tools/check_coverage.py` against the committed baseline, writes the
   per-file table into the job summary and uploads the report.  A regression or
   an uncovered new file fails the PR.
4. Use a separate build directory (`build-cov`, the default of
   `tools/coverage.sh`).  `ENABLE_COVERAGE=ON` adds
   `--coverage -O0 -g -fprofile-update=atomic`; the atomic update is required
   because the tests fork workers that write the same `.gcda` files.

Exclusions are acceptable **only** for code that a test process cannot reach:
`fork()` children that `_Exit()` before flushing, signal handlers, and
OS-failure paths that cannot be injected (`socket()`, `mmap()`, `eventfd()`,
a refused multicast join, `poll()` errors on a valid fd, `EINTR`/`EAGAIN` on a
few-byte write).  Mark them inline and say why:

```cpp
if (fd_ < 0) {  // GCOV_EXCL_BR_LINE - only fails on fd exhaustion
  ...
}
```

Supported by the checker: `GCOV_EXCL_LINE`, `GCOV_EXCL_BR_LINE`,
`GCOV_EXCL_START` / `GCOV_EXCL_STOP`.  Exception-unwind arms are ignored.

**A marker must sit on the line it describes.**  `GCOV_EXCL_LINE` and
`GCOV_EXCL_BR_LINE` apply to the line that carries the comment; a comment line
above the code does not exclude anything, so a *statement* marker belongs at the
end of the statement and a *range* marker (`START`/`STOP`) on its own line
before the first and after the last line to skip.

The checker also ignores a line whose entire content is closing braces
(`}`, `});`, `};`).  gcc emits a counter for the exit block of some functions
even though nothing can execute there — such a line carries no logic, so it is
neither counted nor expected to be hit.

**Unreachable-by-construction code should be simplified, not excluded.**
A condition that can never be false (a guard duplicated below its caller, a
`contains()` check after the loop that adds to the set, an `||` arm with an
identical operand) is dead weight: delete it, or turn an exhaustive `switch`
over an enum into an `if` chain so every arm stays reachable.  Exclusions are
the last resort for what the operating system could do but a test cannot.

## Current coverage state

**Everything under `src/` is at 100 % lines and 100 % branches.**
`tools/coverage.sh --strict` is clean, and `tools/coverage-baseline.json` only
exists as a ratchet: every file is recorded at 100 %, so a new gap in *any*
file fails the gate.

The last two legacy files were closed with process-level harnesses rather than
unit tests, because their logic only exists in a running system:

| File | how it is covered |
|---|---|
| `src/fcgi/fcgi_server.cpp` | `tests/unit/test_fcgi_server_run.cpp` drives the real FastCGI record protocol over a Unix socket (a child runs `run()`, the test speaks BEGIN_REQUEST/PARAMS/STDIN and reads STDOUT/END_REQUEST); `tests/e2e/test_spawn_mode_e2e.cpp` covers the spawn-fcgi mode of the built binary. |
| `src/main.cpp` | `tests/e2e/test_main_e2e.cpp` starts the built binary: command line (`-v`/`-h`/`--discover`), configuration validation, and the fork-based supervisor — it kills a worker and the cache reader to force restarts, kills a worker holding long-poll tokens, and shuts the pool down with SIGTERM. |

Coverage builds install a SIGTERM handler in the worker/cache-reader children
(`KNX_COVERAGE` only) so their counters are flushed when the supervisor stops
them; without it every line a child executed would look dead.

## Coding Style

- **C++20** standard.
- Clang-format: Google style, 100 character line limit.
- **Indentation: 2 spaces. Tab width: 8 spaces.** Never use tabs.
- `.clang-format` and `.clang-tidy` are in the repository root.
- Run `clang-format -i <file>` before committing.
- Header guards: `#pragma once` (modern and supported everywhere).
- Namespace: `cvknx` (CometVisu KNX Backend).

## Copyright Header

**Every source file (.cpp, .h, CMakeLists.txt) MUST start with the GPLv3
copyright header.** This is mandatory for all files, now and in the future.

For C++ files (`.cpp`, `.h`):
```cpp
// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
```

For CMake files (`CMakeLists.txt`), use `#` instead of `//`:
```cmake
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: ...
# ...
```

The copyright header must be followed by exactly one blank line before the
actual file content (e.g., `#pragma once` or `#include`).

## File Organization

| Directory        | Purpose                                      |
|------------------|----------------------------------------------|
| `src/`           | Production source code                       |
| `src/bus/`       | Transport seam: `BusLink`, `BusConfig`, `BusFactory`, shared TX link, cache reader loop |
| `src/knxnetip/`  | cEMI codec, KNXnet/IP framing, routing/tunnelling links, discovery |
| `src/fcgi/`      | FastCGI protocol implementation              |
| `src/router/`    | URL routing and handler dispatch             |
| `src/handlers/`  | `/l`, `/r`, `/w` endpoint handlers           |
| `src/knxd/`      | knxd Unix socket client and eibd protocol    |
| `src/state/`     | Session store, shared & local group caches, shared TX queue |
| `src/util/`      | Query string parser, JSON builder, hex utils |
| `tests/unit/`    | Unit tests (mocked dependencies)             |
| `tests/integration/` | Integration tests (real or fake socket)  |
| `tests/e2e/`     | End-to-end tests (full FCGI cycle)           |
| `mocks/`         | Mock/fake implementations for testing        |

## Key Design Decisions

1. **Fork-based worker pool**: The FCGI accept loop uses multiple forked
   worker processes, each running its own `FCGX_Accept_r()` loop on the
   shared listen socket. The OS serializes accept calls across processes.
   This is critical because long-poll `/r` requests block for up to 300
   seconds — without multiple workers, one long-poll would block all other
   clients. Worker count is configurable via `FCGI_WORKERS` (default: 20).
   **The listen socket is mandatory** (`FCGI_SOCKET`, a Unix path or a TCP
   port): the backend opens it itself and the workers inherit it.  The
   classic spawn-fcgi model (FastCGI on stdin/stdout) is deliberately not
   supported — a process that does not own a listen socket can host neither
   the pool nor the cache reader, so it would accept writes into a TX ring
   that nobody drains.  Refusing to start is better than pretending to work.
   Workers never open a bus connection of their own (see decision 2).
   All shared state (SharedGroupCache, SharedTxQueue, SessionStore) lives in
   `mmap(MAP_SHARED|MAP_ANONYMOUS)` memory created **before the first fork**
   and is protected by process-shared robust mutexes.  This is not optional:
   a client logs in once and then sends `s=<id>` with requests that any worker
   may serve, so a session store that is merely a copied member of `main()`
   answers 401 in every worker but one (that was a real bug, verified 15/15
   with `FCGI_WORKERS=4`).
   **The parent supervises the pool and is the only process that reaps
   children** (no SIGCHLD handler that calls `waitpid()` — it would steal the
   exit statuses).  Dead workers and a dead cache reader are restarted with a
   crash-loop backoff (100 ms … 30 s).  The parent therefore keeps the listen
   socket open so replacements inherit it; closing it made every respawned
   worker fall back to spawn-fcgi mode and exit immediately.
   Fork-based workers avoid Docker < 20.10.10 seccomp incompatibility where
   `clone3`/`clone` with process-sharing flags (CLONE_VM, CLONE_THREAD) is
   blocked, preventing `std::thread` creation.

2. **Transports and the single bus owner**: `KNX_TRANSPORT` is **required**
   (`knxd`, `ip-tunnel`, `ip-routing`); there is no default, so a wrong `ip-*`
   setup can never be mistaken for a knxd outage.  Handlers, router and cache
   only ever see the `BusLink` interface (`src/bus/bus_link.h`); the
   `BusFactory` is the only place that knows the implementations.  Never add
   an `if (transport == ...)` outside `main.cpp` / `BusFactory`.
   - **Exactly one process owns the bus connection** — the cache reader — for
     RX *and* TX of every transport.  Workers only push into the shared TX
     queue (`SharedTxQueue`, mmap ring + inherited `eventfd`) via
     `SharedTxBusLink`.  `FCGI_WORKERS` therefore multiplies neither knxd
     client slots (one instead of `FCGI_WORKERS + 1`) nor KNXnet/IP tunnels
     (one instead of one per worker).
   - **knxd** (`KnxdClient`): one read-write group socket, opened by the cache
     reader.  Every `EIB_OPEN_GROUPCON` reply is read with a deadline
     (`set_handshake_timeout_ms()`, default 5 s) — a knxd that accepts the
     connection but never answers must not block the caller.  The client never
     sleeps and never reconnects inside `send_group_packet()` /
     `poll_group_telegram()`: it reports the failure (and drops the socket, so
     `is_connected()` becomes false) and the cache reader — the single
     reconnect authority — retries with backoff.  The 50 ms interval that
     protects knxd's IP tunnel is declared as `min_send_interval_ms()` and
     applied by the reader as a *wait between sends*, so reception is never
     blocked.
   - **Sender validation**: a transport must not trust unicast datagrams.  The
     tunnel socket is `connect()`ed to the configured device (the kernel filters
     by peer) and only frames carrying *our* channel count as liveness; the
     routing socket verifies every datagram's destination address with
     `IP_PKTINFO` against the configured group.
   - **ip-routing** (`RoutingBusLink`): one UDP socket, joined to
     `KNX_IP_MULTICAST`, TTL 16, loop enabled.  Stateless: no ACKs, no
     heartbeat, no outage detection (a disappeared router is undetectable —
     `/r` therefore never invents a `"bus"` outage for this transport).
     **Every participant on the IP backbone acts as a router**, so a write is
     a `ROUTING_INDICATION` (service `0x0530`) carrying `L_Data.ind` (`0x29`)
     with `KNX_IP_INDIVIDUAL_ADDR` as the source (default `15.15.250`) — an
     `L_Data.req` or a `0.0.0` source is silently dropped by knxd and by real
     interfaces.  Our own frame comes back through the multicast loop and is
     the bus echo of the write: it is pushed exactly once, which is what
     announces the value to the other clients.  The outgoing interface is
     selected by its IPv4 address (not its index) so the source address is
     deterministic.
   - **ip-tunnel** (`TunnelingBusLink`): CONNECT / CONNECTIONSTATE /
     DISCONNECT, one sequence counter, every inbound request ACKed,
     out-of-sequence frames ACKed **and discarded** (a duplicate toggle would
     switch twice), retransmission with the *same* sequence on a missing ACK,
     heartbeat every `KNX_IP_HEARTBEAT_SEC` (devices drop a tunnel after 120 s
     of silence).  The heartbeat is sent from `maintain()`, which the cache
     reader calls every loop iteration — it polls the raw fds itself and would
     otherwise never reach the transport's timer.  An unanswered
     `CONNECTIONSTATE_REQUEST` marks the tunnel lost, so `/r` reports the
     outage instead of freezing.
   - **A `TUNNELLING_ACK` is only an ACK when its status is `0x00`.**
     `0x01` ("busy, repeat") is handled like a missing ACK so the normal
     retransmission path repeats the same sequence; `0x02` ("busy, do not
     repeat") drops the telegram and makes `send_group_packet()` return
     `false`.  Treating a NACK as success would lose the write silently while
     `/w` answers 200 — the exact failure mode rule "never claim a success"
     exists for.
   - **KNXnet/IP service codes are fixed by the standard** (03_08_01 5.*):
     tunnelling is `0x0420`/`0x0421`, routing is `0x0530`/`0x0531`/`0x0532`,
     device configuration is `0x0310`/`0x0311`.  Wrong constants compile and
     pass a symmetric unit test, but no real device answers — that is exactly
     what the black-box suites exist for.  A `CONNECT_REQUEST` must carry the
     CRI structure length (`0x04` as `body[size-16]`); a `TUNNELLING_ACK` is
     four bytes (structure length, channel, sequence, status), the request five
     (its reserved byte before the cEMI).
   - **cEMI trap**: only `L_Data.ind` (0x29) with a **group** destination
     reaches the cache.  `L_Data.con` (0x2E) confirms *our own* write and must
     be dropped, otherwise every write is delivered twice.  `L_Data.req` is
     what a tunnelling client sends (and what a device confirms with `.con`);
     a routing participant sends `.ind`.  The parser rejects every other
     message code (bus monitor, raw frame, property access, reset, polling)
     **before it computes any offset**, because those layouts are different and
     a shared header makes it easy to read a foreign frame as a group value.
     A non-empty additional-info block is legal and must be skipped: all
     following fields are relative to its end, and both the standard and the
     extended frame format use that same cEMI layout.
   - **Outage key**: `/r` and `/w` report `"knxd": {...}` for the knxd
     transport and `"bus": {...}` for the KNXnet/IP transports — same inner
     shape, key from `BusConfig::outage_key()`.
   - **TX queue semantics**: a telegram that cannot be sent stays queued (a
     write is never lost silently), but a write is **rejected** while the bus
     is unavailable — `SharedTxBusLink` returns `false`, `/w` answers `503`
     instead of pretending success.

3. **Shared-memory group cache (`SharedGroupCache`)**: A chained hash table in
   `mmap(MAP_SHARED|MAP_ANONYMOUS)` memory, accessible by all worker processes.
   Each entry stores the KNX group address, APDU value data (up to 14 bytes), a
   Unix timestamp, the `pushed_at` position and the intrusive LRU links.  The
   region is `SharedCacheData` + `capacity` bucket heads + `capacity` entry
   slots; `capacity` comes from `KNX_CACHE_CAPACITY` (default **16384**, ~700
   KiB) and is fixed before `fork()`.
   - **The cache holds the most recently pushed `capacity` distinct addresses.**
     A push of a new address when the table is full evicts the least recently
     *pushed* one (O(1) via the LRU list), so a busy installation keeps its
     active addresses instead of dropping every new one after 2048 (which is
     what the old fixed table did).  Reading does *not* refresh recency — the
     order is "last telegram", not "last client poll".
   - Group address 0 is a normal address: occupancy is defined by the bucket
     chains, not by a `0` sentinel.
   - Timestamp comparisons are signed, so a backwards clock step cannot make an
     entry look infinitely old.
   Synchronization:
   - `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST`
     protects all read/write access to cache entries.
   - `pthread_cond_t` with `PTHREAD_PROCESS_SHARED` and `CLOCK_MONOTONIC`
     enables zero-CPU blocking for long-poll readers.
   - `std::atomic<uint32_t> position` — lock-free reads, monotonically
     increasing across all workers.  The single source of truth for the `i`
     field in `/r` responses.
   - `std::atomic<uint32_t> generation` — incremented on each push, used by
     `wait_for_new_data()` for efficient change detection.  It sits on its own
     cache line (`alignas(64)`), away from `position`, which the reader writes
     on every telegram.
   - `wait_for_new_data(0)` is a non-blocking "nothing can arrive in zero
     time" and always reports `false`; blocking callers pass a positive
     timeout.
   - **The table must survive a process death.**  `CacheLock` reports
     `owner_died()` on `EOWNERDEAD`; `push()`, `get()` and `get_delta()` then
     call `rebuild_locked()`, which re-links every bucket chain and the LRU
     list from the slot array and clamps stored lengths.  Every chain index,
     LRU neighbour and stored `value_len` is range-checked at the point of use:
     the slots live in memory shared by all workers, so an unchecked torn index
     is an out-of-bounds access in every worker, not just the one that crashed.

4. **Long-poll**: For `/r` requests, the ReadHandler blocks on the shared
   condition variable via `pthread_cond_timedwait()` instead of polling a bus
   socket fd.  When the cache reader pushes new data, it broadcasts the
   condition variable — all blocked workers wake immediately (microsecond
   latency) and call `get_delta()` to find matching entries.  Zero CPU while
   waiting.  The `wait_for_new_data()` method handles spurious wakeups and
   robust-mutex recovery (EOWNERDEAD).

   **No request waits longer than 330 s** (`kMaxClientWaitSec`, a little above
   the client's own 300 s long-poll re-create, so backend and client do not
   fight over every reconnect).  `LONGPOLL_TIMEOUT_SEC` is capped at the same
   value and a client supplied `t` is clamped by `clamp_client_wait()` — an
   unbounded `t` would let one request pin a worker for years.  Sessions are
   *not* affected: they live for `SESSION_TTL_SEC` (default 30 min).

   Only `/r` takes a load-shedding/concurrency token; `/w` and `/l` never do,
   because a spurious 503 on a write surfaces in CometVisu as a failed switch
   operation.  Tokens are released via RAII (a throwing handler yields HTTP 500
   instead of terminating the worker) and the parent restores the token of a
   worker that died, so the pool cannot shrink permanently.

   **The long-poll semaphores are sized below the pool** (`num_workers - 2`,
   `longpoll_capacity()` in `main.cpp`): the reservation is what actually keeps
   `/w` and `/l` servable while `LONGPOLL_TIMEOUT_SEC` worth of `/r` clients are
   blocked.  Sizing them to the full pool (as an earlier version did) let a
   handful of readers occupy every worker.

   **Every `/r` and `/w` request must carry the protocol's session parameter
   `s`.**  A missing `s` answers `401 {"error":"missing session"}`; the
   anonymous id `"0"` (returned by `/l` without credentials) stays valid.
   Credentials are *not* validated — see the security model below — but a
   request without a session must never reach the bus or the cache.

   **The initial read is one atomic snapshot.**  `ReadHandler` takes values and
   position from a single `get_delta(0, …)` lock acquisition.  Reading the
   values first and `position()` afterwards loses a telegram pushed in between:
   the response would report an `i` that already covers it, and no later delta
   could ever match.  For the same reason the poll loop queries the delta
   *before* blocking — a client that missed telegrams must not wait for the next
   one (which may be hours away).

5. **KNX semantic correctness rules** — these are non-negotiable constraints
   that every handler implementation MUST satisfy:

   **Rule 1 — No duplicate delivery**: Every KNX telegram must be delivered
   to the client exactly once.  Duplicate delivery is a correctness bug
   because KNX telegrams carry semantic meaning beyond their data value:
   a "toggle" command toggles twice if delivered twice, a "scene trigger"
   triggers the scene twice, a "step" dimming command steps twice.  The
   handler must use `already_written` deduplication within a single response
   AND must never return data with a stale position (`i`) that would cause
   re-reporting of the same telegram on the next poll.

   **Rule 2 — Immediate delivery**: Group telegrams arriving on the knxd
   group socket must be processed without delay.  The cache reader process
   pushes them to the shared cache and broadcasts the condition variable
   immediately.  Blocked long-poll readers wake and query `get_delta()`
   with zero additional latency.

   **Rule 3 — Authoritative index**: The `i` value in every response must
   come from the shared cache's `position` counter.  This counter is
   `std::atomic<uint32_t>`, incremented atomically on each push, and shared
   across all worker processes via `mmap`.  The handler must never fabricate,
   locally infer, or manipulate the position.  The shared counter guarantees
   monotonicity — `i` never goes backward, preventing duplicate delivery
   across worker boundaries.

6. **No external JSON library**: The JSON responses are simple enough to build
   with a minimal, purpose-built `JsonBuilder`.

7. **No external HTTP parser**: FastCGI provides parsed `QUERY_STRING` via
   `FCGI_PARAMS`. We parse the query string ourselves with `QueryString`.

8. **Cache reader process**: A dedicated child process (forked before the
   worker pool) that owns the single bus connection (`BusLink`) for RX *and*
   TX.  It runs a `poll()`-based loop over `{link_fd, tx_eventfd}`: drain the
   shared TX queue first, then drain all pending group telegrams, parse APDU
   data, and push value data to the shared cache.  Each push increments the
   shared position counter and broadcasts the condition variable to wake
   blocked long-poll readers.
   The loop lives in `run_cache_reader()` (`src/bus/cache_reader.{h,cpp}`, unit
   tested via `MockBusLink`) and is resilient by design: the bus may be absent
   at startup or restart at any time (`Link down, terminating`), so every
   failure path reconnects forever with an exponential backoff (500 ms … 30 s)
   and an interruptible sleep.  It must never busy-loop — a spin here burns a
   full CPU core on an embedded system, which is exactly the bug this design
   prevents.  `wait_for_activity()` therefore distinguishes `Timeout` (no data,
   connection fine) from `Error` (POLLHUP/POLLERR/POLLNVAL or not connected),
   and the eventfd is always consumed before draining so it cannot stay
   readable forever.

   **The reader also owns the global send rate limit.**  Because exactly one
   process sends, `KNX_TX_MAX_PER_SEC` (default 10/s) is enforced by a single
   `interval_ms` derived as `max(link.min_send_interval_ms(), ceil(1000/rate))`
   and applied by `drain_tx_queue()` as a wait between sends.  A client — or a
   buggy one retrying in a loop — therefore cannot flood the bus, however many
   writes it queues; the telegrams stay in the ring (`TxDrain::wait_ms` keeps
   reception running while the interval elapses).

9. **Bus availability is reported to the client**: the cache reader publishes
   its connection state into the shared memory region
   (`SharedGroupCache::publish_bus_status()`, read back with `bus_status()`):
   `available`, `last_ok` (Unix time of the last successful connection) and
   `down_since` (Unix time the current outage started, 0 while connected).
   Every retry keeps the first `down_since` of the current outage.  The state
   starts as "available" — until the reader reports otherwise no read may be
   rejected.
   Whenever the connection is down, `/r` and `/w` append
   `"knxd":{"ok":false,"since":<ts>,"lastOk":<ts>}` (key `"bus"` for the
   KNXnet/IP transports) via
   `add_bus_outage()` (`src/handlers/bus_status_json.{h,cpp}`); the field is
   absent while knxd is reachable, so the normal case costs no bytes.  `/r`
   answers **`503`** (a read is not successful while the bus is unreachable) and
   skips the long-poll wait — nothing can arrive, so waiting would only delay
   the message; the body still carries the last known values.  `/w` answers
   `503 {"error":"write failed",...}` and passes `force_unavailable = true`,
   because a failed send proves the outage even if the shared state has not
   caught up yet.  Without this the client cannot distinguish "bus is quiet"
   from "connection lost" and shows frozen values forever.

10. **Sessions are shared memory, not process state**: `SessionStore` owns a
    fixed-size slot array in `mmap(MAP_SHARED|MAP_ANONYMOUS)` memory, created
    in the parent before `fork()` (`SESSION_MAX` slots, default 10000 ≈ 300
    KiB; `SESSION_TTL_SEC`, default 1800).  Every public method takes the
    process-shared robust mutex — there is no unlocked public mutator
    (`count()` and `cleanup_expired()` included).
    - A session id is 16 hex characters from a per-process-seeded
      `mt19937_64`; a collision is detected against the store and the id
      regenerated.
    - When the table is full the **oldest** session (by creation sequence, not
      by wall clock — several logins within one second must still evict in
      order) is evicted.  A client that logs in again therefore always finds
      room, and no existing client is ever told "no".
    - The anonymous session `"0"` is always valid and never occupies a slot.
    - **Authentication is a known, accepted limitation**: `u`/`p` are not
      validated (the backend must sit behind the web server's auth).

## Security Model

**The backend does not authenticate.**  The web server in front of it
authenticates the user; `/l` accepts `u`/`p` without validating them, and session
ids come from a per-process `mt19937_64` — they are identifiers, not secrets, and
are deliberately not required to be cryptographically strong.  Everything below
is enforced anyway, because each item was a bug rather than a policy choice:

- **The session parameter is mandatory**: `/r` and `/w` answer
  `401 {"error":"missing session"}` without `s` (the anonymous id `"0"` stays
  valid).  Omitting it must never skip the check.
- **The FCGI endpoint is the trust boundary**: a Unix socket is `chmod 0660`
  after creation; TCP mode (`FCGI_SOCKET=:port`) has no peer check and logs a
  warning at startup.  Never expose it beyond the reverse proxy.
- **Bounded work per request**: query strings are capped (256 keys × 4096 values
  × 8192 pairs), `/r?t=0` stops generating bus reads when the TX ring rejects
  one, and `KNX_TX_MAX_PER_SEC` caps the outgoing telegram rate globally.
- **Untrusted input is validated, not trusted**: KNXnet/IP frames must match
  their length field exactly, cEMI rejects foreign message codes before any
  offset is computed, APDUs longer than 256 octets are refused, KNXnet/IP total
  lengths are never truncated, `KNX_IP_IFACE` must be representable, and the
  debug log redacts `u`/`p` and escapes control characters.
- **`ip-routing` is unauthenticated by protocol**: any host on the multicast
  VLAN can inject group values, so it belongs on a trusted network segment.
  `ip-tunnel` is the safer transport until KNX IP Secure (ROADMAP) exists.
- **Shared memory is shared between processes**: every index and length read
  from it is treated as untrusted (see decision 3), and a robust-mutex recovery
  rebuilds the structure instead of walking a torn chain.

A source review (security audit, 2026-11) found no use-after-free, uninitialised
read or buffer overflow in the parsers; the remaining risk is deployment, not
parsing.

## Knxd Protocol Details

The eibd client protocol over Unix socket:

### Wire Format
```
[2 bytes: payload length (big-endian)] [payload]
```

### Payload Structure
```
[2 bytes: message type] [message-specific data]
```

### Key Message Types (from `eibtypes.h`)

| Constant                | Value    | Purpose                        |
|-------------------------|----------|--------------------------------|
| `EIB_OPEN_GROUPCON`     | `0x0026` | Open group socket connection   |
| `EIB_GROUP_PACKET`      | `0x0027` | Send group telegram            |
| `EIB_APDU_PACKET`       | `0x0025` | Received group telegram        |

### Group Address Format
KNX three-level address `X/Y/Z` → 16-bit:
```
uint16_t addr = (X << 11) | (Y << 8) | Z;
```

### APDU for group values (KNX 03_07_03)
The APDU starts with two octets that carry TPCI and APCI:

```
octet 1: [7..6] TPCI type | [5..2] TPCI | [1..0] APCI high bits
octet 2: [7..6] APCI low bits | [5..0] value (only for values <= 6 bits)
```

The APCI is therefore `(octet1 & 0x03) << 2 | (octet2 >> 6)`:
`0x0` = A_GroupValue_Read, `0x1` = A_GroupValue_Response, `0x2` =
A_GroupValue_Write.  Everything else (connection-oriented TPCI, individual
address services, the reserved patterns) is **not** a group value and must be
rejected — `parse_apdu()` does exactly that and returns `std::optional<Apdu>`.

A value that fits into 6 bits travels inside octet 2; a longer one starts at
octet 3:

```
0x00 0x80 0x2a        → A_GroupValue_Write, data=2a        (6-bit packing)
0x00 0x80 0x0c 0x6f   → A_GroupValue_Write, data=0c6f      (DPT 9.001)
```

A 1-byte value above `0x3F` must use the multi-octet form (`0x00 0x80 0x42`),
which is what `WriteHandler` produces and what every device expects.

### Group Socket Open (EIB_OPEN_GROUPCON)
```
[0x00 0x26] [write_only_byte]
```
Response: empty success or error message.

### Sending a Group Packet (EIB_GROUP_PACKET)
```
[0x00 0x27] [dest_addr_hi] [dest_addr_lo] [APDU bytes...]
```

### Receiving a Group Packet (EIB_APDU_PACKET)
```
[0x00 0x25] [src_pa_hi] [src_pa_lo] [dst_ga_hi] [dst_ga_lo] [APDU bytes...]
```
Note: The destination group address (dst_ga) is at offset 2-3 of the payload
(after the 2-byte type), NOT at offset 0-1 (which is the source physical address).

knxd also echoes injected telegrams back as GROUP_PACKET messages:
```
[0x00 0x27] [src_pa_hi] [src_pa_lo] [dst_ga_hi] [dst_ga_lo] [APDU bytes...]
```
Both message types are handled identically by the cache reader — the
payload structure is the same (src_pa + dst_ga + apdu).

## APDU Decoding

`parse_apdu()` (`src/knxd/knxd_protocol.cpp`) is the single implementation,
shared by every transport:

- `b0` carries the TPCI *type* in bits 7..6 and the APCI **high** bits in
  bits 1..0.  A group value always travels in a data packet, so
  `(b0 & 0xC0) != 0` is rejected.
- The APCI is 4 bits wide: `((b0 & 0x03) << 2) | (b1 >> 6)`.
  `0x2` = A_GroupValue_Write, `0x1` = A_GroupValue_Response,
  `0x0` = A_GroupValue_Read.  Every other value is refused — there is no
  "unknown but successful" result, because a caller must never mistake a
  management frame for a group value.
- For a 2-octet APDU the value is `b1 & 0x3F` (≤ 6 bits); otherwise the
  payload is `b2, b3, ...` (`std::optional<Apdu>` with `Apdu::data`).
- The result is a value: `parse_apdu(std::span<const uint8_t>)`, not an
  out-parameter triple.

## Hex Encoding for CometVisu

KNX data values are transmitted as hex strings in CometVisu JSON:
- Single byte `0x42` → `"42"`
- Two bytes `0x0c 0x6f` → `"0c6f"`
- Always lowercase, no spaces, no `0x` prefix.

## Test Mocking Strategy

For unit tests of the shared cache, use `SharedGroupCache::create()` which
allocates a real mmap'd region — this works fine in single-process tests.
Pass a small capacity (`create(4)`) to exercise LRU eviction without pushing
thousands of telegrams.  For multi-instance tests (simulating multiple
workers), use `attach()` to share the same underlying `SharedCacheData`
pointer; the same applies to `SessionStore` (and a plain `fork()` test proves
that a child sees the parent's session — the regression test for "sessions are
per worker").

For integration tests of the ReadHandler, push test data directly to the
shared cache via `cache_.push()` before calling `handle()`.  For poll-loop
tests that need deferred data arrival, use a separate thread that calls
`cache_.push()` after a short delay while the handler is blocked in
`wait_for_new_data()`.

### Concurrency and failure-mode tests (mandatory for transport code)

The multi-process parts of the system are the ones that break silently, so they
each own a dedicated suite — extend them, do not weaken them:

| Suite | What it proves |
|---|---|
| `tests/integration/test_tx_queue_multiprocess.cpp` | forked producers lose/duplicate/reorder nothing, a killed producer does not wedge the ring, `EOWNERDEAD` recovery under load, full queue rejects cleanly, a restarted consumer finds the backlog intact |
| `tests/unit/test_cache_reader_backpressure.cpp` | slow/silent bus, backlog preserved across an outage, prompt shutdown with a large backlog, concurrent handler enqueues |
| `tests/integration/test_transport_recovery.cpp` | real `TunnelingBusLink` + queue against a rate-limited / silent / restarting router: exactly-once delivery, backlog survives the outage, device-side tunnel loss triggers a reconnect |
| `test_concurrent_clients` (new cases) | many FCGI clients across forked workers reach the bus exactly once; writes during an outage are rejected **and not queued**; blocked long-polls never delay `/w` |

Rules these tests encode (do not regress them):
- the TX ring is peek-then-drop: a telegram is only removed after the link
  accepted it;
- at most `kMaxTxBatch` telegrams are sent per drain call, with a zero-timeout
  poll in between, so RX and `should_stop()` are never starved;
- a failed send must fall through to a sleeping wait — a backlog plus a failing
  link must never spin;
- every loop iteration re-checks `BusLink::is_connected()`: a link can die
  between two polls (device dropped the tunnel) and the reconnect depends on it.

For transport-independent tests, `MockBusLink` in `mocks/mock_bus_link.h`
provides a scriptable `BusLink` (connection failures, send failures, queued
inbound telegrams).  ReadHandler tests do not need it: they push directly into
the shared cache.

### Black-box interoperability tests (the only thing that catches protocol bugs)

A symmetric unit test proves that our encoder and our decoder agree — not that
either matches the standard.  Every KNXnet/IP defect found so far (wrong
service codes, a missing CRI length byte, a five-byte ACK, `L_Data.req` instead
of `.ind` for routing, a heartbeat that was never sent) was invisible to the
unit tests and fatal against a real peer.

`tests/e2e/knxnetip_blackbox.{h,cpp}` provides the harness:

| Piece | Purpose |
|---|---|
| `Knxd` | a supervised real `knxd`: `-b ip:<group>:<port>:<iface>` is a KNXnet/IP **router**, `-T -S <group>:<port>` a **tunnel server**; restart simulates an outage |
| `Backend` | the deployed `cometvisu-knx-fcgi` binary as a child process with a direct FCGI socket and its own log |
| `fcgi_request()` | a minimal FastCGI client (one connection per request, usable from `std::async` for long-polls) |
| `RoutingMonitor` | a member of the routing group: records every `ROUTING_INDICATION` with source, TTL and cEMI, and injects telegrams from a *foreign* source port, exactly like another router |
| `knxtool` calls | `groupswrite` injects on knxd's bus; `groupcacheread` is the independent witness that a write reached it |

The two suites (`test_ip_routing_blackbox`, `test_ip_tunnel_blackbox`, label
`blackbox`, 59 cases) cover login, cache reads, long-poll wake-up and latency,
multi-byte and maximum-length values, per-address filtering, repeated identical
telegrams, monotonic positions, multi-client delivery, "the write is not echoed
twice", writes arriving on the bus, the wire format (service code, cEMI type,
source address, TTL), outage reporting and reconnect, NAT/local-address options
and concurrent long-polls.

Rules for these tests:
- they must skip (never fail) when `knxd`/`knxtool` are missing — the CI
  `no-knxd` job and the coverage job run without them;
- a test that asserts what the backend puts on the wire must assert the
  *service code* and the *cEMI message code*, not just the payload;
- knxd ignores datagrams from its own IP:port ("ignore multicast packets sent
  by ourselves"), so a same-host test must give the backend a different source
  address — hence `KNX_IP_IFACE=lo` on both sides (source `127.0.0.1` vs
  knxd's `172.17.0.3`);
- knxd accepts `-S` only as the *last* option group; the harness builds the
  command line in that order;
- give the backend several workers (`FCGI_WORKERS=4`): a blocked long-poll must
  not be able to starve `/w`.

## Reference: Existing CometVisu Backend Implementations

- PHP: https://github.com/CometVisu/cometvisu-bsp/blob/master/src/php/helper/KnxHelper.php
- Perl (linkback): part of the CometVisu distribution

These handle address parsing, hex conversion, and the full protocol flow.

## Logging

Use simple stderr-based logging with levels:
- `ERROR`: Fatal problems
- `WARN`: Recoverable issues
- `INFO`: Normal operational messages
- `DEBUG`: Detailed diagnostics

Format: `[LEVEL] [timestamp] message` — `DebugLog::log(Level, std::string_view)`
implements it (one `std::cerr` write per line, so lines from different worker
processes never interleave).  `DebugLog::http_request()` / `knxd_send()` /
`knxd_recv()` are the `DEBUG_BACKEND=1` traces; operational messages are always
written.  Never `std::cout` for a diagnostic: stdout carries the startup
banner only.
