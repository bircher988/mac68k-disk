#!/bin/sh
# tests/run.sh - tests of mac68k-disk. Needs the built binary and python3.
#
# For MFS and HFS: create a disk, add MacBinary (I, II, III) and raw files
# (one with only a resource fork, an empty one, a ~300K one), list them, get
# them back and compare name, type, creator, Finder flags and both forks
# bit for bit; replace, remove and re-add (free space must come back); error
# paths (disk full, duplicate names, bad names) must leave the image
# unchanged; startup sets the boot blocks' startup application. MFS gets the
# Finder's folders, also as later Finders record them; HFS gets folders and a
# stress test with 150 files that forces a multi-level catalog B-tree.
# DiskCopy 4.2 images: both checksums, the tag data, the header. After every
# change the image is verified by tests/mactest.py, a structure checker
# written independently of the C code.
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

echo "== MFS folders (the Finder's, in its DeskTop file)"
run "$D" mkdir "$M" Games
check "$M"
run "$D" add "$M" "$IN/app.bin" --name "Games:Hello"
run "$D" mkdir "$M" "Games:Old"
run "$D" mkdir "$M" Tools
check "$M"
contains "$("$D" ls -R "$M")" "Games:Hello" "ls -R"
contains "$("$D" ls -R "$M")" "Games:Old:" "ls -R"
contains "$("$D" ls -l "$M")" "2 items" "ls -l folder"
run "$D" get "$M" "Games:Hello" -o "$TMP/out/mfs-games.bin"
same_but_name "$IN/app.bin" "$TMP/out/mfs-games.bin"
run "$D" get "$M" Hello -o "$TMP/out/mfs-plain.bin"          # a plain name finds it in any folder
same_but_name "$IN/app.bin" "$TMP/out/mfs-plain.bin"
fails "$M" "already exists" "$D" add "$M" "$IN/app.bin" --name "Tools:Hello"   # names are unique on MFS
fails "$M" "already exists" "$D" mkdir "$M" Games
fails "$M" "folder not empty" "$D" rm "$M" Games
run "$D" add -f "$M" "$IN/app.bin" --name "Tools:Hello"     # moves the file along
contains "$("$D" ls "$M" Tools)" "Hello" "replaced into Tools"
run "$D" rm "$M" "Games:Old"
run "$D" rm "$M" Games
check "$M"
fails "$M" "no file or folder" "$D" ls "$M" Games
run "$D" rm "$M" "Tools:Hello"
run "$D" rm "$M" Tools
check "$M"
run "$D" rm "$M" DeskTop                                     # as the Finder: the folders go with it
check "$M"

echo "== MFS folders as later Finders record them (98 bytes before the name)"
L=$TMP/later.dsk
run "$D" new "$L" --name "Later"
run "$D" mkdir "$L" "XXXXTools"
run "$D" mkdir "$L" "Docs"
run "$D" add "$L" "$IN/app.bin" --name "XXXXTools:Hello"
# the record of XXXXTools rewritten in place: four more bytes before the name, which is
# now "Tools" (the record keeps its length), and the resource's name with it
python3 - "$L" <<'PY' || bad "could not rewrite the folder record"
import sys
img = bytearray(open(sys.argv[1], 'rb').read())
old = b'\x09XXXXTools'
at = [i for i in range(len(img)) if img.startswith(old, i)]
rec = [i for i in at if img[i - 94:i - 92] == b'\x00\x08']
name = [i for i in at if i not in rec]
assert len(rec) == 1 and len(name) == 1, (rec, name)
img[rec[0]:rec[0] + 10] = b'\x00\x00\x00\x00\x05Tools'
img[name[0]:name[0] + 6] = b'\x05Tools'
open(sys.argv[1], 'wb').write(img)
PY
check "$L"
contains "$("$D" ls -R "$L")" "Tools:Hello" "ls -R, later record"
case "$("$D" ls -R "$L")" in *XXXX*) bad "later record: the old name still shows";; *) good;; esac
run "$D" get "$L" "Tools:Hello" -o "$TMP/out/later.bin"
same_but_name "$IN/app.bin" "$TMP/out/later.bin"
run "$D" mkdir "$L" "Tools:Inner"
run "$D" add "$L" "$IN/raw.txt" --name "Tools:Inner:Raw"
check "$L"

echo "== MFS folders as the first Finders record them (58 bytes, the name on the resource)"
F=$TMP/first.dsk
run "$D" new "$F" --name "First"
run "$D" mkdir "$F" "Old Tools"
run "$D" mkdir "$F" "Docs"
run "$D" add "$F" "$IN/app.bin" --name "Old Tools:Hello"
# the record of Old Tools rewritten in place: 58 bytes and room for eleven items, the
# number of items at 56, no name in the record (the resource keeps its name)
python3 - "$F" <<'PY' || bad "could not rewrite the folder record"
import sys
img = bytearray(open(sys.argv[1], 'rb').read())
name = b'\x09Old Tools'
rec = [i - 94 for i in range(94, len(img)) if img.startswith(name, i) and img[i - 94:i - 92] == b'\x00\x08']
assert len(rec) == 1, rec
r = rec[0]
assert int.from_bytes(img[r - 4:r], 'big') == 94 + len(name)
img[r - 4:r] = (58 + 4 * 11).to_bytes(4, 'big')
img[r + 56:r + 58] = (1).to_bytes(2, 'big')
img[r + 58:r + 94 + len(name)] = bytes(36 + len(name))
open(sys.argv[1], 'wb').write(img)
PY
check "$F"
contains "$("$D" ls -R "$F")" "Old Tools:Hello" "ls -R, first record"
run "$D" get "$F" "Old Tools:Hello" -o "$TMP/out/first.bin"
same_but_name "$IN/app.bin" "$TMP/out/first.bin"
run "$D" add "$F" "$IN/raw.txt" --name "Old Tools:Raw"
run "$D" mkdir "$F" "Old Tools:Inner"
run "$D" add "$F" "$IN/raw.txt" --name "Old Tools:Inner:Deep"
check "$F"
contains "$("$D" ls -R "$F")" "Old Tools:Inner:Deep" "a folder inside a first record"
fails "$F" "folder not empty" "$D" rm "$F" "Old Tools"
run "$D" rm "$F" "Old Tools:Inner:Deep"
run "$D" rm "$F" "Old Tools:Inner"
run "$D" rm "$F" "Old Tools:Raw"
run "$D" rm "$F" "Old Tools:Hello"
run "$D" rm "$F" "Old Tools"
check "$F"
case "$("$D" ls -R "$F")" in *"Old Tools"*) bad "first record: the folder is still there";; *) good;; esac
contains "$("$D" ls -l "$L")" "2 items" "ls -l, later record"
fails "$L" "already exists" "$D" mkdir "$L" Tools
run "$D" rm "$L" "Tools:Inner:Raw"
run "$D" rm "$L" "Tools:Inner"
run "$D" rm "$L" "Tools:Hello"
run "$D" rm "$L" Tools
check "$L"
# a record whose own name fits neither way: the resource's name counts
python3 - "$L" <<'PY' || bad "could not damage the folder record"
import sys
img = bytearray(open(sys.argv[1], 'rb').read())
rec = [i for i in range(len(img)) if img.startswith(b'\x04Docs', i) and img[i - 94:i - 92] == b'\x00\x08']
assert len(rec) == 1, rec
img[rec[0]] = 0x7F
open(sys.argv[1], 'wb').write(img)
PY
contains "$("$D" ls "$L")" "Docs:" "folder name from the resource name"
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
[ "$("$D" ls "$G" | wc -l | tr -d " ")" = 190 ] && good || bad "190 files expected on the grown disk"
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

echo "== startup disks: new --system"
$PY mkbin "$IN/system.bin" --name "System" --type ZSYS --creator MACS --flags 0x1000 --data 400 --rsrc 30000 --seed 11
$PY mkbin "$IN/finder.bin" --name "Finder" --type FNDR --creator MACS --flags 0x1000 --rsrc 20000 --seed 12
$PY mkbin "$IN/clip.bin"   --name "Clipboard File" --type CLIP --creator MACS --data 8 --seed 13
S=$TMP/sys-flat.dsk                  # a startup disk without folders, as mac68k-disk made them
run "$D" new "$S" --name "System Disk"
python3 -c "
import sys; f = open(sys.argv[1], 'r+b'); f.write(b'LK' + bytes(range(90)) + b'\x06Finder' + bytes(1024 - 2 - 90 - 7))" "$S"
run "$D" add "$S" "$IN/system.bin" "$IN/finder.bin" "$IN/clip.bin" "$IN/app.bin"
fails_new "$TMP/p0.dsk" "no boot blocks" "$D" new "$TMP/p0.dsk" --system "$M"
P1=$TMP/p1.dsk
run "$D" new "$P1" --name Programs --system "$S"
check "$P1"
contains "$("$D" ls -R "$P1")" "System Folder:System" "system files in the System Folder"
contains "$("$D" ls -l "$P1" "System Folder")" "Clipboard File" "all non-applications"
if "$D" ls -R "$P1" | grep -q "Hello App"; then bad "new --system copied an application"; else good; fi
contains "$("$D" info "$P1")" 'startup   yes (System Folder "System Folder")' "info"
[ "$(head -c 1024 "$P1" | cksum)" = "$(head -c 1024 "$S" | cksum)" ] && good || bad "boot blocks not copied"
run "$D" get "$P1" "System Folder:System" -o "$TMP/out/sys1.bin"
same "$IN/system.bin" "$TMP/out/sys1.bin"
run "$D" startup "$P1" Finder                                 # found in the System Folder
run "$D" add "$P1" "$IN/app.bin"
check "$P1"
P2=$TMP/p2.dsk                       # from a disk with a System Folder: only its contents
run "$D" new "$P2" --system "$P1"
check "$P2"
if "$D" ls -R "$P2" | grep -q "Hello App"; then bad "new --system copied more than the System Folder"; else good; fi
contains "$("$D" ls -l "$P2")" "3 items" "System Folder of a folder disk"
H2=$TMP/p3.dsk                       # onto HFS: the System Folder is blessed
run "$D" new "$H2" --hfs --system "$P1"
check "$H2"
contains "$("$D" info "$H2")" 'startup   yes (System Folder "System Folder")' "blessed on HFS"
# HFS: the volume header names the folder of the startup application; the Mac finds it
# there and in the System Folder (System 3.2 and 6.0.8 start the Finder otherwise)
eq() { [ "$1" = "$2" ] && good || bad "$3: '$1', expected '$2'"; }
eq "$($PY startupdir "$H2")" "-" "a new startup disk records no folder"
fails "$H2" 'no file "Hello App" at the top level or in the System Folder' "$D" startup "$H2" "Hello App"
run "$D" add "$H2" "$IN/app.bin" --name "System Folder:"
run "$D" startup "$H2" "Hello App"                            # a plain name: also found in the System Folder
eq "$($PY startupdir "$H2")" "System Folder" "startup application in the System Folder"
contains "$("$D" info "$H2")" 'opens     Hello App, from the folder "System Folder"' "info, System Folder"
if "$D" info "$H2" | grep -q "not there"; then bad "info: startup application reported missing"; else good; fi
run "$D" add "$H2" "$IN/app.bin"                              # ... but first at the top level
run "$D" startup "$H2" "Hello App"
eq "$($PY startupdir "$H2")" ":" "startup application at the top level"
contains "$("$D" startup "$H2")" 'opens "Hello App" at startup, from the top level of the disk' "startup, top level"
run "$D" mkdir "$H2" "Games"
run "$D" add "$H2" "$IN/app.bin" --name "Games:Play"
run "$D" startup "$H2" "Games:Play"
eq "$($PY startupdir "$H2")" "Games" "startup application in a folder"
contains "$("$D" info "$H2")" 'opens     Play, from the folder "Games"' "info, folder"
check "$H2"
fails "$H2" 'no file "Games:Nothing" on the disk' "$D" startup "$H2" "Games:Nothing"
fails "$H2" 'no folder "Nowhere"' "$D" startup "$H2" "Nowhere:Play"
fails "$H2" "names a folder" "$D" startup "$H2" "Games:"
fails "$H2" "15 characters" "$D" startup "$H2" "Games:A Name Far Too Long"
run "$D" startup "$H2" "Games:Soon" -f                        # -f with a path: name and folder
eq "$($PY startupdir "$H2")" "Games" "startup -f keeps the folder of the path"
contains "$("$D" startup "$H2")" '"Soon" at startup, from the folder "Games" (not there and not in the System Folder' "startup: application missing"
contains "$("$D" info "$H2")" 'opens     Soon, from the folder "Games" (not there' "info: startup application missing"
run "$D" startup "$H2" "Finder"                               # in the System Folder
eq "$($PY startupdir "$H2")" "System Folder" "back to the Finder"
run "$D" startup "$H2" "Elsewhere" -f                         # -f with a plain name: the folder stays
eq "$($PY startupdir "$H2")" "System Folder" "startup -f, plain name"
run "$D" startup "$H2" "Games:Play"
run "$D" rm "$H2" "Games:Play"
run "$D" rm "$H2" "Games"                                     # the folder goes: so does its number in the header
eq "$($PY startupdir "$H2")" "-" "the folder of the startup application removed"
run "$D" rm "$H2" "Hello App"
run "$D" rm "$H2" "System Folder:Hello App"
check "$H2"
P4=$TMP/p4.dsk                       # from HFS back to MFS
run "$D" new "$P4" --system "$H2"
check "$P4"
contains "$("$D" ls -R "$P4")" "System Folder:Finder" "HFS -> MFS"

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
# startup: show and set the application the disk opens at boot (bbHelloName, $5A)
run "$D" startup "$B"
fails "$B" "no file \"Nothing\"" "$D" startup "$B" Nothing
fails "$B" "15 characters" "$D" startup "$B" "A Name Far Too Long"
run "$D" startup "$B" "Hello App"
contains "$("$D" startup "$B")" "opens \"Hello App\" at startup" "startup name"
contains "$("$D" info "$B")" "opens     Hello App" "info shows the startup application"
[ "$(od -A n -t x1 -j 90 -N 16 "$B" | tr -d ' \n')" = "0948656c6c6f20417070000000000000" ] && good || bad "bbHelloName bytes"
run "$D" startup "$B" Elsewhere -f
contains "$("$D" startup "$B")" "opens \"Elsewhere\"" "startup -f"
check "$B"
fails "$M" "no boot blocks" "$D" startup "$M" Finder
echo "== DiskCopy 4.2 images"
DC=$TMP/dc42.image
run $PY mkdc42 "$M" "$DC" --tags
check "$DC"
contains "$("$D" info "$DC")" "400K DiskCopy 4.2 image" "info of a DiskCopy image"
[ "$("$D" ls -l -R "$DC")" = "$("$D" ls -l -R "$M")" ] && good || bad "ls of the DiskCopy image differs from the raw image"
cp "$DC" "$TMP/dc42.before"
run "$D" add "$DC" "$IN/app.bin" --name "DC App"
check "$DC"                                                  # the checker verifies both checksums
run "$D" get "$DC" "DC App" -o "$TMP/out/dc.bin" && same_but_name "$IN/app.bin" "$TMP/out/dc.bin"
# header and size stay, the tags of the changed sectors are cleared, the others kept
python3 - "$TMP/dc42.before" "$DC" <<'PY' && good || bad "DiskCopy image after add: header, size or tags"
import sys
a, b = (open(f, 'rb').read() for f in sys.argv[1:3])
assert len(a) == len(b) and a[:72] == b[:72] and a[80:84] == b[80:84], 'header or size'
n = (len(a) - 84) // (512 + 12)
changed = [s for s in range(n) if a[84 + s * 512:84 + (s + 1) * 512] != b[84 + s * 512:84 + (s + 1) * 512]]
tag = lambda x, s: x[84 + n * 512 + s * 12:84 + n * 512 + (s + 1) * 12]
assert changed, 'nothing changed'
assert all(tag(b, s) == bytes(12) for s in changed), 'tag of a changed sector not cleared'
assert all(tag(b, s) == tag(a, s) for s in range(n) if s not in changed), 'tag of an unchanged sector touched'
PY
run "$D" mkdir "$DC" "DC Folder"
run "$D" add -f "$DC" "$IN/app.bin" --name "DC Folder:DC App"
check "$DC"
run "$D" rm "$DC" "DC Folder:DC App"
run "$D" rm "$DC" "DC Folder"
check "$DC"
fails "$DC" "no file or folder" "$D" rm "$DC" "DC App"
run $PY mkdc42 "$H" "$TMP/dc42h.image"                       # HFS, without tag data
contains "$("$D" info "$TMP/dc42h.image")" "800K DiskCopy 4.2 image" "info of an HFS DiskCopy image"
run "$D" add "$TMP/dc42h.image" "$IN/app.bin" --name "DC App"
check "$TMP/dc42h.image"
[ "$(wc -c < "$TMP/dc42h.image" | tr -d ' ')" = 819284 ] && good || bad "size of the HFS DiskCopy image"
run $PY mkdc42 "$M" "$TMP/dc42bad.image" --bad-sum           # a wrong checksum: told, and set right by a change
contains "$("$D" info "$TMP/dc42bad.image")" "do not match" "info names the wrong checksum"
$PY check "$TMP/dc42bad.image" > /dev/null && bad "the checker accepts a wrong checksum" || good
run "$D" add "$TMP/dc42bad.image" "$IN/app.bin" --name "DC App"
check "$TMP/dc42bad.image"
case "$("$D" info "$TMP/dc42bad.image")" in *"do not match"*) bad "checksum still reported wrong";; *) good;; esac
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
        if [ "$(free_blocks "$img")" -lt 8 ]; then echo "   (full: no add, get and rm)"; continue; fi
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
