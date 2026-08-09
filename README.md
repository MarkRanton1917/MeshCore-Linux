# MeshCore

A MeshCore room server for Linux: a LoRa mesh node that keeps a small
noticeboard, forwards other people's packets, and can be run as a whole network
of processes on one laptop.

The stack is written so that every layer can be built and tested without the one
above or below it. Packets are parsed without crypto, crypto knows nothing about
packets, routing knows nothing about posts, and nothing except `platform` makes a
system call. The wiring is paid for once, in [main.cpp](src/main.cpp).

## Modules

Dependencies point downward only; no module names the one above it.

| Module | Does | Depends on |
| --- | --- | --- |
| [packet](src/packet/) | Frame layout: parse, serialize, encode/decode payloads. No heap, no state. | — |
| [crypto/core](src/crypto/core.h) | Primitives and key material: Ed25519, X25519, SHA-256, HMAC, AES-128. | — |
| [crypto/protocol](src/crypto/protocol.h) | MeshCore-specific constructions built from those primitives. | core, packet |
| [identity](src/identity/) | Our keypair and the contacts learned from adverts, plus a shared-secret cache. | core, packet |
| [routing](src/routing/) | Flood and direct routing, deduplication, path learning, acks and retries. | identity, packet, protocol |
| [room](src/room/) | The noticeboard: posts, clients, logins, access control, per-client sync. | identity, routing |
| [radio](src/radio/) | Airtime, duty cycle, TX queue, and the drivers. The only module that would know about SPI. | platform |
| [platform](src/platform/) | Clock, file store, config, logging — the only system calls in the tree. | — |
| [telemetry](src/telemetry/) | Event bus and counters. Switchable off with a flag; nothing else notices. | platform |

`routing` never reads a clock — time arrives through `tick()`. `room` never sees
a byte off the air. `routing` and `room` publish telemetry events to a bus and
never call telemetry directly.

## Protocol notes

A frame is `header(1) [transport_codes(4)] path_length(1) path(0..64) payload(0..184)`,
255 bytes maximum. The header carries the route type in the low two bits and the
payload type in the next four; `path_length` carries the hop count in six bits and
the per-hop hash size in the other two.

- Adverts are signed with Ed25519 over `public_key || timestamp || appdata` — the
  signature sits between those fields on air, so it is not a contiguous span.
- Direct payloads are sealed with AES-128-ECB, PKCS#7, and HMAC-SHA256 truncated
  to two bytes. The AES key is the head of the X25519 shared secret, taken raw.
  There is no IV and no nonce: replay defence belongs to the layer above.
- Deduplication is SHA-256 over the header and payload, with the path left out
  because it grows on every hop.
- A one-byte node hash is ambiguous by design, so a received packet is matched
  against every contact sharing that byte and settled by whose MAC checks out.

## Build

Requirements: CMake ≥ 3.16, a C++20 compiler, `pkg-config`, and libsodium
(`libsodium-dev` on Debian/Ubuntu). `lib/json` (nlohmann/json) and `lib/radiolib`
are vendored in-tree — no submodule init needed.

```sh
cmake -S . -B build
cmake --build build -j
```

This produces `build/meshcore-node` and the `meshcore` library the tests link
against. `-DSANITIZE=ON` adds AddressSanitizer and UndefinedBehaviorSanitizer.

## Run

```sh
./build/meshcore-node [config.json]     # defaults to ./meshcore.json
```

The node refuses to start rather than doing something quietly wrong: an
unreadable identity is never replaced with a fresh one (the network would see a
stranger and nobody would notice), and an unsynchronised wall clock is fatal
because a node advertising itself dated 1970 corrupts its neighbours' records.
`SIGINT`/`SIGTERM` save contacts and room state on the way out.

Configuration is [meshcore.json](meshcore.json); every key has a default:

| Key | Meaning |
| --- | --- |
| `node.dir` | Where the identity, contacts and room state live |
| `node.name` | Name advertised to the network |
| `node.flush_ms` | How often state is written (lazily — a write per advert would wear out the card) |
| `node.advert_ms` | Advert interval |
| `log.level` | `error`, `warn`, `info`, `debug` |
| `radio.driver` | `udp` or `virtual` |
| `radio.udp_bind`, `radio.udp_port` | Local socket; port `0` lets the kernel choose |
| `radio.udp_peers` | `"host:port"` each — who hears this node |
| `radio.udp_group`, `radio.udp_group_port` | Optional multicast, so a LAN needs no peer list |
| `radio.frequency`, `radio.spreading_factor`, `radio.bandwidth`, `radio.coding_rate` | LoRa parameters; these must match the network bit for bit |
| `radio.duty_cycle` | Percent per sliding hour (10 on 868 MHz in Europe) |
| `room.admin_password`, `room.guest_password` | Empty means that role cannot log in |
| `room.anonymous_read` | Let strangers read without a password |
| `telemetry.enabled`, `telemetry.queue`, `telemetry.report_ms` | Event ring and reporting interval |

### Radio drivers

`udp` carries frames over UDP between processes on one machine or hosts on a
LAN, keeping the real airtime and duty-cycle accounting. Each node's peer list
is the visibility matrix, so a chain A–B–C is three configs rather than a special
mode. `virtual` is the in-process shared medium the tests use. There is no
SX1262 driver yet; RadioLib is vendored for it.

### A network on one machine

[network.sh](network.sh) generates a config and data directory per node and runs
them all, labelling every log line with the node it came from. Ctrl-C stops the
network with `SIGTERM`, so state is saved.

```sh
./network.sh                  # three nodes, full mesh
./network.sh -n 5 -t chain    # five nodes in a line, 1-2-3-4-5
./network.sh -n 4 -t star -v  # hub plus three leaves, debug logging
./network.sh -c               # wipe ./run first, so every node starts a stranger
```

## Tests

Eight suites, one per module, on a [minimal harness](test/check.h) with no
framework and no dependencies. The virtual radio and the fake clock are what
make them fast: a multi-node routing test runs in one process, and a timeout test
does not take twelve seconds.

```sh
cd build && ctest --output-on-failure
```

Crypto is pinned to published vectors rather than to itself, so a rewrite that
changes behaviour fails instead of agreeing with the new bug.

## Formatting

`.clang-format` is authoritative. [format.sh](format.sh) rewrites the tree;
[check-format.sh](check-format.sh) fails without touching it.

## License

MIT — see [LICENSE](LICENSE).
