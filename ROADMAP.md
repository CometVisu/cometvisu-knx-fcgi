# Roadmap

Deferred and out-of-scope work.  Everything else that used to be described in
`PLAN.md` and `PLAN-KNXNETIP.md` is implemented in 2.0.0 — the architecture and
design decisions live in [AGENTS.md](AGENTS.md), the client contract and
configuration in [README.md](README.md), and the release history in
[CHANGELOG.md](CHANGELOG.md).

## Deferred

- **`/f` endpoint** — filter management (`POST`/`PUT`/`DELETE`) for batch
  address operations.  Not implemented since 1.0; CometVisu works without it.
  Would need a definition of the request/response shape first.
- **KNX IP Secure** — secure wrapper (Curve25519, AES-128-CCM) in front of the
  KNXnet/IP frame codec.  Explicitly out of scope for 2.0.0: deployments that
  need it stay on the `knxd` transport.  The frame codec in `src/knxnetip/` is
  transport-neutral, so a wrapper can be added without touching any `BusLink`.
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
