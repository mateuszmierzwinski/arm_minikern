# XFS on minikern

How this kernel reads XFS, what it refuses, and the on-disk facts it was
built from — **measured** against filesystems made by Linux's own
`mkfs.xfs` 6.15 rather than taken from a header. Where a number here
disagrees with the code, the code is wrong.

## 1. Why XFS

Because **Linux can check our work**, which no format of our own invention
could offer:

| Tool | What it settles |
|---|---|
| `mkfs.xfs` | makes the test images, including populated ones, without root |
| `xfs_repair -n` | says whether what we wrote is still a valid filesystem |
| `xfs_db` | prints any structure we doubt, and verifies its checksum |

The rule that follows, and that CI enforces: **`xfs_repair -n` is clean
after every test.** A filesystem implementation that only its own reader
approves of has not been tested.

### Populating an image without root

Mounting needs root; `mkfs.xfs -p <protofile>` does not. A protofile names
the files to copy in, so a test can build a filesystem with directories of
any size and then read it back:

```
DUMMY
0 0
d--755 0 0
hello.txt ---644 0 0 /path/on/the/host
subdir d--755 0 0
nested.txt ---644 0 0 /path/on/the/host
$
$
```

Two constraints met the hard way: **xfsprogs 6.15 refuses to make a
filesystem smaller than 300 MB**, so test images are 512 MiB sparse files
(about 65 MiB of real disk); and `mkfs.xfs` defaults to features this reader
refuses, so the test images are made with the set in §4.

## 2. The facts, as measured

Every offset below was read out of a real image and confirmed by decoding a
structure whose contents were known independently.

### The superblock (byte 0, and the start of each allocation group)

Magic `0x58465342` ("XFSB"). The fields this reader uses are listed in
`src/core/xfs/xfs_format.h`; the ones worth calling out:

| Field | In the test image | Note |
|---|---|---|
| `blocksize` | 4096 | `1 << blocklog`, checked |
| `sectsize` | 512 | **the checksum's extent** — see below |
| `inodesize` | 512 | mkfs's default for v5; 256 also accepted |
| `inopblock` | 8 | must equal `blocksize / inodesize` and `1 << inopblog` |
| `agblocks` / `agcount` | 32768 / 4 | `agblocks <= 1 << agblklog` |
| `agblklog` | 15 | how many bits of an inode number are the block |
| `rootino` | 96 | where a path walk starts |
| `versionnum` | `0xb4a5` | the low nibble is the version: 5 |

**The superblock's CRC32C covers its whole sector, not its fields.** This
cost a debugging session: summing the 264 bytes the reader knows about gave
`0x6e1f030b`, summing 512 gave `0x08e999ae`, and the stored value was
`0x08e999ae`. An inode's checksum covers `inodesize`, and a directory
block's covers `blocksize` — in each case the whole structure, with the
checksum field itself counted as zero, stored little-endian.

### Inode numbers are addresses

```
agno  = ino >> (agblklog + inopblog)
agbno = (ino >> inopblog) & ((1 << agblklog) - 1)
slot  =  ino & ((1 << inopblog) - 1)
block = agno * agblocks + agbno          /* not agno << agblklog */
```

Confirmed: inode 100, with `agblklog` 15 and `inopblog` 3, is group 0,
block 12, slot 4 — byte 51200 of the image, where its magic duly sits. The
last line is the one that is easy to get wrong: `agblocks` need not be a
power of two, so the block number is a multiplication, not a shift.

An inode carries **its own number** at offset 152 and the filesystem's UUID
at 160. Both are checked: a block that holds an inode is not proof that it
holds *this* inode.

### Extent records

Sixteen bytes of packed bit fields:

```
bit    127   unwritten (reads as zeroes)
bits 73-126  logical block offset within the file   (54 bits)
bits 21-72   filesystem block                        (52 bits)
bits  0-20   block count                             (21 bits)
```

Confirmed against a 200000-byte file whose record was
`00000000 00000000 00000000 02800031`: offset 0, block 20, count 49 — which
is what `xfs_db`'s `bmap` reported for it.

### Directories

Three shapes, and this reader walks all of them by reading only the data:

- **Shortform**, inside the inode: `count(u8) i8count(u8) parent(4 or 8)`,
  then entries of `namelen(u8) offset(u16) name[] ftype(u8) ino(4 or 8)`.
  Note the offset field sits *before* the name. "." and ".." are not stored
  — the parent is in the header — so a listing synthesizes them.
- **One block** (magic `XDB3`): a 64-byte header, then entries, and at the
  **end of the same block** the hash entries and a tail of
  `count(u32) stale(u32)`. The entries therefore stop at
  `blocksize - 8 - count * 8`, and a reader that walks to the end of the
  block parses the hash array as entries.
- **Several blocks** (magic `XDD3`): the same header and entries, with
  nothing after them. The hash structures live at logical offset 32 GiB and
  above, which is why a listing can ignore them entirely.

An entry is `ino(u64) namelen(u8) name[] ftype(u8) tag(u16)` rounded up to
eight bytes, with the tag at the end holding the entry's own offset. Unused
space starts with `0xffff` and carries its own length. Every directory block
also names its **owner**, which is checked against the inode being listed.

### The log, and what "clean" means

A freshly made log is **not** zeroed: `mkfs.xfs` writes exactly one record,
and that record is the unmount record. Measured, at the log's first bytes:

```
feedbabe  magic
00000001  cycle
00000002  version
00000200  length (512)
...
00000001  one operation
```

and its single operation, in the next 512-byte block:

```
00000001  tid
00000008  length
aa        client: the filesystem itself
20        flags: XLOG_UNMOUNT_TRANS   ← the evidence
6e55      the payload's magic, in the **host's** byte order
```

That payload is written in native byte order, so both orders are accepted
and the flag is what the check relies on.

**The reader refuses a dirty log.** Finding the newest record cheaply
matters — a 512 MiB filesystem's log is 64 MiB, and a linear scan over a
network block device would be thousands of round trips — so the head is
found by **bisection on the cycle number**, which the first word of every
512-byte block carries. About seventeen reads, then a short backward scan
for the newest record header, which must be an unmount. A log that was
never written has nothing to replay and is clean.

## 3. What is supported

| | |
|---|---|
| **Version** | v5 only (CRC metadata) |
| **Inodes** | 256 or 512 bytes, classic layout (32-bit extent counts) |
| **Forks** | `LOCAL` (inside the inode) and `EXTENTS`; **`BTREE` is refused** |
| **Directories** | shortform, one block, many blocks — by data, so a lookup is a linear scan |
| **Files** | read, including holes and unwritten extents (both read as zeroes) |
| **Writes** | **not yet** — P9.4, and the log comes first |
| **Formatting** | yes, from user space: `userland/cmd/mkfsxfs` (§7) |
| **Mounting** | `mount("/dev/nbd0", "/mnt", "xfs", MS_RDONLY, "")`, one mount, read-only |

Every structure read is verified: its magic, its CRC32C, the filesystem's
UUID, and the field by which it identifies itself (an inode knows its
number, a directory block knows its owner). A structure that fails is an
I/O error, never data.

## 4. What is refused, and why that is the point

A reader that half-understands a filesystem is a reader that corrupts it.
So the feature check is a **whitelist**: a flag nobody here has heard of
stops the mount.

| Refused | Because |
|---|---|
| `reflink` | extents shared between files; a write would have to break the sharing |
| `rmapbt` | a reverse-mapping btree we would have to maintain |
| `spinodes` | sparse inode chunks change inode allocation |
| `nrext64` | 64-bit extent counts are **a different inode layout**, and mkfs.xfs turns this on by default |
| `needsrepair` | the filesystem is known to be broken |
| any log-incompat flag | the log holds records we cannot read |
| a dirty log | the metadata on disk is older than the log |
| `BTREE` forks | a file with more extents than fit in its inode |
| v4, or `inprogress` | not self-describing, or never finished |

An image for this reader is made with:

```bash
mkfs.xfs -m crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0 \
         -i sparse=0,size=512,nrext64=0 -n ftype=1 -b size=4096 <image>
```

`finobt` and `inobtcount` are allowed because this reader never reads the
inode btrees: a path walk starts at the root inode from the superblock and
follows directory entries, so free-inode bookkeeping is nothing to it. That
changes when writing arrives.

## 5. How it is tested

Both halves, because either alone would be self-confirming:

- **`xfs_live_test`** — filesystems made by `mkfs.xfs`, with the files that
  went into them to compare against: geometry, all three directory shapes
  (a 42-entry block directory and a 502-entry leaf one), files read back
  byte for byte, and then every refusal in §4 provoked deliberately. The
  image's md5 is checked afterwards: a read-only mount that changed a byte
  fails the test.
- **`xfs_test`** — synthetic filesystems with *valid checksums*, damaged one
  field at a time: 21 cases plus a 3000-round mutation fuzz, with the block
  device underneath counting any read outside itself (there are none). Also
  run under ASan and UBSan (`xfs_sanitized_test`).

The commands worth knowing when a structure is in doubt:

```bash
xfs_db -r -c "sb 0" -c "p" image            # the superblock, checksum verified
xfs_db -r -c "inode 96" -c "p" -c "bmap" image
xfs_repair -n image                          # the judge
```

## 6. The block device underneath

The volume is a file on a server, reached over **NBD** (`src/core/nbd.h`,
the newstyle fixed handshake with `NBD_OPT_GO` and simple replies), with a
buffer cache over it (`src/core/bcache.h`) because every structure above is
a round trip. NBD rather than a protocol of our own for the same reason as
XFS: `qemu-nbd` serves, and `nbdinfo`, `nbdcopy` and `qemu-io` consume, so
both ends of ours are tested against implementations that are not.

The server is in `server/cmd/nbdserver`, and `/dev/nbd0` is how a program on
the board reaches the volume.

## 7. Formatting, from user space

`mkfs` is a Go program (`userland/cmd/mkfsxfs`, over
`userland/internal/xfsmk`), not kernel code. Formatting is a hundred packed
fields that must come out exactly right, Go is a far better place to get
that right than a freestanding kernel, and **`xfs_repair` is a better judge
of the result than any test of ours**. The same package runs on the build
machine against a file and on the board against `/dev/nbd0`, because the
device it writes to is a port.

What it makes is the simplest filesystem the reader supports, in the shape
`mkfs.xfs` makes it — which matters, because `xfs_repair` compares a
filesystem against what mkfs *would* have done and says so when it differs:

```
group 0:  0-3 headers and btree roots, 4-7 spare, 8-15 the inode chunk,
          then free to the end
group 1:  0-3 headers and btree roots, 4.. the log, then free
```

Two groups minimum (`mkfs.xfs` refuses one: "must have at least two
superblocks for redundancy"), one inode chunk of 64 inodes with three used,
one or two free extents per group, and no finobt — there is no free-inode
btree to keep because nothing here allocates an inode yet.

Three numbers came out of xfs_repair's complaints rather than any document:

- **`sb_inoalignmt` is the inode cluster in blocks** — 8 KiB scaled by the
  inode size, so four blocks for 512-byte inodes. Get it wrong and the
  superblock is refused outright: "inconsistent inode alignment value".
- **The inode chunk goes at block 8 of group 0**, after the four blocks XFS
  keeps in reserve for a btree split. Put it at block 4 and the root inode
  is "valid but in unaligned location (expected 131136)".
- **Every inode needs an attribute format of 2**, even with no attributes: a
  zero there means "a device", and xfs_repair says so about every inode.

And two came out of comparing our log record with mkfs's, byte by byte: the
record header's `h_fmt`, UUID and size live at 300, 304 and 320 — after a
**64-entry** cycle-data array, not the 56 I had assumed — and the first word
of every block after the header is replaced by the cycle number, with the
word it displaced kept in that array. Until both were right, `xfs_repair`
read our log as "totally zeroed" rather than as clean.

The result, on a 512 MiB volume, is the same geometry `mkfs.xfs` produces to
the block: two groups of 65536, root inode 64, 114672 free blocks.

## 8. What proves it

```bash
bazel test //:xfs_live_test        # the reader, on real images
bazel test //:mkfsxfs_live_test    # the formatter, judged by xfs_repair
bazel test //:xfs_qemu_test        # the board does the lot
```

The last one is the whole path: a Go program on the board formats a volume
that is a file on a server — over this kernel's own TCP, its own NBD client
and its own buffer cache — asks the kernel to mount it, reads it back, and
then the host runs `xfs_repair` on that same file and finds nothing to say.
