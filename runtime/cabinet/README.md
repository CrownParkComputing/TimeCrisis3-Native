# Experimental TC3 cabinet transport

The implementation is gated by `TC3_CABINET_ROLE=left|right` and
`TC3_CABINET_DIR=/absolute/private/directory`. Leave these unset for the normal
single-cabinet baseline. Each process owns independent guest state and a Unix
datagram endpoint; an absent peer is a transport error, never a fabricated frame.

`bridge.cpp` substitutes the original `ilsocket` Connect/Send/SendTo transport
boundaries, gives each virtual cabinet its own EUI64/node ID, and polls incoming
packets at the original ILSOCK receive wait. The original IOP creates socket
objects, allocates buffers, binds ports, runs the receive queues and ILDRV RPC
workers, and sends SIF replies. All host socket operations are nonblocking.

When the checked receive loop finds no packet, the bridge redirects its guest
WaitEventFlag call to the registered guest DelayThread(500us). That real kernel
function yields to other IOP threads and returns to the original receive loop.
The return code is checked by the original loop; its result-mask word is never
read. The bridge is restricted to the known return address, arguments and
socket event ID, so unrelated kernel waits follow the original implementation.

Original TST3 ILSOCK identifies its local EUI64 as two byte-swapped words; the
bridge exposes distinct virtual identities and preserves its 20-byte packet
layout exactly. Its guest queue is an eight-entry ring with seven usable slots.
Only verified memory ranges and checked queue indices are written.

The checked module is verified by its export version, selected relocated export
addresses and receive-loop instructions. The compile-time byte identity and
complete source evidence are retained privately in `.local/link-analysis/evidence.json`;
original firmware and decompiler output are not included in the source tree.

This is an experimental transport implementation. Live validation observed the
original LINK ON 2 status and distinct solo linked partner cameras. Codec/queue
tests alone do not establish those runtime results, full stage progression or
original freeze resolution. Soft reset and savestate
lifecycle still require runtime verification. The transport does not provide a
second-screen compositor or mix audio.

Validation commands, from the project root:

```
c++ -std=c++20 -O2 -Wall -Wextra -Werror -I. tests/cabinet_link_test.cpp -o .local/cabinet-link-test
.local/cabinet-link-test
c++ -std=c++20 -O2 -Wall -Wextra -Werror -I. tests/cabinet_guest_queue_test.cpp -o .local/cabinet-guest-queue-test
.local/cabinet-guest-queue-test
```

The first test needs permission to bind Unix sockets. It exchanges real binary
packets across two processes, verifies exact envelope bytes and rejects oversized
or truncated frames. It also checks absent/disconnected peers and duplicate
endpoint ownership. The guest queue test verifies port/GUID filtering, payload
placement, deferred wake metadata, and full/exhausted/corrupt queue behavior.

## Additional checked module revision

The shared bridge also recognizes the Ridge Racer V RRV3/A ILSOCK 2.2 export and
receive-loop profile. Its globals are shifted by 0xa0 and its wait return is
0x2240. Queue descriptor and wire layout are preserved; receive maximum remains
the value set by that original guest (288 bytes for Ridge). A separate profile
regression rejects cross-revision guards and tests both receive layouts. This
recognition does not establish linked Ridge races; title runtime proof is kept
in the separate RidgeRacerV-Native project. TC3's offsets and behavior remain
covered by the original receive-queue regression.
