# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - unreleased

### Removed

- **Spawn-fcgi mode** (FastCGI on stdin/stdout) is gone: a process that does
  not own a listen socket cannot host the worker pool or the cache reader.  See
  *Breaking changes* for the migration.

### Security

- **`/r` and `/w` now require the protocol's session parameter `s`** and answer
  `401 {"error":"missing session"}` without it (the anonymous session `"0"` that
  `/l` returns stays valid).  Authentication itself remains the web server's
  job; this closes the trivial "omit `s` and skip the check" path.
- **Global send rate limit**: `KNX_TX_MAX_PER_SEC` (default **10** telegrams per
  second) bounds the outgoing traffic of *every* client and worker, because the
  cache reader is the only process that talks to the bus.  `/r?t=0` additionally
  stops generating `GroupValueRead` telegrams as soon as the TX ring rejects one,
  instead of hammering a queue that cannot accept them.
- **Long polls can no longer starve the short requests**: two workers are
  reserved for `/w` and `/l`, so a full set of blocked `/r` clients cannot delay
  a switch operation for up to 330 s.
- **FCGI socket**: a Unix socket is now restricted to `0660` after it is
  created, and TCP mode (`FCGI_SOCKET=:port`, no authentication and no peer
  check) logs a warning at startup.
- **Tunnelling ACK status is honoured**: `TUNNELLING_ACK` `0x01` retransmits the
  same sequence (as the standard asks), `0x02` drops the telegram and fails the
  write.  A NACK was previously treated as success, silently losing the write.
- **Shared memory is fail-safe**: after a robust-mutex recovery (`EOWNERDEAD`)
  the group cache rebuilds its bucket/LRU structure, every chain and LRU index is
  range-checked, and stored value lengths from the TX ring / cache are clamped.
  A worker that dies mid-`push()` can no longer make the other workers read
  outside the shared slot array.
- **Debug log**: `u=`/`p=` values are redacted and control characters are
  replaced, so credentials never reach a log file and a request cannot forge log
  lines.
- Smaller hardening: over-long APDUs are rejected by the cEMI and KNXnet/IP
  encoders instead of being length-truncated, `JSON` strings escape C0 control
  characters, a too-long `KNX_IP_IFACE` is a startup error, `MSG_CTRUNC` is
  checked on routing datagrams, and a clock step backwards no longer expires
  every session.

### Delivery correctness

- **The initial read of `/r` is one atomic snapshot** of values *and* position.
  Previously the position was read after the values, so a telegram pushed in
  between was reported as "already delivered" by `i` and never sent.
- **A delta is delivered before blocking**: telegrams that arrived between two
  requests are returned immediately instead of waiting for the *next* telegram
  (which may be hours away on a quiet bus).

### Breaking changes

- **`FCGI_SOCKET` is required; the spawn-fcgi mode is gone.**  The backend
  always opens its own FastCGI listen socket (a Unix socket path or
  `host:port`) and serves it with the fork-based worker pool.  Running the
  binary under `spawn-fcgi` (FastCGI on stdin/stdout) is no longer supported: in
  that mode no cache reader exists, so `/w` answered `200` for telegrams that
  were only queued and `/r` could never return a value.  A missing
  `FCGI_SOCKET` is now a startup error.
  **Migration:** start the binary from systemd/supervisord/the container
  entrypoint with `FCGI_SOCKET=/run/cometvisu-fcgi.sock` and point the web
  server at that socket (Apache
  `ProxyPass "/cgi-bin/r" "unix:/run/cometvisu-fcgi.sock|fcgi://localhost/r"`,
  nginx `fastcgi_pass unix:/run/cometvisu-fcgi.sock;`), or use
  `FCGI_SOCKET=127.0.0.1:9000` with `fastcgi_pass 127.0.0.1:9000;`.  The
  official CometVisu container already works this way.

- **`KNX_TRANSPORT` is now mandatory.**  There is no default transport: choose
  `knxd`, `ip-tunnel` or `ip-routing`.  An unset or unknown value stops the
  backend at startup with `[ERROR] KNX_TRANSPORT is required …` instead of
  silently falling back to knxd — a wrong `ip-*` setup must not look like an
  outage.
  **Migration:** add `KNX_TRANSPORT=knxd` to keep 1.x behaviour unchanged.
- **The project is renamed** from `cometvisu-knxd-fcgi` to
  `cometvisu-knx-fcgi` (<https://github.com/CometVisu/cometvisu-knx-fcgi>).
  Update container, systemd and supervisor commands that start the binary by
  path; a `cometvisu-knxd-fcgi` symlink keeps existing setups working.
- **The outage block in `/r` and `/w` uses a transport-specific key**: `"knxd"`
  for the knxd transport (unchanged) and `"bus"` for the KNXnet/IP transports.
  The inner object (`ok`, `since`, `lastOk`) is identical.

### Added

- **`SESSION_TTL_SEC` / `SESSION_MAX`** (default `1800` / `10000`): session
  lifetime and the number of concurrent sessions.  A *session* lives for
  minutes — it is deliberately independent of the request wait limit.
- **`KNX_CACHE_CAPACITY`** (default `16384`): number of distinct group
  addresses kept in the shared cache.
- **`KNX_TX_MAX_PER_SEC`** (default `10`): hard upper bound on outgoing
  telegrams per second, over all clients and workers (see *Security* above).
- **Direct KNXnet/IP tunnelling** (`KNX_TRANSPORT=ip-tunnel`): the backend is a
  client of a KNX IP interface or router — no knxd required.  Includes the
  tunnel handshake, acknowledgement and retransmission, rejection of duplicate
  and out-of-order frames, keep-alive, and automatic reconnect after a device
  restart or tunnel loss.
- **Direct KNXnet/IP routing** (`KNX_TRANSPORT=ip-routing`): the backend joins
  the routing multicast group (default `224.0.23.12`) and works with **no knxd
  installed**.
- **`KNX_IP_INDIVIDUAL_ADDR`** (`ip-routing`, default `15.15.250`): the cEMI
  source address of our routing indications.  A router rejects a frame from
  `0.0.0`, so this is what makes a routing *write* reach the bus.
- **`--discover`**: sends a KNXnet/IP SEARCH_REQUEST and lists every answering
  device with its individual and multicast address.
- **Black-box interoperability suites** (`tests/e2e/test_ip_routing_blackbox.cpp`,
  `tests/e2e/test_ip_tunnel_blackbox.cpp`): a real `knxd` provides the
  KNXnet/IP router and tunnel server, `knxtool` injects telegrams, a test
  process monitors the multicast backbone and the deployed binary is driven over
  a direct FastCGI socket.  59 cases cover login, long-poll, writes, duplicate
  delivery, reconnect, outage reporting and the transport options.  They skip
  themselves when `knxd`/`knxtool` are not installed.

### Changed

- **`/r` answers 400 for an unparsable `t`** and every request wait is capped
  at **330 s** (`kMaxClientWaitSec`), a little above the client's own 300 s
  long-poll re-create.  A client can no longer pin a worker with `t=999999`.
- **`/w` answers 400 for a value that is not a `A_GroupValue_Write`** (bad
  hex, missing APCI, too long) instead of pretending success, and a partially
  applied multi-address write reports `write partially failed` with the number
  of failed addresses.
- **The cache keeps the last `KNX_CACHE_CAPACITY` distinct addresses** (LRU
  eviction) instead of dropping every new address once a fixed table of 2048
  was full.
- **Transport pacing belongs to the transport.**  `KnxdClient` no longer
  sleeps 50 ms inside `send_group_packet()` (which stalled the receive path)
  and no longer reconnects behind the cache reader's back; it declares
  `min_send_interval_ms()` and the reader *waits* between sends, so inbound
  telegrams keep flowing.

- **One bus connection regardless of `FCGI_WORKERS`.**  All workers share the
  single bus link owned by the cache reader, so the worker count no longer
  multiplies knxd client slots (one instead of `FCGI_WORKERS + 1`) or KNXnet/IP
  tunnels (one instead of one per worker — a KNX IP interface usually offers
  only four).
- **A write during a bus outage is reported as failed** (`503`) instead of
  being queued: the client learns immediately that the telegram did not leave,
  rather than receiving `200`.  A telegram that was accepted but cannot be sent
  stays queued and is delivered after the reconnect.
- **knxd is optional at build time.**  `-DKNX_BUILD_KNXD_TRANSPORT=OFF` builds
  a binary without any knxd header or daemon.

### Fixed

- **Login sessions did not work with more than one worker.**  The session
  store was an ordinary member copied by `fork()`, so a session created by
  `/l` was unknown to every other worker: with `FCGI_WORKERS=4` every
  follow-up `/r`/`/w` answered `401` (verified 15/15).  Sessions now live in
  shared memory like the caches, and the oldest session is evicted when
  `SESSION_MAX` is reached.
- **A cEMI frame with additional information was misparsed.**  Control
  fields, source and destination were read at fixed offsets while the APDU
  offset correctly accounted for the additional-info length, so a legal frame
  with `additional_info_len > 0` produced wrong group addresses.  Only
  `L_Data.ind` with a group destination is parsed now; every other message
  code (`.req`, `.con`, bus monitor, raw, property access, reset) is rejected
  before any offset is computed.
- **`parse_apdu()` decoded the APCI from the wrong bits** and accepted the
  reserved pattern `0xC0` as an "unknown type" with empty data, which then
  reached the cache as an empty value.  The 4-bit APCI is now split across
  both APDU octets as `(octet1 & 0x03) << 2 | (octet2 >> 6)`, only the three
  group-value services are accepted, and the result is returned by value.
- **`SharedGroupCache` could not store group address 0**, advanced `i` for
  entries it dropped, compared timestamps with unsigned arithmetic (a
  backwards clock step made every entry look 136 years old) and shared one
  cache line between `position` and `generation`.
- **`SessionStore::cleanup_expired()` and `count()` ran without the mutex**,
  and `parse_env_int()` truncated a huge value into an `int` before the range
  check.
- **The KNXnet/IP tunnel accepted datagrams from any sender.**  The UDP socket
  is connected to the configured device now, so the kernel filters by peer;
  any frame also had to carry *our* channel before it counted as liveness.
- **`ip-routing` accepted a unicast datagram sprayed at its port.**  Every
  datagram's destination address is checked with `IP_PKTINFO` against the
  configured routing group.

- **`ip-routing` never exchanged a single frame with a real KNXnet/IP router.**
  The `ROUTING_INDICATION` service code was `0x0330`; the KNXnet/IP standard
  (03_08_01, 5.*) defines `0x0530` (`ROUTING_LOST_MESSAGE` `0x0531`,
  `ROUTING_BUSY` `0x0532`).  Routers silently dropped our frames and we dropped
  theirs, so the transport could neither read nor write the bus.
- **`ip-routing` used the wrong cEMI frame type for writes.**  A routing
  participant acts as a router, so it sends `L_Data.ind` (`0x29`); an
  `L_Data.req` (`0x11`) — the tunnelling frame type — is discarded by knxd and
  by real interfaces.  Writes are now published as an indication with the
  configured individual address.
- **`ip-tunnel` could not talk to a real interface.**  The `TUNNELLING_REQUEST`
  / `TUNNELLING_ACK` service codes were `0x0320`/`0x0321` instead of
  `0x0420`/`0x0421`, so devices ignored our data frames (and we ignored theirs),
  and a device never ACKed a write.
- **`CONNECT_REQUEST` was missing the CRI structure-length byte.**  A device
  validates `CRI[0] == body size - 16` and drops the request otherwise; the
  tunnel never came up.
- **`TUNNELLING_ACK` was one byte too long.**  The acknowledgement has no
  reserved byte (structure length, channel, sequence, status), so every ACK of a
  real device was rejected as malformed — writes were retransmitted and then
  dropped, and the sequence counter ran out of sync.
- **The tunnelling heartbeat was never sent.**  The cache reader polls the raw
  file descriptors and never called the transport's timer logic, so
  `KNX_IP_HEARTBEAT_SEC` had no effect and a device would drop the tunnel after
  120 s of silence.  `BusLink::maintain()` now runs once per loop iteration.
- **A dead KNXnet/IP device was never noticed.**  An unanswered
  `CONNECTIONSTATE_REQUEST` now ends the tunnel, so `/r` reports the outage
  instead of waiting forever on frozen values.
- **`ip-routing` selected its outgoing interface by index**, which leaves the
  source address to the routing table — wrong on a multi-homed host and when
  another KNXnet/IP participant runs on the same host.  The interface's IPv4
  address is used instead.
- **A `POST`/`PUT` request could terminate a worker** in direct-socket mode: the
  body length was taken from the wrong stream and an overflowing value threw
  out of the request loop.  The worker died (and was restarted), the request was
  lost.
- **The cache reader could stop reconnecting** when writes were in flight, so
  queued telegrams stayed stuck until a restart.
- **A permanently failing send could spin the cache reader at 100 % CPU.**
- **A telegram that failed to send was dropped.**  It now stays queued and is
  sent after the reconnect — a write is never lost silently.
- **A rejected knxd connection leaked a file descriptor** per attempt when
  `KNXD_SOCKET` was misconfigured, eventually exhausting the process.

## [1.2.1] — 2026-11-15

### Fixed

- **Startup log lines were repeated once per worker process.**  When stdout is
  not a terminal — the normal case in a container — the parent's log output is
  buffered.  Each worker forked with those lines still pending, inherited a copy
  of the buffer and printed it again, so `docker logs` showed the startup banner
  up to `FCGI_WORKERS` times.  All forks now flush the buffered output first
  (`src/util/fork_safety.{h,cpp}`, covered by `test_fork_safety`).
- **Log lines of concurrently starting processes could interleave.**  The worker
  and cache-reader warnings are now emitted as one line each, so ten starting
  workers can no longer produce output like `[WARN] [WARN] Worker pid=Cannot
  connect …`.

## [1.2.0] — 2026-11-14

### Added

- **`/r` and `/w` report a knxd outage to the client.**  A dead knxd used to be
  invisible over HTTP: `/r` kept answering `200` with the last known — but
  frozen — values, and only `/w` failed, so a CometVisu client could not tell
  "the bus is quiet" from "the connection is gone".  The cache reader now
  publishes its connection state (available / last successful connection /
  start of the current outage) in the shared memory region, and both handlers
  append a block while knxd is unreachable:

  ```json
  {"d":{...},"i":42,"knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
  {"error":"write failed","knxd":{"ok":false,"since":1763000000,"lastOk":1762990000}}
  ```

  The field is absent while knxd is reachable (no overhead in the normal case).
  `since` is the Unix time the current outage started, `lastOk` the Unix time of
  the last successful connection; both are omitted when unknown (knxd never
  connected since startup, or the outage start was not recorded yet).
  A read is not successful while the bus connection is gone, so `/r` answers
  `503` — and it answers immediately instead of waiting for the long-poll
  timeout, because nothing can arrive.  The body still carries the last known
  values, so a client may keep showing them and add a "connection lost"
  marker.  `/w` answers `503` with `{"error":"write failed",...}`.
  `JsonBuilder` gained `add_bool()` for the `ok` literal.

### Fixed

- **The router dropped the `/w` response body.**  `Router::route()` only copied
  `http_status` for `/w`, so error details never reached the client.
- **Configurations with more than 250 group addresses lost all values above
  address ~250** ([CometVisu#1521](https://github.com/CometVisu/CometVisu/issues/1521)).
  `QueryString` capped the number of values per key at 250 (and total key/value
  pairs at 256) and silently dropped the excess.  Since CometVisu subscribes
  every address of a visualization in one request, no `GroupValueRead` was sent
  for the dropped addresses and the long-poll filter never matched them — the
  corresponding tiles stayed empty forever (independent of the order in the
  config).  The limits are now `kMaxValuesPerKey = 4096`,
  `kMaxTotalPairs = 8192` and `kMaxQueryParams = 256`, and they are public
  constants on `QueryString`.

### Changed

- **Query-string limit violations are no longer silent.**  `QueryString`
  reports truncation via `limit_exceeded()`; `/r` and `/w` answer with
  `400 {"error":"too many addresses"}` instead of returning (or writing) an
  incomplete address list.

### Tests

- **A CI job can no longer hang on a leaked test worker.**  The fork-based
  tests (`test_fork_workers`, `test_concurrent_clients`) left forked workers
  behind on early assertion failures and terminated them with `SIGTERM`,
  which a worker blocked in `FCGX_Accept_r()` may ignore.  A surviving worker
  keeps the pipe open that ctest uses to capture test output, so ctest waits
  forever for an EOF that never comes and the job sits at
  `Start 23: test_fork_workers` for hours.  Workers now die with their parent
  (`PR_SET_PDEATHSIG`), are killed with `SIGKILL` and reaped per pid, and are
  tracked so `TearDown()` reaps them even after a failed `ASSERT`.
- `test_fork_workers`'s fork-failure test no longer lowers `RLIMIT_NPROC`
  (which counts the tasks of the whole real UID and therefore behaves
  completely differently on a CI runner than on a developer machine); the
  fork primitive is injected instead, making the failure point deterministic.
  A new test `WorkersDieWithTheirParent` covers the orphan scenario.
- CI runs `ctest --timeout 120` and the job has a `timeout-minutes`
  backstop, so a wedged test fails within seconds instead of blocking the
  workflow.

## [1.1.0] — 2026-11-01

### Fixed

- **Cache reader burned 100% CPU when knxd was missing.**
  `KnxdClient::wait_for_activity()` reported "not connected" and
  `POLLHUP`/`POLLERR`/`POLLNVAL` as a plain timeout, so the cache reader
  loop retried `poll()` in a tight userspace loop and never reconnected —
  `/r` kept returning stale values forever while one CPU core was saturated.
  `WaitResult` now has an `Error` state and the loop (new
  `run_cache_reader()` in `src/knxd/cache_reader.cpp`) reconnects forever
  with an exponential backoff (500 ms … 30 s, rate-limited logging).
- The daemon no longer refuses to start (or exits) when knxd is unavailable:
  parent, cache reader and workers log a warning and retry in the background,
  so knxd may be restarted at any time without restarting the backend.
- `KnxdClient::connect()` now remembers the socket path even when the attempt
  fails, so `reconnect()` can recover once knxd is back.
- **A `/w` request can no longer block forever on the knxd handshake.**
  `open_group_socket()` read the `EIB_OPEN_GROUPCON` reply from a blocking
  socket without a deadline, so a knxd that accepted the connection but never
  answered hung the worker (and with it the request) indefinitely.  The reply
  is now read with a deadline (default 5 s, `set_handshake_timeout_ms()`).
- **Writes and logins are no longer rejected with 503.** The concurrency and
  load-shedding semaphores applied to `/w`/`/l` as well, so a pool full of
  long-polls made switch operations fail.  Both now only guard `/r`.
  Tokens are released via RAII (also when a handler throws — that used to
  terminate the worker process; it now returns HTTP 500), and a worker that
  dies restores its token instead of permanently shrinking the pool.
- **The parent supervisor restarts dead children.** The SIGCHLD handler used
  to `waitpid()` all children itself, so the supervisor never saw an exit
  status: a crashed worker was never replaced and a dead cache reader was
  never restarted (leaving `/r` on stale data forever).  The parent is now the
  only reaper, keeps the listen socket open for replacements, restarts workers
  and the cache reader with a crash-loop backoff, and no longer ignores
  SIGTERM while blocked in `wait()` (`sigaction()` without `SA_RESTART`).
- `FcgiRequest::path()` falls back to the last component of `SCRIPT_NAME` when
  no `REQUEST_URI` is passed (pure-CGI setups such as `cgi-fcgi`).

## [1.0.0] — 2026-07-05

### Added

- Initial release of cometvisu-knxd-fcgi.
- FastCGI backend implementing the CometVisu Protocol.
- `/l` (login) endpoint with session management.
- `/r` (read) endpoint with long-poll support via `poll()`.
- `/w` (write) endpoint for sending KNX telegrams.
- knxd Unix socket client with eibd binary protocol support.
- Address cache integration with knxd's built-in cache.
- Configurable knxd socket path (`KNXD_SOCKET` env var).
- Configurable long-poll timeout (`LONGPOLL_TIMEOUT_SEC` env var).
- Debug logging via `DEBUG_BACKEND` environment variable.
- JSON response builder (no external JSON library dependency).
- Query string parser.
- Comprehensive test suite: unit, integration, and end-to-end tests.
- CMake build system with Google Test integration.
- GPLv3 licensed.
