#!/bin/sh
# tests/run.sh - tests of mac68k-disk. Needs the built binary and python3.
#
# For MFS and HFS: create a disk, add MacBinary (I, II, III) and raw files
# (one with only a resource fork, an empty one, a ~300K one), list them, get
# them back and compare name, type, creator, Finder flags and both forks
# bit for bit; replace, remove and re-add (free space must come back); error
# paths (disk full, duplicate names, bad names, DiskCopy images) must leave
# the image unchanged. HFS also gets folders and a stress test with 150
# files that forces a multi-level catalog B-tree. After every change the
# image is verified by tests/mactest.py, a structure checker written
# independently of the C code.
# MAC68K_TEST_DISKS=<dir> adds tests on copies of the images in <dir>.
# At the end, tests/oracle.sh runs the interoperability tests with hfsutils
# if it is installed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
D=${MAC68K_DISK:-$ROOT/mac68k-disk}
[ -x "$D" ] || { echo "mac68k-disk binary not found: $D (run make first)"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is needed for the tests"; exit 1; }
PY="python3 $HERE/mactest.py"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/mac68k-disk-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
pass=0 fail=0

bad() { echo "FAIL: $*"; fail=$((fail + 1)); }
good() { pass=$((pass + 1)); }
# run CMD...: must succeed
run() {
    if out=$("$@" 2>&1); then good; return 0; fi
    bad "$* -> $out"; return 1
}
# fails IMAGE MESSAGE CMD...: must fail with MESSAGE in its output and leave IMAGE unchanged
fails() {
    img=$1 msg=$2; shift 2
    before=$(cksum < "$img")
    if out=$("$@" 2>&1); then bad "$*: should have failed"; return; fi
    case "$out" in *"$msg"*) ;; *) bad "$*: expected '$msg', got: $out"; return;; esac
    [ "$(cksum < "$img")" = "$before" ] || { bad "$*: image changed although the command failed"; return; }
    good
}
check() {
    if out=$($PY check "$@"); then good; else bad "structure: $out"; fi
}
same() {    # same A B: MacBinary files agree in name, type, creator, flags and forks
    if out=$($PY cmpbin "$1" "$2"); then good; else bad "$out"; fi
}
same_but_name() {
    if out=$($PY cmpbin "$1" "$2" --ignore-name); then good; else bad "$out"; fi
}
# fails_new FILE MESSAGE CMD...: must fail with MESSAGE and not create FILE
fails_new() {
    file=$1 msg=$2; shift 2
    if out=$("$@" 2>&1); then bad "$*: should have failed"; return; fi
    case "$out" in *"$msg"*) ;; *) bad "$*: expected '$msg', got: $out"; return;; esac
    [ ! -e "$file" ] && good || bad "$*: created $file"
}
same_bytes() {
    if cmp -s "$1" "$2"; then good; else bad "$1 and $2 differ"; fi
}
free_blocks() { "$D" info "$1" | awk '/allocation blocks/ { print $(NF-1) }'; }
contains() {    # contains TEXT PATTERN WHAT
    case "$1" in *"$2"*) good;; *) bad "$3: '$2' missing in: $1";; esac
}

IN=$TMP/in
mkdir -p "$IN" "$TMP/out"
$PY mkbin "$IN/app.bin"      --name "Hello App"  --type APPL --creator HAPP --flags 0x2000 --rsrc 2000 --seed 1
$PY mkbin "$IN/both.bin"     --name "Both Forks" --type TEXT --creator ttxt --data 5000 --rsrc 700 --seed 2
$PY mkbin "$IN/rsrconly.bin" --name "Rsrc Only"  --type rsrc --creator RSED --rsrc 1234 --seed 3
$PY mkbin "$IN/empty.bin"    --name "Empty"      --type TEXT --creator ttxt --seed 4
$PY mkbin "$IN/big.bin"      --name "Big File"   --type DATA --creator BIGF --data 300000 --rsrc 100 --seed 5
$PY mkbin "$IN/mb1.bin"      --name "Old Format" --type TEXT --creator MACA --flags 0x2000 --data 1000 --rsrc 300 --version 1 --seed 6
$PY mkbin "$IN/mb3.bin"      --name "New Format" --type PICT --creator ttxt --flags 0x1040 --data 3000 --version 3 --seed 7
$PY mkbin "$IN/inited.bin"   --name "Placed"     --type APPL --creator PLCD --flags 0x2101 --rsrc 500 --seed 8
$PY mkbin "$IN/placed.bin"   --name "Placed"     --type APPL --creator PLCD --flags 0x2000 --rsrc 500 --seed 8
$PY mkbin "$IN/huge.bin"     --name "Huge"       --type DATA --creator BIGF --data 1600000 --seed 9
head -c 777 "$IN/big.bin" > "$IN/raw.txt"
head -c 3333 "$IN/both.bin" > "$IN/rsrc.dat"
cp "$IN/raw.txt" "$IN/odd:name.txt"
$PY mkbin "$IN/large.bin"    --name "Large"      --type DATA --creator BIGF --data 1000000 --rsrc 5000 --seed 10

roundtrip() {   # roundtrip FORMAT IMAGE
    fmt=$1 img=$2
    echo "== $fmt round trip"
    check "$img"
    run "$D" add "$img" "$IN/app.bin" "$IN/both.bin" "$IN/rsrconly.bin" "$IN/empty.bin" "$IN/big.bin" "$IN/mb1.bin" "$IN/mb3.bin"
    check "$img"
    listing=$("$D" ls "$img")
    for n in "Hello App" "Both Forks" "Rsrc Only" "Empty" "Big File" "Old Format" "New Format"; do
        contains "$listing" "$n" "ls"
    done
    for pair in "Hello App=app" "Both Forks=both" "Rsrc Only=rsrconly" "Empty=empty" "Big File=big" "Old Format=mb1" "New Format=mb3"; do
        n=${pair%=*} f=${pair#*=}
        run "$D" get "$img" "$n" -o "$TMP/out/$f.bin" && same "$IN/$f.bin" "$TMP/out/$f.bin"
    done
    long=$("$D" ls -l "$img")
    contains "$long" "APPL HAPP          0       2000" "ls -l"
    contains "$long" "TEXT ttxt       5000        700" "ls -l"

    # raw files, --type/--creator, --rsrc, forks on their own
    run "$D" add "$img" "$IN/raw.txt"
    run "$D" get "$img" raw.txt --data -o "$TMP/out/raw.txt" && same_bytes "$IN/raw.txt" "$TMP/out/raw.txt"
    contains "$("$D" ls -l "$img" raw.txt)" "???? ????        777          0" "raw file"
    run "$D" add "$img" "$IN/raw.txt" --name Notes --type TEXT --creator ttxt
    contains "$("$D" ls -l "$img" Notes)" "TEXT ttxt        777          0" "--type/--creator"
    run "$D" add "$img" "$IN/raw.txt" --name "With Rsrc" --rsrc "$IN/rsrc.dat" --type APPL --creator WRSC
    run "$D" get "$img" "With Rsrc" --rsrc -o "$TMP/out/rsrc.dat" && same_bytes "$IN/rsrc.dat" "$TMP/out/rsrc.dat"
    run "$D" get "$img" "With Rsrc" --data -o "$TMP/out/wr.txt" && same_bytes "$IN/raw.txt" "$TMP/out/wr.txt"
    run "$D" get "$img" "Both Forks" --rsrc -o "$TMP/out/both.rsrc"
    "$D" get "$img" "Both Forks" -o - > "$TMP/out/both-stdout.bin" && same "$IN/both.bin" "$TMP/out/both-stdout.bin"
    check "$img"

    # Finder flags: "inited" and "on desk" are cleared so the Finder places the icon
    run "$D" add "$img" "$IN/inited.bin"
    run "$D" get "$img" Placed -o "$TMP/out/placed.bin" && same "$IN/placed.bin" "$TMP/out/placed.bin"

    # names: duplicates (also in other case), replace with -f, bad names
    fails "$img" "already exists" "$D" add "$img" "$IN/app.bin"
    fails "$img" "already exists" "$D" add "$img" "$IN/both.bin" --name "HELLO app"
    run "$D" add -f "$img" "$IN/both.bin" --name "Hello App"
    run "$D" get "$img" "hello app" -o "$TMP/out/replaced.bin"
    same_but_name "$IN/both.bin" "$TMP/out/replaced.bin"
    fails "$img" "':' is not allowed" "$D" add "$img" "$IN/odd:name.txt"
    fails "$img" "no folder" "$D" add "$img" "$IN/raw.txt" --name "Nowhere:Notes"
    fails "$img" "no file" "$D" get "$img" "Nothing Here"
    check "$img"

    # remove and add again: the space must come back
    f1=$(free_blocks "$img")
    run "$D" rm "$img" "Big File"
    f2=$(free_blocks "$img")
    check "$img"
    [ "$f2" -gt "$f1" ] && good || bad "rm did not free space ($f1 -> $f2)"
    fails "$img" "no file" "$D" get "$img" "Big File"
    run "$D" add "$img" "$IN/big.bin"
    f3=$(free_blocks "$img")
    [ "$f3" = "$f1" ] && good || bad "free blocks after rm + add: $f3, before: $f1"
    run "$D" get "$img" "Big File" -o "$TMP/out/big2.bin" && same "$IN/big.bin" "$TMP/out/big2.bin"
    check "$img"

    # disk full leaves the image as it was
    fails "$img" "disk full" "$D" add "$img" "$IN/huge.bin"
    fails "$img" "disk full" "$D" add "$img" "$IN/rsrc.dat" "$IN/huge.bin"
}

# ---------------------------------------------------------------- MFS
echo "== MFS"
M=$TMP/mfs.dsk
run "$D" new "$M" --name "Test MFS"
contains "$("$D" info "$M")" 'MFS volume "Test MFS", 400K image' "info"
contains "$("$D" info "$M")" "391 allocation blocks of 1024 bytes" "info"
fails "$M" "already exists" "$D" new "$M"
roundtrip MFS "$M"
fails "$M" "no folders" "$D" mkdir "$M" Games
fails "$M" "no folders" "$D" add "$M" "$IN/app.bin" --name "Games:Hello"
LONG64=$(printf '%064d' 0)
fails "$M" "longer than 63" "$D" add "$M" "$IN/raw.txt" --name "$LONG64"
run "$D" add "$M" "$IN/raw.txt" --name "$(printf '%063d' 0)"
fails_new "$TMP/x.dsk" "MFS disks are 400k" "$D" new "$TMP/x.dsk" --size 800k
fails_new "$TMP/x.dsk" "unsupported size" "$D" new "$TMP/x.dsk" --hfs --size 720k
check "$M"

# ---------------------------------------------------------------- HFS
echo "== HFS"
H=$TMP/hfs.dsk
run "$D" new "$H" --hfs --name "Test HFS"
contains "$("$D" info "$H")" 'HFS volume "Test HFS", 800K image' "info"
contains "$("$D" info "$H")" "1594 allocation blocks of 512 bytes" "info"
roundtrip HFS "$H"
fails "$H" "longer than 31" "$D" add "$H" "$IN/raw.txt" --name "$(printf '%032d' 0)"

echo "== HFS folders"
run "$D" mkdir "$H" Games
run "$D" mkdir "$H" "Games:Arcade"
fails "$H" "already exists" "$D" mkdir "$H" games
fails "$H" "no folder" "$D" mkdir "$H" "Nowhere:Sub"
run "$D" add "$H" "$IN/app.bin" --name "Games:Hello"
run "$D" add "$H" "$IN/both.bin" "$IN/mb1.bin" --name "Games:Arcade:"
check "$H"
contains "$("$D" ls "$H" Games)" "Arcade:" "ls folder"
rec=$("$D" ls -R "$H")
contains "$rec" "Games:Arcade:Old Format" "ls -R"
contains "$rec" "Games:Hello" "ls -R"
run "$D" get "$H" "Games:Hello" -o "$TMP/out/gh.bin"
same_but_name "$IN/app.bin" "$TMP/out/gh.bin"
run "$D" get "$H" ":games:arcade:both forks" -o "$TMP/out/gab.bin" && same "$IN/both.bin" "$TMP/out/gab.bin"
fails "$H" "not empty" "$D" rm "$H" Games
fails "$H" "is a folder" "$D" get "$H" Games
run "$D" rm "$H" "Games:Arcade:Both Forks"
run "$D" rm "$H" "Games:Arcade:Old Format"
run "$D" rm "$H" "Games:Arcade"
check "$H"
contains "$("$D" ls -l "$H")" "folder        1 item" "folder valence"

echo "== HFS names"
run "$D" add "$H" "$IN/raw.txt" --name "Über Café ß"
contains "$("$D" ls "$H")" "Über Café ß" "Mac Roman name"
run "$D" get "$H" "über café ß" --data -o "$TMP/out/uber.txt" && same_bytes "$IN/raw.txt" "$TMP/out/uber.txt"
check "$H"

echo "== HFS stress (150 files, folders, multi-level catalog)"
S=$TMP/stress.dsk
mkdir -p "$TMP/many" "$TMP/many2"
$PY mkmany "$TMP/many" 150 File
$PY mkmany "$TMP/many2" 20 Doc
run "$D" new "$S" --hfs
run "$D" add "$S" "$TMP"/many/File0*.bin
run "$D" add "$S" "$TMP"/many/File1*.bin
for f in A B C; do run "$D" mkdir "$S" "Folder $f"; done
run "$D" add "$S" "$TMP"/many2/*.bin --name "Folder B:"
run "$D" mkdir "$S" "Folder B:Inner"
run "$D" add "$S" "$TMP"/many2/Doc00*.bin --name "Folder B:Inner:"
check "$S"
tree=$($PY tree "$S")
depth=${tree#depth=}; depth=${depth%% *}
[ "$depth" -ge 3 ] && good || bad "catalog depth $depth, expected an index level or two ($tree)"
echo "   catalog: $tree"
[ "$("$D" ls "$S" | grep -c '^File ')" = 150 ] && good || bad "ls does not show 150 files"
[ "$("$D" ls -R "$S" | grep -c 'Doc ')" = 29 ] && good || bad "ls -R does not show 29 files in folders"
for i in 001 037 075 099 100 128 150; do
    run "$D" get "$S" "File $i" -o "$TMP/out/f.bin" && same "$TMP/many/File$i.bin" "$TMP/out/f.bin"
done
run "$D" get "$S" "Folder B:Inner:Doc 009" -o "$TMP/out/d.bin" && same "$TMP/many2/Doc009.bin" "$TMP/out/d.bin"
for i in $(seq 1 3 150); do printf 'File %03d\n' "$i"; done > "$TMP/rmlist"
while read -r n; do "$D" rm "$S" "$n" > /dev/null || bad "rm $n"; done < "$TMP/rmlist"
check "$S"
[ "$("$D" ls "$S" | grep -c '^File ')" = 100 ] && good || bad "100 files expected after removing 50"
run "$D" add "$S" "$TMP/many/File001.bin" "$TMP/many/File004.bin" "$TMP/many/File148.bin"
run "$D" get "$S" "File 148" -o "$TMP/out/f.bin" && same "$TMP/many/File148.bin" "$TMP/out/f.bin"
check "$S"

echo "== HFS catalog growth: one file per command until the catalog has to move"
G=$TMP/grow.dsk
mkdir -p "$TMP/more"
$PY mkmany "$TMP/more" 40 More
run "$D" new "$G" --hfs
for f in "$TMP"/many/*.bin "$TMP"/more/*.bin; do "$D" add "$G" "$f" > /dev/null || bad "add $f"; done
check "$G"
tree=$($PY tree "$G")
echo "   catalog: $tree"
case "$tree" in *extents=12+*) bad "the catalog was not moved ($tree)";; *) good;; esac
[ "$("$D" ls "$G" | wc -l)" = 190 ] && good || bad "190 files expected on the grown disk"
for f in File001 File150 More040; do
    n=$(echo "$f" | sed 's/\([A-Za-z]*\)\([0-9]*\)/\1 \2/')
    run "$D" get "$G" "$n" -o "$TMP/out/g.bin" && same "$TMP/$( [ "${f#More}" = "$f" ] && echo many || echo more )/$f.bin" "$TMP/out/g.bin"
done

echo "== HFS 400K and 1440K"
for size in 400k 1440k; do
    img=$TMP/hfs$size.dsk
    run "$D" new "$img" --hfs --size $size
    run "$D" add "$img" "$IN/big.bin" "$IN/app.bin"
    run "$D" get "$img" "Big File" -o "$TMP/out/b.bin" && same "$IN/big.bin" "$TMP/out/b.bin"
    check "$img"
done
contains "$("$D" info "$TMP/hfs1440k.dsk")" "2874 allocation blocks of 512 bytes" "1440K geometry"
run "$D" add "$TMP/hfs1440k.dsk" "$IN/large.bin"
run "$D" get "$TMP/hfs1440k.dsk" Large -o "$TMP/out/l.bin" && same "$IN/large.bin" "$TMP/out/l.bin"
check "$TMP/hfs1440k.dsk"

echo "== boot blocks, foreign formats"
B=$TMP/boot.dsk
run "$D" new "$B" --hfs
python3 -c "
import sys; f = open(sys.argv[1], 'r+b'); f.write(b'LK' + bytes(range(256)) * 3 + bytes(1024 - 2 - 768))" "$B"
head -c 1024 "$B" > "$TMP/boot.before"
run "$D" add "$B" "$IN/app.bin"
head -c 1024 "$B" > "$TMP/boot.after"
same_bytes "$TMP/boot.before" "$TMP/boot.after"
contains "$("$D" info "$B")" "startup   yes" "boot blocks"
python3 -c "
import struct, sys
data = open(sys.argv[1], 'rb').read()
h = bytearray(84); h[0] = 4; h[1:5] = b'Test'
struct.pack_into('>II', h, 64, len(data), 0); h[80] = 0; h[81] = 0x12; h[82] = 1; h[83] = 0
open(sys.argv[2], 'wb').write(bytes(h) + data)" "$M" "$TMP/dc42.image"
fails "$TMP/dc42.image" "DiskCopy 4.2 images are not supported yet" "$D" ls "$TMP/dc42.image"
fails "$TMP/dc42.image" "DiskCopy 4.2 images are not supported yet" "$D" add "$TMP/dc42.image" "$IN/app.bin"
head -c 409600 /dev/zero > "$TMP/zero.dsk"
fails "$TMP/zero.dsk" "no MFS or HFS volume" "$D" ls "$TMP/zero.dsk"
fails_new "$TMP/missing.dsk" "no such file" "$D" ls "$TMP/missing.dsk"

# Optional: MAC68K_TEST_DISKS=<dir> runs over copies of every image in that
# directory (for example Apple's system disks): info, ls -R, get of every
# file, then add, get and rm of a file; the structure is checked after each change.
if [ -n "${MAC68K_TEST_DISKS:-}" ]; then
    echo "== images in $MAC68K_TEST_DISKS"
    for src in "$MAC68K_TEST_DISKS"/*; do
        [ -f "$src" ] || continue
        img=$TMP/foreign.img
        cp "$src" "$img"
        if ! "$D" info "$img" > "$TMP/info.txt" 2>&1; then echo "   skipped $(basename "$src"): $(cat "$TMP/info.txt")"; continue; fi
        n=0
        "$D" ls -R "$img" | grep -v ':$' > "$TMP/files.txt"
        while IFS= read -r f; do
            "$D" get "$img" ":$f" -o "$TMP/out/x.bin" > /dev/null && n=$((n + 1)) || bad "$(basename "$src"): get $f"
        done < "$TMP/files.txt"
        echo "   $(basename "$src"): $(head -1 "$TMP/info.txt" | sed 's/^[^:]*: //'), $n files read"
        run "$D" add "$img" "$IN/app.bin" --name "Test App"
        run "$D" get "$img" "Test App" -o "$TMP/out/t.bin" && same_but_name "$IN/app.bin" "$TMP/out/t.bin"
        check "$img"
        run "$D" rm "$img" "Test App"
        check "$img"
    done
fi

echo "== install"
run make -s -C "$ROOT" install PREFIX="$TMP/prefix"
run "$TMP/prefix/bin/mac68k-disk" version
run make -s -C "$ROOT" uninstall PREFIX="$TMP/prefix"
[ ! -e "$TMP/prefix/bin/mac68k-disk" ] && good || bad "uninstall left the binary"

echo "tests: $pass passed, $fail failed"
status=0
[ "$fail" = 0 ] || status=1
if [ -x "$HERE/oracle.sh" ]; then
    "$HERE/oracle.sh" || status=1
fi
exit $status
