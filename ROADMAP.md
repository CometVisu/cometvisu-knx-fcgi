# Roadmap

Deferred and out-of-scope work.  The 2.0.0 feature set — the fork-based worker
pool, the shared caches and TX queue, the transport seam and the direct
KNXnet/IP transports — is implemented and documented elsewhere:

- architecture and design decisions: [AGENTS.md](AGENTS.md)
- build, configuration and implementation notes: [README.md](README.md)
- client contract: [Protocol](https://github.com/CometVisu/CometVisu/wiki/Protocol)
  (English) / [Protokoll](https://github.com/CometVisu/CometVisu/wiki/Protokoll)
  (German) in the CometVisu wiki
- release history: [CHANGELOG.md](CHANGELOG.md)

## Deferred

- **`/f` endpoint** — filter management (`POST`/`PUT`/`DELETE`) for batch
  address operations.  The protocol defines the request shape (see the wiki),
  but CometVisu works without filters; the backend answers `404` for `/f`.
- **KNX IP Secure** — secure wrapper (Curve25519, AES-128-CCM) in front of the
  KNXnet/IP frame codec.  Explicitly out of scope for 2.0.0: deployments that
  need it stay on the `knxd` transport.  The frame codec in `src/knxnetip/` is
  transport-neutral, so a wrapper can be added without touching any `BusLink`.
- **Client support for the neutral outage key** — `/r` and `/w` report the
  bus outage as `"bus"` for the KNXnet/IP transports, but the CometVisu client
  (`cv.io.Client.getKnxdStatus`) currently only recognises `"knxd"`.  Until the
  client accepts both keys, an outage on `ip-tunnel`/`ip-routing` is not shown
  to the user as a bus outage.  Either the client learns `bus` (preferred, the
  wiki should define the neutral key) or the backend emits `knxd` for every
  transport.
- **Interop runs against real hardware** — a real interface (MDT, Gira,
  Siemens …) is still a manual checklist.  Everything a KNXnet/IP peer does on
  the wire is automated now: `tests/e2e/test_ip_routing_blackbox.cpp` and
  `tests/e2e/test_ip_tunnel_blackbox.cpp` drive a real `knxd` as the KNXnet/IP
  router and tunnel server, inject with `knxtool` and monitor the multicast
  backbone.  Those suites found and now guard the service codes, the CRI length
  byte, the ACK layout, the routing cEMI type and the keep-alive.

## Not planned

- IPv6 / KNXnet/IP v2 — the HPAI codec keeps the protocol byte so it could be
  added later without breaking the frame layout.
- Device management (0x0310) and bus monitoring (0x80 layer).
