# cometvisu-knx-fcgi

FastCGI backend in modern C++ that implements the
[CometVisu Protocol](https://github.com/CometVisu/CometVisu/wiki/Protocol)
and connects to the KNX bus.

Bus access goes through one of three transports, all equally supported:

| `KNX_TRANSPORT` | Transport                          | Needs                      |
|-----------------|------------------------------------|----------------------------|
| `knxd`          | knxd daemon over a Unix socket     | knxd running locally       |
| `ip-tunnel`     | KNXnet/IP tunnelling client        | a KNX IP interface/router  |
| `ip-routing`    | KNXnet/IP multicast routing member | a KNX IP router            |

`KNX_TRANSPORT` is **required** — there is no default, so a misconfiguration
can never be mistaken for a bus outage.  `ip-routing` and `ip-tunnel` need no
knxd at all; `-DKNX_BUILD_KNXD_TRANSPORT=OFF` builds a binary without any knxd
dependency.

## Upgrading from 1.x

The backend was called `cometvisu-knxd-fcgi` up to v1.2.1; 2.0 renamed it and
changed four things that a deployment has to follow:

| 1.x | 2.0 |
|---|---|
| binary `cometvisu-knxd-fcgi` | binary `cometvisu-knx-fcgi` (repository <https://github.com/CometVisu/cometvisu-knx-fcgi>) |
| knxd implied | `KNX_TRANSPORT=knxd` must be set (or `ip-tunnel` / `ip-routing`) |
| started by `spawn-fcgi` on stdin/stdout | `FCGI_SOCKET` is required; the backend opens and serves the listen socket itself |
| `/r` and `/w` worked without `s` | both answer `401` without the protocol's session parameter `s` |

A symlink `cometvisu-knxd-fcgi -> cometvisu-knx-fcgi` keeps old paths working;
everything else needs an explicit setting.  See
[CHANGELOG.md](CHANGELOG.md) for the full list, including the transport-specific
outage key (`"knxd"` vs `"bus"`).

## Endpoints

All three endpoints take their parameters in the query string and answer JSON.
`ADDRESS_PREFIX` decides whether addresses are written `1/2/3` (default) or
`KNX:1/2/3`.

| Endpoint | Purpose | Parameters |
|---|---|---|
| `/l` | login, creates a session | `u`, `p` (accepted, not validated — see [Security](#security)) |
| `/r` | read values, long-poll | `s` session, `a` address (repeatable), `t` timeout/age filter, `i` last known position |
| `/w` | write a group value | `s` session, `a` address (repeatable), `v` hex APDU |

**`GET /l`** returns the protocol version and a new session id; with `BASE_URL`
or build information a `"c"` configuration block is added:

```json
{"v":"0.0.2","s":"9f1c0d2a4b6e8f70","c":{"baseURL":"/proxy/visu","fcgiVersion":"2.0.0"}}
```

Without `u`/`p` the session is the anonymous id `"0"`, which stays valid for
every later request.

**`GET /r`** returns the values of the requested addresses plus the shared cache
position `i`, which the client sends back as `i=` on the next request:

```json
{"d":{"1/2/3":"42","1/2/4":"0c6f"},"i":141}
```

* `t` absent — long-poll up to `LONGPOLL_TIMEOUT_SEC` seconds, no age filter.
* `t=0` — read from the bus: return cached values, enqueue a `GroupValueRead`
  for every address without a cached value, return immediately.
* `t>0` — return values at most `t` seconds old, then poll for `t` seconds
  (capped at 330).
* `t<0` — cache only, return immediately.
* `i` absent or `0` — a full snapshot instead of a delta.

Status codes: `200` ok, `400` malformed request (missing/invalid address list,
`t` not a number, too many parameters), `401` missing/invalid session, `404` no
address could be parsed, `503` the bus is unavailable (the body still carries
the last known values plus the outage block).

**`GET`/`POST /w`** writes the APDU given in `v` (hex, with the APCI byte, e.g.
`800c6f` for a 2-byte write of `0x0c6f`) to every address in `a`:

* `200` — empty body, the telegram was handed to the bus.
* `400` — `v` is not an `A_GroupValue_Write`, or the parameter list was cut off.
* `404` — no address could be parsed.
* `503` — `{"error":"write failed","failed":1,"total":1,"knxd":{...}}` — the
  write did not reach the bus (partial failures report the counts and
  `"write partially failed"`).

A write is never answered with `200` unless it left the backend, and the
outgoing rate is capped globally by `KNX_TX_MAX_PER_SEC`.

## Quick Start

```bash
# Prerequisites (knxd-dev is only needed for the knxd transport)
sudo apt install libfcgi-dev cmake g++
sudo apt install knxd-dev        # only for KNX_TRANSPORT=knxd

# Build (using system-installed knxd headers)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Building without the knxd-dev system package

If you build knxd from source, point CMake at your checkout:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKNXD_SOURCE_DIR=/path/to/knxd
cmake --build build -j$(nproc)
```

The build expects to find `eibtypes.h` in `<KNXD_SOURCE_DIR>/include/`.

### Development container (recommended)

The repository ships a VS Code dev container with the complete toolchain, so no
local package installation is needed:

```bash
# In VS Code: "Dev Containers: Reopen in Container"
# or from a terminal (needs the devcontainer CLI):
devcontainer up --workspace-folder .
```

`.devcontainer/Dockerfile` installs g++/clang/cmake/ninja, clang-format,
clang-tidy, gdb, `libfcgi-dev`, `knxd` + `knxd-dev` (for the knxd transport) and
`tcpdump`/`socat`/`iproute2` for KNXnet/IP debugging.  The project is configured
and built automatically after the container is created; the existing
`.vscode` tasks and launch configurations work unchanged inside the container.

> For testing against a real KNX IP router, UDP 3671 and the routing multicast
> group `224.0.23.12` must be reachable from the container — on Linux add
> `"runArgs": ["--network=host"]` to `.devcontainer/devcontainer.json`.

### Run tests / Run the server

```bash
# Run tests
ctest --test-dir build --output-on-failure

# Only the black-box interoperability suites (needs knxd + knxtool)
ctest --test-dir build -L blackbox --output-on-failure

# Run the backend (it opens FCGI_SOCKET itself)
KNX_TRANSPORT=knxd FCGI_SOCKET=/run/cometvisu-fcgi.sock ./build/src/cometvisu-knx-fcgi

# Or with a TCP port on the loopback interface
KNX_TRANSPORT=knxd FCGI_SOCKET=127.0.0.1:9000 ./build/src/cometvisu-knx-fcgi

# Point the web server at the Unix socket, e.g. Apache:
#   ProxyPass "/cgi-bin/r" "unix:/run/cometvisu-fcgi.sock|fcgi://localhost/r"
# or nginx:
#   fastcgi_pass unix:/run/cometvisu-fcgi.sock;

# Direct KNXnet/IP connection, no knxd needed
KNX_TRANSPORT=ip-tunnel KNX_IP_HOST=192.168.1.10 FCGI_SOCKET=:9000 \
    ./build/src/cometvisu-knx-fcgi
KNX_TRANSPORT=ip-routing KNX_IP_IFACE=eth0 FCGI_SOCKET=:9000 \
    ./build/src/cometvisu-knx-fcgi

# Which devices answer?
./build/src/cometvisu-knx-fcgi --discover
```

### Code coverage

```bash
tools/coverage.sh                  # instrumented build + tests + gate
tools/coverage.sh --strict         # require 100 % lines and branches everywhere
tools/coverage.sh --list-uncovered # show what is still missing
```

CI measures coverage on **every pull request** (`.github/workflows/ci.yml`,
job `coverage`), publishes the per-file table in the workflow summary and fails
the PR on a regression or on new code that is not fully covered.

The gate is `tools/check_coverage.py`: it reads the gcov counters of every
translation unit and requires that each line and each branch arm of `src/` is
reached.  Files listed in `tools/coverage-baseline.json` may keep their
recorded (legacy) coverage but must never regress; any file that is not listed
must be fully covered.  Use a separate build directory — `tools/coverage.sh`
does that for you.

### Environment variables

| Variable              | Default    | Description                              |
|-----------------------|------------|------------------------------------------|
| `KNX_TRANSPORT`       | **required** | `knxd`, `ip-tunnel` or `ip-routing`. Unset or unknown → startup error. |
| `KNXD_SOCKET`         | `/run/knx` | knxd transport: path to the knxd Unix socket |
| `KNX_IP_HOST`         | *(required for `ip-tunnel`)* | Address of the KNX IP interface/router |
| `KNX_IP_PORT`         | `3671`     | KNXnet/IP UDP port                       |
| `KNX_IP_LOCAL_ADDR`   | *(OS choice)* | `ip-tunnel`: local address advertised in the HPAI (NAT friendly when unset) |
| `KNX_IP_MULTICAST`    | `224.0.23.12` | `ip-routing`: multicast group          |
| `KNX_IP_INDIVIDUAL_ADDR` | `15.15.250` | `ip-routing`: individual address used as the cEMI source of our routing telegrams (`A.L.D`). A router rejects `0.0.0`, so this must be a free address of the installation. |
| `KNX_IP_IFACE`        | *(default)* | `ip-routing`: interface for the group join / `IP_MULTICAST_IF` |
| `KNX_IP_NAT`          | `0`        | `ip-tunnel`: `1` = NAT mode              |
| `KNX_IP_HEARTBEAT_SEC`| `60`       | `ip-tunnel`: `CONNECTIONSTATE_REQUEST` interval (10–86400) |
| `KNX_IP_ACK_TIMEOUT_MS`| `1000`    | `ip-tunnel`: ACK timeout before retransmitting (10–60000) |
| `KNX_IP_RETRIES`      | `3`        | `ip-tunnel`: retransmissions per telegram (1–10) |
| `FCGI_SOCKET`         | *(required)* | FCGI listen socket: `:port` for TCP, or a filesystem path for a Unix socket. The backend opens it itself; a Unix socket is created with mode `0660`. A TCP port has no authentication or peer check — keep it on loopback. |
| `FCGI_WORKERS`        | `20`       | Number of pre-forked worker processes (1–256). Each worker serves one request at a time; an idle worker costs a few MB of RAM. Workers never open a bus connection of their own — they enqueue writes into the shared TX queue. Two workers are kept free for `/w` and `/l`, so at most `max(1, FCGI_WORKERS - 2)` long-polls are served at the same time. |
| `LONGPOLL_TIMEOUT_SEC`| `300`      | Max seconds to wait in long-poll `/r` |
| `KNX_TX_MAX_PER_SEC`  | `10`       | Hard upper bound on outgoing telegrams per second over **all** clients and workers (1–1000). The cache reader is the only process that sends, so this limit cannot be bypassed by a client — or by a buggy one — and it protects the bus from flooding. |
| `BASE_URL`            | *(unset)*  | When set, the login (`/l`) response includes a config object `"c"` with `"baseURL"` set to this value. Used by the web server to advertise the URL prefix under which the backend is reachable (e.g. `/proxy/visu`). |
| `SESSION_TTL_SEC`     | `1800`     | Lifetime of a session in seconds (1–604800). Sessions are independent of the request wait limit. |
| `SESSION_MAX`         | `10000`    | Concurrent sessions (1–1000000); when the store is full the oldest session is evicted, so a client that logs in again always gets a session. |
| `KNX_CACHE_CAPACITY`  | `16384`    | Distinct group addresses kept in the shared cache (4–1000000). When full, the least recently *pushed* address is evicted. |
| `ADDRESS_PREFIX`      | *(empty)*  | Default namespace for addresses without one: `ADDRESS_PREFIX=KNX` turns `1/2/3` into `KNX:1/2/3`. |
| `LOGIN_EXTRA_CONFIG`  | *(unset)*  | Raw JSON members appended to the `"c"` block of the `/l` response (e.g. `"foo":"bar"`). |
| `DEBUG_BACKEND`       | *(unset)*  | Set to `1` to enable debug logging to stderr |

At startup, the server logs the effective values of all configuration
environment variables to stdout:

```
[INFO] cometvisu-knx-fcgi 2.0.0 starting
[INFO] Configuration:
[INFO]   KNX_TRANSPORT         knxd
[INFO]   BUS                   transport=knxd socket=/run/knx
[INFO]   FCGI_SOCKET           /run/cometvisu-fcgi.sock
[INFO]   FCGI_WORKERS          9
[INFO]   LONGPOLL_TIMEOUT_SEC  300
[INFO]   KNX_TX_MAX_PER_SEC    10
[INFO]   ADDRESS_PREFIX        (not set)
[INFO]   BASE_URL              (not set)
[INFO]   LOGIN_EXTRA_CONFIG    (not set)
[INFO]   DEBUG_BACKEND         (not set)
```

### FastCGI environment variables

These are standard
[CGI variables](https://fastcgi-archives.github.io/FastCGI_Specification.html)
provided by the web server. The application reads them automatically — you do
not need to set them yourself.

| Variable          | Description                                        |
|-------------------|----------------------------------------------------|
| `REQUEST_METHOD`  | HTTP method: `GET`, `POST`, or `PUT`               |
| `REQUEST_URI`     | Full request URI, e.g. `/cgi-bin/l?u=USER&p=PASS`  |
| `QUERY_STRING`    | Query string portion, e.g. `u=USER&p=PASS&d=DEVICE`|
| `SCRIPT_NAME`     | Virtual path to the script, e.g. `/cgi-bin/l`     |
| `PATH_INFO`       | Extra path after the script name, e.g. `/l`       |
| `CONTENT_TYPE`    | MIME type of the request body (POST/PUT)           |
| `CONTENT_LENGTH`  | Size of the request body in bytes (POST/PUT)       |
| `SERVER_PROTOCOL` | Protocol string, e.g. `HTTP/1.1`                   |

> **Routing note**: The application determines which handler (`/l`, `/r`,
> `/w`) to invoke by inspecting `PATH_INFO` first. If `PATH_INFO` is empty,
> it strips `SCRIPT_NAME` from `REQUEST_URI` to derive the path. When
> `SCRIPT_NAME` itself ends with the endpoint (e.g. `/cgi-bin/l`), the last
> path component is used as the handler key. This means all common web-server
> configurations — `ScriptAlias`, `fastcgi_pass` with `SCRIPT_NAME` pointing
> to a directory or directly to the binary — work out of the box.

### Debug mode

Set `DEBUG_BACKEND=1` (or `true`/`yes`/`on`) to enable detailed debug output to
stderr. This prints every HTTP request/response and the bus traffic with
millisecond timestamps, making it easy to trace the full communication flow:

```
[2026-07-04 12:34:56.789] → HTTP REQUEST: GET /r?a=KNX:1/2/3&t=5&s=9f1c…
[2026-07-04 12:34:56.792]   ← KNXD RECV: cache_push addr=1/2/3 data=42
[2026-07-04 12:34:56.793] ← HTTP RESPONSE: 200 body={"d":{"KNX:1/2/3":"42"},"i":1}
[2026-07-04 12:35:12.101] → HTTP REQUEST: GET /w?a=KNX:1/2/3&v=8042&s=9f1c…
[2026-07-04 12:35:12.102]   → KNXD SEND: tx_queue addr=1/2/3 apdu=008042
[2026-07-04 12:35:12.102] ← HTTP RESPONSE: 200 body=
```

The bus traces (`KNXD SEND` / `KNXD RECV`) are printed by the cache reader, i.e.
the one process that owns the bus connection, and are indented under the request
that caused them where that is known. Long URIs (>500 chars) and large response
bodies (>1000 chars) are truncated with a `... (truncated, NNNN chars total)`
marker.

## Architecture

See [AGENTS.md](AGENTS.md) for the architecture, the design decisions (worker
pool, shared cache, transport seam, TX queue) and the coding rules.  Deferred
work is listed in [ROADMAP.md](ROADMAP.md).

## Security

**The backend performs no authentication of its own.**  `/r` and `/w` require a
session id (`s`) as defined by the CometVisu protocol, but the credentials sent
to `/l` are not validated — authentication is the web server's job, and the
backend must never be reachable without it.  In practice this means:

- Run the backend behind a web server that enforces authentication: a reverse
  proxy or the FastCGI host itself.  The backend must never be reachable
  directly from an untrusted network.
- `FCGI_SOCKET=<path>` creates a Unix socket with mode `0660`; the web server
  user must be in the socket's group.  `FCGI_SOCKET=:port` listens on **all**
  interfaces with no peer check — the backend logs a warning, and such a setup
  belongs on a loopback-only interface at most.
- `KNX_TRANSPORT=ip-routing` accepts any `ROUTING_INDICATION` on the multicast
  group as bus traffic: every host on that LAN can inject group values.  Run it
  on an isolated, trusted VLAN (or use `ip-tunnel` and, eventually, KNX IP
  Secure — see [ROADMAP.md](ROADMAP.md)).
- `KNX_TX_MAX_PER_SEC` (default 10) caps the outgoing telegram rate over all
clients; it is a protection against a buggy or hostile client flooding the bus,
not a substitute for supervision.
- `DEBUG_BACKEND=1` logs request URIs; the `u`/`p` values of `/l` are redacted,
but keep such logs protected anyway.

## License

See [LICENSE](LICENSE).
