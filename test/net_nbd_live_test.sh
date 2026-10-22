#!/usr/bin/env bash
# net_nbd_live_test.sh <nbd_probe>: drives the kernel's own NBD client
# (core/nbd.c, through test/nbd_probe.c) against **qemu-nbd** — a reference
# server nobody here wrote. The fake server in nbd_test.c can only prove the
# client is consistent with our idea of NBD; this proves it speaks NBD.
#
# Each case ends by comparing the server's image file on disk with what the
# client believes it wrote, so a write that lands at the wrong offset fails
# here rather than becoming a corrupt filesystem later.
set -euo pipefail

probe="$(realpath "$1")"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
img="$tmp/vol.raw"
port=0
pid=0

command -v qemu-nbd >/dev/null || { echo "qemu-nbd is not installed" >&2; exit 1; }

cleanup() {
    [ "$pid" -ne 0 ] && kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# A free port: bind one, note it, let it go. The race with another process is
# not worth a lock file in a test that runs for two seconds.
free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

# serve <qemu-nbd args...>: (re)starts the server on a fresh port.
serve() {
    cleanup
    port="$(free_port)"
    qemu-nbd --persistent --format=raw --export-name=vol --bind=127.0.0.1 --port="$port" "$@" "$img" &
    pid=$!
    for _ in $(seq 1 50); do
        if "$probe" 127.0.0.1 "$port" vol info >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    fail "qemu-nbd did not come up on port $port"
}

run() { "$probe" 127.0.0.1 "$port" vol "$@"; }

# ── a 4 MiB volume of known noise ───────────────────────────────────────────
dd if=/dev/urandom of="$img" bs=1M count=4 status=none
cp "$img" "$tmp/original.raw"

serve
echo "---- info"
run info | tee "$tmp/info"
grep -q "size 4194304" "$tmp/info" || fail "the client read the wrong size"
grep -q "flush yes" "$tmp/info" || fail "the export should offer flush: $(cat "$tmp/info")"
grep -q "ro no" "$tmp/info" || fail "the export should be writable: $(cat "$tmp/info")"

echo "---- read matches the file on disk"
run read 0 65536 "$tmp/head.bin"
dd if="$img" of="$tmp/head.expected" bs=1 count=65536 status=none
cmp "$tmp/head.bin" "$tmp/head.expected" || fail "the first 64 KiB differ"

# An offset that is not block-aligned, spanning a boundary, and a length
# larger than one request (NBD_MAX_IO), so the client's own chunking is used.
echo "---- an unaligned read larger than one request"
run read 1234567 200000 "$tmp/mid.bin"
dd if="$img" of="$tmp/mid.expected" bs=1 skip=1234567 count=200000 status=none
cmp "$tmp/mid.bin" "$tmp/mid.expected" || fail "the unaligned span differs"

echo "---- write, and the server's file really changed"
run fill 1048576 131072 0xa5
run verify 1048576 131072 0xa5
dd if="$img" of="$tmp/filled.bin" bs=1 skip=1048576 count=131072 status=none
if tr -d '\245' <"$tmp/filled.bin" | read -r -n1 _; then
    fail "the image holds bytes that are not 0xa5 where we filled it"
fi
# Nothing outside the written range moved.
dd if="$img" of="$tmp/before.bin" bs=1 count=1048576 status=none
dd if="$tmp/original.raw" of="$tmp/before.expected" bs=1 count=1048576 status=none
cmp "$tmp/before.bin" "$tmp/before.expected" || fail "a write damaged the bytes before it"
dd if="$img" of="$tmp/after.bin" bs=1 skip=1179648 status=none
dd if="$tmp/original.raw" of="$tmp/after.expected" bs=1 skip=1179648 status=none
cmp "$tmp/after.bin" "$tmp/after.expected" || fail "a write damaged the bytes after it"

echo "---- writing a file through, then reading it back"
head -c 100000 /dev/urandom >"$tmp/payload.bin"
run write 3000000 "$tmp/payload.bin"
run read 3000000 100000 "$tmp/payload.back"
cmp "$tmp/payload.bin" "$tmp/payload.back" || fail "what came back is not what went in"

echo "---- the last byte, and one past it"
run read 4194303 1 "$tmp/last.bin"
if run read 4194304 1 "$tmp/past.bin" 2>/dev/null; then
    fail "a read past the end of the export succeeded"
fi

echo "---- a read-only export refuses writes"
serve --read-only
run rofs || fail "a read-only export did not refuse a write"
run read 0 4096 "$tmp/ro.bin"   # reading it still works

echo PASS
