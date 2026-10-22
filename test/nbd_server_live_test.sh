#!/usr/bin/env bash
# nbd_server_live_test.sh <nbdserver> <nbd_probe>: drives **our** NBD server
# with reference clients — nbdinfo, nbdcopy and qemu-io, none of which we
# wrote — and then with our own client.
#
# This is the other half of net_nbd_live_test.sh. Together they mean neither
# end of this protocol is only ever checked by the other end of it: the
# client talks to qemu-nbd, and the server talks to libnbd and QEMU.
set -euo pipefail

server="$(realpath "$1")"
probe="$(realpath "$2")"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
vols="$tmp/volumes"
img="$vols/vol.img"
port=0
pid=0

for tool in nbdinfo nbdcopy qemu-io; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

cleanup() {
    [ "$pid" -ne 0 ] && kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

# serve <extra args...>: (re)starts our server on a fresh port.
serve() {
    cleanup
    port="$(free_port)"
    "$server" -listen "127.0.0.1:$port" -volumes "$vols" "$@" &
    pid=$!
    for _ in $(seq 1 50); do
        if nbdinfo "nbd://127.0.0.1:$port/vol" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    fail "our server did not come up on port $port"
}

uri() { echo "nbd://127.0.0.1:$port/$1"; }

mkdir -p "$vols"
dd if=/dev/urandom of="$img" bs=1M count=4 status=none
dd if=/dev/urandom of="$vols/second.img" bs=64k count=1 status=none
cp "$img" "$tmp/original.raw"

serve

echo "---- nbdinfo reads the export"
nbdinfo "$(uri vol)" | tee "$tmp/info"
grep -q "export-size: 4194304" "$tmp/info" || fail "nbdinfo saw the wrong size"
grep -q "is_read_only: false" "$tmp/info" || fail "the export should be writable"
grep -qi "can_flush: true" "$tmp/info" || fail "the export should offer flush"
grep -q "block_size_preferred: 4096" "$tmp/info" || fail "the preferred block size is wrong"

echo "---- nbdinfo lists the exports"
nbdinfo --list "nbd://127.0.0.1:$port" >"$tmp/list"
grep -q 'export="vol"' "$tmp/list" || fail "vol is missing from the list"
grep -q 'export="second"' "$tmp/list" || fail "second is missing from the list"

echo "---- nbdcopy reads the whole export, byte for byte"
nbdcopy "$(uri vol)" "$tmp/copy.raw"
cmp "$img" "$tmp/copy.raw" || fail "a full read through nbdcopy differs from the file"

echo "---- nbdcopy writes the whole export"
dd if=/dev/urandom of="$tmp/new.raw" bs=1M count=4 status=none
nbdcopy "$tmp/new.raw" "$(uri vol)"
cmp "$tmp/new.raw" "$img" || fail "a full write through nbdcopy did not land in the file"

echo "---- qemu-io writes a pattern and reads it back"
qemu-io -f raw "$(uri vol)" \
    -c "write -P 0x5a 1048576 65536" \
    -c "read -P 0x5a 1048576 65536" \
    -c "flush" >"$tmp/qemu-io.log" 2>&1 || { cat "$tmp/qemu-io.log"; fail "qemu-io refused the export"; }
dd if="$img" of="$tmp/pattern.bin" bs=1 skip=1048576 count=65536 status=none
if tr -d '\132' <"$tmp/pattern.bin" | read -r -n1 _; then
    fail "the file holds bytes that are not 0x5a where qemu-io wrote"
fi

echo "---- qemu-io is refused past the end"
if qemu-io -f raw "$(uri vol)" -c "read 4194304 512" >/dev/null 2>&1; then
    fail "a read past the end of the export succeeded"
fi

echo "---- our own client against our own server"
"$probe" 127.0.0.1 "$port" vol info | grep -q "size 4194304" || fail "our client disagrees about the size"
"$probe" 127.0.0.1 "$port" vol fill 2097152 131072 0x3c
"$probe" 127.0.0.1 "$port" vol verify 2097152 131072 0x3c
if "$probe" 127.0.0.1 "$port" nope info 2>/dev/null; then
    fail "an export we do not have was served"
fi

echo "---- the default export"
cp "$tmp/original.raw" "$vols/default.img"
serve
"$probe" 127.0.0.1 "$port" "" info | grep -q "size 4194304" || fail "the empty name did not open default.img"

echo "---- a read-only server refuses every writer"
serve -read-only
nbdinfo "$(uri vol)" | grep -q "is_read_only: true" || fail "the export should be read-only"
if qemu-io -f raw "$(uri vol)" -c "write -P 0xff 0 512" >/dev/null 2>&1; then
    fail "a read-only export accepted a write from qemu-io"
fi
"$probe" 127.0.0.1 "$port" vol rofs || fail "our client could not confirm the refusal"
cmp <(dd if="$img" bs=512 count=1 status=none) <(dd if="$tmp/new.raw" bs=512 count=1 status=none) ||
    fail "the volume changed while it was read-only"

echo PASS
