#!/usr/bin/env bash
# net_sanitized_test.sh <sources...>: builds the network tests with clang
# under AddressSanitizer and UndefinedBehaviorSanitizer and runs each. The
# stack and the netboot client read hostile input, so an out-of-bounds read
# that happens not to crash must still fail a test. clang's compiler-rt
# provides the runtimes (Fedora's gcc libasan is not installed here), hence
# a script rather than a cc_test.
set -euo pipefail
command -v clang >/dev/null || { echo "clang is not installed" >&2; exit 1; }
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

# The include roots are <root>/src and <root>/test, and the sources arrive as
# runfiles paths, so the root is whatever prefix sits in front of them — which
# is nothing at all when the project is the workspace root. Deriving it by
# walking up to a directory named "minikern" looked simpler and was wrong: in a
# tree where no parent carries that name, dirname reaches / and stays there.
case "$1" in
*/src/*) root="${1%%/src/*}" ;;
*/test/*) root="${1%%/test/*}" ;;
src/* | test/*) root="." ;;
*) echo "cannot tell the project root from $1" >&2; exit 1 ;;
esac
[ -d "$root/src/core/net" ] || { echo "$root is not the project root" >&2; exit 1; }

# Every library source, plus one test main at a time. The test directory is
# matched at a path boundary, not by "/test/": at the workspace root the path
# is "test/net_test.c", with nothing in front of it to match.
libs=$(printf '%s\n' "$@" | grep '\.c$' | grep -vE '(^|/)test/')
mains=$(printf '%s\n' "$@" | grep -E '(^|/)test/.*_test\.c$')
[ -n "$libs" ] && [ -n "$mains" ] ||
    { echo "no sources matched: a test that builds nothing must not pass" >&2; exit 1; }

for main in $mains; do
    name="$(basename "$main" .c)"
    echo "== $name (asan+ubsan)"
    clang -std=c17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -I "$root/src" -I "$root/test" -o "$tmp/$name" $libs "$main"
    "$tmp/$name"
done
