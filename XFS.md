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

---

## 9. Writing the log, and the four facts that came out of xfs_logprint

A metadata change goes to the log before it goes to its place. What this
kernel does **not** do is replay one: the reader still refuses a dirty log,
`xfs_repair` remains the recovery tool, and every batch ends with an unmount
record so the log is clean again. That refusal buys a real simplification,
and it is deliberate rather than unfinished — because the log is never left
dirty, it is never a ring with a head to maintain, so each batch is written
from the log's first block.

The only metadata a data write changes is the file's recorded size, and that
is what goes through the log today: a write may reach the end of the blocks
the file already owns (a 262000-byte file owns 262144 bytes of them), and the
size it leaves behind is logged, then written in place, with the inode's own
CRC recomputed.

**Four facts, each one settled by writing records by hand into an image and
asking `xfs_logprint` about them** — the same method as the rest of this
document, and the same reason: a judge that is not ours.

| Fact | How it failed first |
|---|---|
| The log is addressed in **512-byte basic blocks**, not filesystem blocks, and `h_lsn` names one of those | Writing at a filesystem block put the unmount record eight blocks from where its header claimed. Our own reader still found it — it scans every block — but `xfs_repair` compared the head with the tail and declared the log to hold metadata changes needing replay |
| The transaction header is an **operation of its own**, after a `START` operation that carries nothing | With the header on the `START` operation, recovery skipped it and read the *next* operation as the header: "bad header magic number" |
| That header is **16 bytes**, not 12 (magic, type, tid, item count) | At 12 bytes recovery silently did not register an item, and then read the following operation as another header — the same message, from a different cause |
| **Log items are native-endian**, which is the one place this format is not big-endian | An `ilf_size` of 2 written big-endian was read as 512: "bad number of regions (512) in inode log format" |

The last one is the one worth remembering: a log item is a memory image
replayed by a kernel of the same endianness, not an on-disk record. The
logged inode core is 176 bytes at the **same offsets** as the on-disk core
with every field reversed — and its timestamps are two 32-bit words each, so
reversing one as a single 64-bit quantity swaps the seconds with the
nanoseconds.

What `xfs_logprint` now reads back from a record the board wrote, which is
what `xfs_qemu_test` asserts:

```
TRANS: tid:0x6d6b7472  #items:1
INO: cnt:2 total:2 len:56 len:176
        INODE: #regs:2   ino:0x63  flags:0x1   dsize:0
        CORE inode:
                magic:IN  mode:0x81a4  ver:3  format:2
                uid:0  gid:0  nlink:1 projid:0x00000000
                atime:1792658684  mtime:1792658684  ctime:1792658684
                size:0x3ffd4  nblks:0x40  exsize:0  nextents:1  anextents:0
```

`size:0x3ffd4` is 262100, the size the board grew the file to. Four judges
agree about that number and none of them is ours: `xfs_logprint` decodes it
out of the transaction, `xfs_logprint` calls the log `<CLEAN>`, `xfs_repair
-n` finds nothing wrong with the filesystem, and `xfs_db` reads the size back
off the inode.

**What is still missing, and what it would take.** Creating a file, growing
one past its own blocks and removing one all need the *allocator* — the
free-space btrees, the inode btrees and the AGF/AGI counters — and each of
those is another log item type beside the inode core. The log machinery is
now the part that exists; the allocator is the part that does not.

---

## 10. Creating a file, and what the group headers look like

A create changes four things, and each was measured on a filesystem
`mkfs.xfs` made rather than taken from a document:

| Structure | Where | What a create does |
|---|---|---|
| the group's inode header (`XAGI`) | sector 2 of the group's first block | one fewer free inode |
| every inode chunk (`IAB3`) | the block `agi_root` names | the inode's bit cleared, its count down |
| the chunks with a free inode (`FIB3`) | the block at `agi+328` | the same, in the same record |
| the superblock's counter | `sb_ifree` | follows the groups' |

Both trees are the same shape: a 56-byte block header, then 16-byte records
of `startino, freecount, free` — a **set** bit meaning free. A real
filesystem's first group reads `[96, 57, 0xffffffffffffff80]`: inodes 96 to
102 used, 57 of the chunk's 64 still free.

A **shortform directory** lives in its parent's inode as
`count, i8count, parent`, then entries of `namelen(1) offset(2) name
ftype(1) inumber(4)`. The offset is where the entry would sit in a
block-form directory, and those offsets are the part worth measuring: four
files came out at **96, 112, 128, 152**. The first is 64 (a data block's
header) plus sixteen bytes each for "." and "..", and each one after
advances by `(12 + namelen)` rounded up to eight — so the deltas are 16, 16,
24 for names of 1, 2 and 8 characters. The directory's recorded size is the
total length of that structure, which came to 57 for those four.

**What it will not do**, each because the alternative is a btree operation
this does not implement, and a half-done btree is worse than a refusal:
take the **last** free inode of a chunk (that record would have to be
deleted from the free-inode tree), allocate a new chunk (that needs the
block allocator), or touch a tree with more than one level or a directory
that has outgrown its inode. So it creates files in a filesystem that has
room for them, and says which of those it hit when it cannot.

**Three bugs this cost, all found by the judges rather than by reading:**

- `di_next_unlinked` must be `0xffffffff`, not the zero a cleared inode has.
  Zero means "inode 0 is next on the unlinked list" rather than "nothing
  is", and `xfs_repair` said so — *"bad next_unlinked 0x0 on inode 100"* —
  and would have cleared the inode for it.
- **A smaller batch written over a larger one leaves an orphan.** The head
  of the log is found by reading one word per basic block, so a block left
  over from a longer record still carries the cycle and still looks like
  part of the log; `xfs_repair` found one past the unmount record and
  reported a log with valuable metadata changes. The writer now clears a
  window as long as the longest record it can produce.
- Clearing that window zeroed **5632 bytes of a 4096-byte buffer**, over
  whatever static followed it, which came back as a kernel that spun
  instead of finishing. The window is now written in passes that fit.

---

## 11. The free-space btrees, and what a file costs

Two trees describe the same free extents, one ordered by **where** they are
(`AB3B`) and one by **how big** they are (`AB3C`), with 8-byte records of
`startblock, blockcount` at the same offset 56. The group's free-space
header (`XAGF`) is sector 1 of its first block, and carries the free-block
count and the largest extent.

The second ordering is the part that catches people out: **a record whose
count changes usually has to move**, because the tree it is in is sorted by
that count. Within a single leaf — which is all these trees are at this
size — moving a record is sorting the leaf, and that is what this does. A
tree with branches is refused rather than half-walked.

Both directions are implemented, because a filesystem needs both to be
usable:

- **Growing a file** asks for the block immediately after its last one.
  When that block is free the file's last extent simply gets longer, so a
  file written in order stays a single extent; only when it is not does the
  file collect another. Nine thousand bytes written into a file with no
  blocks at all comes back byte for byte, and `xfs_repair` is clean.
- **Removing a file** gives its extents back, each joined to whatever it
  abuts — free space in XFS is kept as maximal extents, and two adjacent
  free records are something `xfs_repair` will tell you about. The
  by-count tree is then rebuilt from the by-block one rather than patched,
  because after a merge it is no longer a simple function of what was
  added, and copying is both shorter and impossible to get subtly wrong.

The test that matters is the one that counts: the board creates a file,
writes into blocks it did not have, reads them back and removes it, and the
host then finds **60 free inodes where there were 60, and 114596 free
blocks where there were 114596**. Not one block or inode leaked, and the
whole filesystem still passes `xfs_repair -n`.

**The bug worth remembering** is the one that cost an hour: an inode btree's
records are keyed by the **group-relative** inode number, and a chunk does
not start at a multiple of its own size — `mkfs.xfs`'s first chunk starts
at inode 96, which is not a multiple of 64. So a chunk cannot be found by
masking an inode number down; it has to be found by **range**. Masking it
made every remove fail with EIO, which is the least informative way to be
wrong.

Still refused, and honestly: filling a hole or an unwritten extent inside a
file (the extent would have to be split), a directory that has outgrown its
inode, a leaf that would have to split, and a tree with branches.
