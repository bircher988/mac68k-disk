#!/bin/sh
# tests/oracle.sh - interoperability with hfsutils (hformat, hmount, hls, hcopy,
# hmkdir), used as an independent HFS implementation. Skipped when hfsutils
# is not installed. hfsutils keeps its current volume in $HOME/.hcwd, so it
# runs with a private HOME here.
#   1. images written by mac68k-disk are read by hls/hcopy, content identical
#   2. images made by hformat get files and folders from mac68k-disk and are
#      read back by hls/hcopy
#   3. images written by hcopy are read by mac68k-disk
#   4. catalog order: names with punctuation, digits, mixed case and accented
#      letters are placed by mac68k-disk where hfsutils finds them by name,
#      also when both tools add files to the same disk
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
D=${MAC68K_DISK:-$ROOT/mac68k-disk}
if ! command -v hmount >/dev/null || ! command -v hcopy >/dev/null || ! command -v hformat >/dev/null; then
    echo "oracle: hfsutils not installed, skipped"
    exit 0
fi
PY="python3 $HERE/mactest.py"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/mac68k-disk-oracle.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export HOME="$TMP/home"
mkdir -p "$HOME" "$TMP/in" "$TMP/out"
pass=0 fail=0
bad() { echo "FAIL: $*"; fail=$((fail + 1)); }
good() { pass=$((pass + 1)); }
run() {
    if out=$("$@" 2>&1); then good; return 0; fi
    bad "$* -> $out"; return 1
}
same() {
    if out=$($PY cmpbin "$1" "$2"); then good; else bad "$out"; fi
}
# hcopy expands wildcards in HFS paths itself: names with [ { * ? are in the
# catalog (they take part in the ordering) but are not looked up by hcopy
globby() { case "$1" in *[\[\{\*\?]*) return 0;; esac; return 1; }
check() {
    if out=$($PY check "$@"); then good; else bad "structure: $out"; fi
}

IN=$TMP/in
$PY mkbin "$IN/app.bin"  --name "Hello App"  --type APPL --creator HAPP --flags 0x2000 --rsrc 2000 --seed 1
$PY mkbin "$IN/both.bin" --name "Both Forks" --type TEXT --creator ttxt --data 5000 --rsrc 700 --seed 2
$PY mkbin "$IN/big.bin"  --name "Big File"   --type DATA --creator BIGF --data 300000 --rsrc 100 --seed 5
$PY mkbin "$IN/empty.bin" --name "Empty"     --type TEXT --creator ttxt --seed 4
mkdir -p "$TMP/many"
$PY mkmany "$TMP/many" 60 File

echo "== oracle 1: mac68k-disk writes, hfsutils reads"
A=$TMP/ours.dsk
run "$D" new "$A" --hfs --name "Ours"
run "$D" add "$A" "$IN/app.bin" "$IN/both.bin" "$IN/big.bin" "$IN/empty.bin"
run "$D" mkdir "$A" "Folder"
run "$D" add "$A" "$TMP"/many/*.bin --name "Folder:"
run hmount "$A"
listing=$(hls -R 2>&1)
for n in "Hello App" "Both Forks" "Big File" "Empty" "Folder" "File 060"; do
    case "$listing" in *"$n"*) good;; *) bad "hls -R does not list $n";; esac
done
[ "$(hls :Folder | wc -w)" = 120 ] && good || bad "hls :Folder: $(hls :Folder | wc -w) words, expected 60 names"
for pair in "Hello App=app" "Both Forks=both" "Big File=big" "Empty=empty"; do
    n=${pair%=*} f=${pair#*=}
    run hcopy -m ":$n" "$TMP/out/h-$f.bin" && same "$IN/$f.bin" "$TMP/out/h-$f.bin"
done
for i in 001 030 060; do
    run hcopy -m ":Folder:File $i" "$TMP/out/h.bin" && same "$TMP/many/File$i.bin" "$TMP/out/h.bin"
done
humount >/dev/null 2>&1

echo "== oracle 2: hformat image, files added by mac68k-disk"
B=$TMP/hformat.dsk
head -c 819200 /dev/zero > "$B"
run hformat -l "Formatted" "$B"
humount >/dev/null 2>&1
run "$D" add "$B" "$IN/app.bin" "$IN/big.bin"
run "$D" mkdir "$B" "Games"
run "$D" add "$B" "$IN/both.bin" --name "Games:Both Forks"
run "$D" add "$B" "$TMP"/many/File0[0-4]*.bin
check "$B"
run hmount "$B"
listing=$(hls -R 2>&1)
for n in "Hello App" "Big File" "Games" "Both Forks" "File 049"; do
    case "$listing" in *"$n"*) good;; *) bad "hls -R does not list $n";; esac
done
run hcopy -m ":Big File" "$TMP/out/b.bin" && same "$IN/big.bin" "$TMP/out/b.bin"
run hcopy -m ":Games:Both Forks" "$TMP/out/b2.bin" && same "$IN/both.bin" "$TMP/out/b2.bin"
run hcopy -m ":File 025" "$TMP/out/f.bin" && same "$TMP/many/File025.bin" "$TMP/out/f.bin"
# hfsutils writes into the same volume after us, we read that back
run hcopy -m "$IN/empty.bin" ":Games:Empty"
humount >/dev/null 2>&1
run "$D" get "$B" "Games:Empty" -o "$TMP/out/e.bin" && same "$IN/empty.bin" "$TMP/out/e.bin"
run "$D" rm "$B" "Hello App"
check "$B"
run hmount "$B"
case "$(hls)" in *"Hello App"*) bad "hls still lists a removed file";; *) good;; esac
humount >/dev/null 2>&1

echo "== oracle 3: hcopy writes, mac68k-disk reads"
C=$TMP/theirs.dsk
head -c 819200 /dev/zero > "$C"
run hformat -l "Theirs" "$C"
run hmkdir ":Sub"
run hcopy -m "$IN/app.bin" ":Hello App"
run hcopy -m "$IN/big.bin" ":Sub:Big File"
for f in "$TMP"/many/File00*.bin; do hcopy -m "$f" ":Sub:" || bad "hcopy $f"; done
humount >/dev/null 2>&1
check "$C" --foreign
listing=$("$D" ls -R "$C")
for n in "Hello App" "Sub:" "Sub:Big File" "Sub:File 009"; do
    case "$listing" in *"$n"*) good;; *) bad "ls -R does not list $n";; esac
done
run "$D" get "$C" "Hello App" -o "$TMP/out/a.bin" && same "$IN/app.bin" "$TMP/out/a.bin"
run "$D" get "$C" "Sub:Big File" -o "$TMP/out/b.bin" && same "$IN/big.bin" "$TMP/out/b.bin"
run "$D" get "$C" "Sub:File 007" -o "$TMP/out/f.bin" && same "$TMP/many/File007.bin" "$TMP/out/f.bin"

echo "== oracle 4: catalog order (lookups by name in hfsutils)"
E=$TMP/order.dsk
run "$D" new "$E" --hfs
# the names are written to files one per line, then added in a shuffled order
cat > "$TMP/names" <<'EOF'
a
B
aa
Zeta
zz
Z_
_under
[bracket
]close
~tilde
{brace
|pipe
0zero
9nine
 space
!bang
@at
^caret
-dash
.dot
Äbc
Abc
Über
Uz
Ubc
Öbc
Oz
Émile
Ezra
Åre
Zebra
ñandu
Nz
EOF
python3 - "$TMP/names" "$TMP/names.shuf" <<'PYEOF'
import random, sys
names = open(sys.argv[1], encoding='utf-8').read().split('\n')[:-1]
random.Random(4).shuffle(names)
open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(names) + '\n')
PYEOF
half=0
while IFS= read -r n; do
    half=$((half + 1))
    [ $half -le 17 ] || break
    "$D" add "$E" "$IN/empty.bin" --name "$n" >/dev/null || bad "add '$n'"
done < "$TMP/names.shuf"
check "$E"
run hmount "$E"
# hfsutils takes names in Mac Roman: convert the UTF-8 list
python3 -c "
import sys
for l in open(sys.argv[1], encoding='utf-8'):
    sys.stdout.buffer.write(l.encode('mac_roman'))" "$TMP/names.shuf" > "$TMP/names.mr"
n=0
while IFS= read -r name; do
    n=$((n + 1))
    if [ $n -le 17 ]; then
        globby "$name" && continue
        hcopy -m ":$name" "$TMP/out/o.bin" 2>/dev/null && good || bad "hfsutils cannot find '$name' added by mac68k-disk"
    else
        hcopy -m "$IN/empty.bin" ":$name" 2>/dev/null || bad "hcopy to '$name'"
    fi
done < "$TMP/names.mr"
humount >/dev/null 2>&1
# now both tools have added names; mac68k-disk adds more, then everything must be found
for extra in "Aardvark" "Ubz" "Oa" "Zz9" "~~"; do
    "$D" add "$E" "$IN/empty.bin" --name "$extra" >/dev/null || bad "add '$extra'"
done
check "$E"
run hmount "$E"
while IFS= read -r name; do
    globby "$name" && continue
    hcopy -m ":$name" "$TMP/out/o.bin" 2>/dev/null && good || bad "hfsutils cannot find '$name' after mixed adds"
done < "$TMP/names.mr"
for extra in "Aardvark" "Ubz" "Oa" "Zz9" "~~"; do
    hcopy -m ":$extra" "$TMP/out/o.bin" 2>/dev/null && good || bad "hfsutils cannot find '$extra'"
done
humount >/dev/null 2>&1
while IFS= read -r name; do
    "$D" get "$E" ":$name" -o "$TMP/out/o.bin" >/dev/null 2>&1 && good || bad "mac68k-disk cannot find '$name'"
done < "$TMP/names.shuf"

echo "== oracle 5: fragmented files with extents overflow records (written by hcopy)"
F=$TMP/frag.dsk
mkdir -p "$TMP/frag"
head -c 819200 /dev/zero > "$F"
run hformat -l "Frag" "$F"
for i in $(seq 10 69); do head -c 10000 "$IN/big.bin" > "$TMP/frag/s$i.dat"; hcopy -r "$TMP/frag/s$i.dat" ":s$i" || bad "hcopy s$i"; done
free=$(hvol | sed -n 's/.*has \([0-9]*\) bytes free.*/\1/p')
head -c $((free - 2048)) /dev/zero > "$TMP/frag/fill.dat"
run hcopy -r "$TMP/frag/fill.dat" ":Filler"
for i in $(seq 10 2 69); do hdel ":s$i" || bad "hdel s$i"; done
$PY mkbin "$TMP/frag/a.bin" --name "Frag A" --type DATA --creator FRAG --data 90000 --rsrc 12000 --seed 11
$PY mkbin "$TMP/frag/b.bin" --name "Frag B" --type DATA --creator FRAG --data 80000 --rsrc 15000 --seed 12
run hcopy -m "$TMP/frag/a.bin" ":Frag A"
run hcopy -m "$TMP/frag/b.bin" ":Frag B"
humount >/dev/null 2>&1
check "$F" --foreign
nrecs=$(python3 -c "
import struct, sys
d = open(sys.argv[1], 'rb').read(); m = d[1024:1536]
off = (struct.unpack_from('>H', m, 0x1C)[0] + struct.unpack_from('>H', m, 0x86)[0]) * 512
print(struct.unpack_from('>I', d, off + 14 + 6)[0])" "$F")
[ "$nrecs" -ge 4 ] && good || bad "expected extents overflow records, found $nrecs"
echo "   extents overflow records written by hcopy: $nrecs"
run "$D" get "$F" "Frag A" -o "$TMP/out/fa.bin" && same "$TMP/frag/a.bin" "$TMP/out/fa.bin"
run "$D" get "$F" "Frag B" -o "$TMP/out/fb.bin" && same "$TMP/frag/b.bin" "$TMP/out/fb.bin"
run "$D" rm "$F" "Frag A"
check "$F"
run "$D" get "$F" "Frag B" -o "$TMP/out/fb2.bin" && same "$TMP/frag/b.bin" "$TMP/out/fb2.bin"
run hmount "$F"
run hcopy -m ":Frag B" "$TMP/out/fb3.bin" && same "$TMP/frag/b.bin" "$TMP/out/fb3.bin"
case "$(hls)" in *"Frag A"*) bad "hls still lists Frag A";; *) good;; esac
humount >/dev/null 2>&1
before=$(cksum < "$F")
if out=$("$D" add "$F" "$TMP/frag/a.bin" 2>&1); then bad "adding a file that needs more than 3 extents should fail"
else case "$out" in *"fragmented"*) good;; *) bad "unexpected message: $out";; esac; fi
[ "$(cksum < "$F")" = "$before" ] && good || bad "image changed after a failed add"

echo "oracle: $pass passed, $fail failed"
[ "$fail" = 0 ]
