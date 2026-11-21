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
for cfg in "1" "2 --same-id" "4 --big" "5 --big --same-id"; do
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

# another game (as the extractor program takes them): its files come out in the folder it names, the games can't be mixed,
# and the port's own check (the importer's default) refuses it
python3 tests/gen_test_nsp.py "$W/x" --seed 8 --app-id 0100C3800049C000 --same-id > /dev/null
python3 tests/gen_test_nsp.py "$W/y" --seed 8 > /dev/null  # (same seed: the same keys, another game)
$CC -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter tools/nspextract.c runtime/nspimport.c -o "$W/ext"
mkdir -p "$W/xout"; cp "$W/ext" "$W/x/prod.keys" "$W/xout/"; cp "$W/x/base.nsp" "$W/x/update.nsp" "$W/xout/"
(cd "$W/xout" && MHGU_NSP_ANY_VERSION=1 ./ext < /dev/null > run.log 2>&1) || { echo "other game: extractor failed"; cat "$W/xout/run.log"; exit 1; }
diff -r "$W/x/expected" "$W/xout/data-mhxx" > /dev/null || { echo "other game: the extracted files differ"; exit 1; }
rm -rf "$W/o2"; mkdir "$W/o2"
if "$W/imp" "$W/x/prod.keys" "$W/o2" "$W/x/base.nsp" "$W/x/update.nsp" > "$W/o2.log" 2>&1; then echo "the port's importer took another game"; exit 1; fi
grep -q "another game" "$W/o2.log" || { echo "no clear message for another game"; exit 1; }
if (cd "$W/xout" && MHGU_NSP_ANY_VERSION=1 ./ext --out "$W/o3" "$W/x/base.nsp" "$W/y/update.nsp" < /dev/null > "$W/o3.log" 2>&1); then echo "mixed games were taken"; exit 1; fi
grep -q "different games" "$W/o3.log" || { echo "no clear message for mixed games"; exit 1; }
echo "other game / mixed games: OK"
echo "test-nsp: OK"
