#!/bin/sh
# NSP import test (runtime/nspimport.c). No game and no keys needed: tests/gen_test_nsp.py makes a made-up base game NSP and
# update NSP (the update's RomFS a BKTR patch of the base's) with made-up keys, the importer extracts them, and the result
# must equal what the generator wrote. With hactool on the PATH (or HACTOOL=...), hactool's own dump of the patched RomFS
# section must also equal the generator's: a second implementation of the AES-CTR and BKTR parts.
# Needs python3 with `cryptography` and a C compiler (CC, default cc).
set -e
cd "$(dirname "$0")/.."
W=${TMPDIR:-/tmp}/nsp-test.$$
trap 'rm -rf "$W"' EXIT
mkdir -p "$W"
CC=${CC:-cc}
$CC -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter tests/test_nspimport.c runtime/nspimport.c -o "$W/imp"
for cfg in "1" "2" "4 --big"; do
    set -- $cfg
    seed=$1; shift
    d="$W/s$seed"
    python3 tests/gen_test_nsp.py "$d" --seed "$seed" "$@" > /dev/null
    for keys in prod.keys prod.direct.keys; do
        rm -rf "$d/out"; mkdir "$d/out"
        "$W/imp" "$d/$keys" "$d/out" "$d/update.nsp" "$d/base.nsp" > /dev/null 2>&1 || { echo "seed $seed $keys: import failed"; exit 1; }
        diff -r "$d/expected" "$d/out" > /dev/null || { echo "seed $seed $keys: the extracted files differ"; exit 1; }
    done
    H=${HACTOOL:-$(command -v hactool || true)}
    if [ -n "$H" ]; then
        "$H" -k "$d/prod.direct.keys" --titlekey="$(cat "$d/titlekey.enc")" --basenca="$d/base.nca" --raw --section1="$d/sec1.bin" "$d/update.nca" > /dev/null 2>&1 || true
        cmp "$d/sec1.bin" "$d/update.section1.expected" || { echo "seed $seed: hactool's patched section differs"; exit 1; }
    fi
    echo "seed $seed $*: OK"
done
echo "test-nsp: OK"
