# minikern netboot — the MKNB protocol, version 1

How a minikern board finds a boot server and fetches its root filesystem.
Both sides are ours: the client is in `src/core/net/netboot.c` (pure, host-tested)
and `src/kernel/netboot.c` (wiring); the server is
`//src/go/tools/mknbserver` (Go, in this monorepo).

It is deliberately small: **UDP only**, no TCP, no multicast, no broadcast, no
retransmission timers beyond a plain window re-request. The board has a DHCP lease
and nothing else when it starts.

---

## 1. Discovery: a unicast sweep

Multicast and broadcast are blocked on many networks, so the board does **not**
rely on either. It sweeps the subnet it was given, one unicast datagram per
address, and listens for a reply that carries our magic:

```
probe the gateway alone for MKNB_GATEWAY_GRACE_MS (400 ms)
then, for each remaining host address H in the lease's subnet:
    send DISCOVER to H:7634 from our ephemeral port
the first valid OFFER that arrives names the server
```

- **The gateway gets a head start of its own.** On a home LAN (and under QEMU's
  slirp, where the host is `10.0.2.2`) the server is most often the gateway, so it
  is probed alone for the first 400 ms. That also gives its ARP resolution time to
  finish, and it stops a host further into the sweep from winning the race.
- **A probe goes out in a broadcast frame when the address's MAC is unknown.** The
  IP destination is still the one host being probed, and every other host drops it
  exactly as it would drop a misdelivered unicast. Resolving 254 addresses by ARP
  first would cost an exchange each, and only one packet can wait for ARP at a time.
  An address already in the ARP cache (the gateway, after the boot-time ping) is
  probed as an ordinary unicast.
- The sweep is paced: `MKNB_SWEEP_BURST` (16) datagrams per 10 ms tick, so a /24
  takes about 160 ms and the EMAC's transmit ring is never overrun.
- It is **bounded**: a subnet larger than `MKNB_SWEEP_MAX` (1024) addresses is not
  swept beyond that many hosts. On a /16 the sweep would otherwise take 65534
  datagrams; the gateway and the first 1023 hosts are tried, and if the server is
  elsewhere the operator passes `minikern.netboot=<ip>` instead.
- Replies are accepted **only** from the port we sent to, only while the client is
  discovering, and only with our transaction id. The first valid OFFER wins and
  fixes the server address for the rest of the session.

## 2. Transfer: a windowed block fetch

```
client → OFFER accepted → READ(base, count) ──▶ server
server → DATA(base) … DATA(base+count-1) ──▶ client
client marks each block; when the window is full it asks for the next
on silence (120 ms) it re-asks for the lowest block still missing
when every block is in: verify SHA-256, then mount
```

- Blocks are `MKNB_BLOCK` (1024) bytes; the last one is short.
- The window is `MKNB_WINDOW` (16) blocks, so 16 KiB is in flight. The size is set
  by the **client's receive ring**, not by the link: the server sends a window as one
  burst, and a window larger than the ring (the H3's EMAC holds 64 frames) overruns
  it and most of the burst is lost. Measured under QEMU: a 64-block window delivered
  33 of 576 blocks sent, while 16 blocks delivers every one.
- A window counts as complete when every block in it has arrived, in any order, and
  the next window is asked for immediately; the stall timer only covers loss.
- A block is written once; duplicates are ignored. There is no ordering requirement.
- The whole transfer has a deadline (`MKNB_DEADLINE_MS`, 30 s). After
  `MKNB_STALL_LIMIT` (8) windows that bring nothing, the server is **abandoned** and
  the sweep resumes: a rogue that offers an image and then delivers none of it, or an
  address a NAT invented, must not be able to spend the whole boot. Any block that
  arrives resets that count, so a merely lossy link keeps going.

## 3. Messages

Every datagram starts with the same 16-byte header, **big-endian** (network order,
as every other field here):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `"MKNB"` = `0x4d4b4e42` |
| 4 | 1 | version | 1; anything else is dropped |
| 5 | 1 | type | below |
| 6 | 2 | reserved | sent 0, ignored on receipt |
| 8 | 4 | xid | the client's session id; echoed by the server |
| 12 | 4 | arg | per type |

| Type | Name | Direction | arg | Body |
|---|---|---|---|---|
| 1 | DISCOVER | client → server | 0 | 6-byte MAC, 16-byte board id (the chip's unique id) |
| 2 | OFFER | server → client | image length in bytes | 2-byte block size, 2-byte reserved, 32-byte SHA-256 of the image, 4-byte session token |
| 3 | READ | client → server | first block index | 2-byte block count, 2-byte reserved, 4-byte session token |
| 4 | DATA | server → client | block index | the block's bytes (1–1024) |
| 5 | DONE | client → server | 0 | empty; a courtesy so the server can log a success |
| 6 | ERROR | server → client | reason code | optional UTF-8 text, at most 64 bytes, for the log |

Reason codes: 1 no image for this board, 2 the image changed mid-transfer,
3 malformed request, 4 the server is busy.

**The session token** is chosen by the server, sent only in the OFFER, and echoed by
the client in every READ. It is what stops the server being a UDP amplifier: a READ
is 24 bytes and the window it asks for is 64 KiB, so serving blocks to whoever asks
would reflect traffic at roughly 3000 times its size at any address an attacker
cares to put in the source field. Because the OFFER goes to the address that probed,
an off-path attacker never sees the token, and a READ without it is answered with
nothing. The token is 32 random bits per session and is not a secret beyond that:
it proves return reachability, not identity.

The server listens on UDP **7634**. The client uses an ephemeral port and keeps it
for the whole session.

## 4. What the client refuses

Everything here has a test in `net_test.c` (`test_netboot_*`):

- a header whose magic, version, type or xid is wrong, or that is short;
- an OFFER that arrives from an address or port we did not probe, or in any state
  but discovering, or a second OFFER after one was taken;
- an OFFER shorter than its body, whose image is larger than the buffer the kernel
  reserved, is zero length, or whose block size is not exactly `MKNB_BLOCK`;
- a DATA block whose index is past the image, whose payload is the wrong length
  for its position (only the last block may be short), or that is not from the
  chosen server — no datagram can write outside the image buffer;
- anything at all once the session has ended, either way;
- an image whose SHA-256 does not match the OFFER's. The image is **executable
  content from the network**, so it is verified before anything is mounted.

## 5. Security: what this does and does not give you

The digest in the OFFER proves the **transfer** was not corrupted or spliced. It
does **not** prove the server is yours: a machine on the same LAN that answers the
sweep first can offer its own image with a matching digest, and the board would run
it. On an untrusted network the protocol is therefore **trust-on-first-reply**,
which is the same posture as PXE and TFTP netboot, and not good enough for a board
that matters.

The fix is an operator-held key: the OFFER carries an Ed25519 signature over
(xid, length, digest) and the kernel ships the public key. That is TASKS.md P5.5,
and until it is done netboot is for a network you control. `minikern.netboot=<ip>`
narrows the exposure meanwhile, because then no sweep happens and only that address
is believed.
