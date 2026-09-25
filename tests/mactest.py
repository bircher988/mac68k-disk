#!/usr/bin/env python3
"""mactest.py - helpers for the mac68k-disk tests, independent of the C code.

  mactest.py mkbin OUT --name N [--type T] [--creator C] [--flags F]
                       [--data SIZE|--data-file F] [--rsrc SIZE] [--seed S] [--version 1|2|3]
        write a MacBinary file with pseudo-random forks
  mactest.py cmpbin A B [--ignore-name]
                              compare two MacBinary files: name, type, creator,
                              Finder flags, data and resource fork
  mactest.py check IMAGE [--foreign]
                              verify the structure of an MFS or HFS image (prints
                              a summary, exit status 1 on problems); --foreign:
                              the image comes from elsewhere, its alternate MDB
                              may be older than the MDB
  mactest.py mkmany DIR N [PREFIX]
                              write N small MacBinary files PREFIX001.bin ...
  mactest.py tree IMAGE       print the catalog tree shape of an HFS image
"""
import random
import struct
import sys

BLK = 512


def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


# ------------------------------------------------------------------ MacBinary

def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def pad128(n):
    return (n + 127) & ~127


def make_macbinary(name, typ, creator, flags, data, rsrc, version=2, crdate=0, mddate=0):
    h = bytearray(128)
    nb = name.encode('mac_roman')
    h[1] = len(nb)
    h[2:2 + len(nb)] = nb
    h[65:69] = typ.encode('mac_roman')
    h[69:73] = creator.encode('mac_roman')
    h[73] = flags >> 8
    struct.pack_into('>II', h, 83, len(data), len(rsrc))
    struct.pack_into('>II', h, 91, crdate, mddate)
    if version >= 2:
        h[101] = flags & 0xFF
        if version == 3:
            h[102:106] = b'mBIN'
        h[122] = 130 if version == 3 else 129
        h[123] = 129
        struct.pack_into('>H', h, 124, crc16(bytes(h[:124])))
    out = bytes(h) + data + bytes(pad128(len(data)) - len(data)) + rsrc + bytes(pad128(len(rsrc)) - len(rsrc))
    return out


def parse_macbinary(b):
    name = b[2:2 + b[1]].decode('mac_roman')
    dlen, rlen = struct.unpack_from('>II', b, 83)
    version = 2 if u16(b, 124) == crc16(b[:124]) else 1
    flags = (b[73] << 8) | (b[101] if version >= 2 else 0)
    sec = u16(b, 120) if version >= 2 else 0
    off = 128 + pad128(sec)
    data = b[off:off + dlen]
    rsrc = b[off + pad128(dlen):off + pad128(dlen) + rlen]
    return dict(name=name, type=b[65:69], creator=b[69:73], flags=flags, data=data, rsrc=rsrc)


def cmd_mkbin(args):
    out = args[0]
    opts = dict(name='File', type='TEXT', creator='????', flags='0', data='0', rsrc='0', seed='1', version='2')
    data_file = None
    i = 1
    while i < len(args):
        k = args[i][2:]
        if k == 'data-file':
            data_file = args[i + 1]
        else:
            opts[k] = args[i + 1]
        i += 2
    rnd = random.Random(int(opts['seed']))
    data = open(data_file, 'rb').read() if data_file else bytes(rnd.getrandbits(8) for _ in range(int(opts['data'])))
    rsrc = bytes(rnd.getrandbits(8) for _ in range(int(opts['rsrc'])))
    with open(out, 'wb') as f:
        f.write(make_macbinary(opts['name'], opts['type'], opts['creator'], int(opts['flags'], 0), data, rsrc,
                               int(opts['version']), crdate=3800000000, mddate=3800000100))


def cmd_cmpbin(args):
    a = parse_macbinary(open(args[0], 'rb').read())
    b = parse_macbinary(open(args[1], 'rb').read())
    keys = ('type', 'creator', 'flags', 'data', 'rsrc') if '--ignore-name' in args else ('name', 'type', 'creator', 'flags', 'data', 'rsrc')
    bad = [k for k in keys if a[k] != b[k]]
    if bad:
        for k in bad:
            va, vb = a[k], b[k]
            if k in ('data', 'rsrc'):
                va, vb = f'{len(va)} bytes', f'{len(vb)} bytes' + (' (same length, different content)' if len(va) == len(vb) else '')
            print(f'{args[0]} vs {args[1]}: {k} differs: {va!r} / {vb!r}')
        return 1
    return 0


# ----------------------------------------------------------------------- MFS

def check_mfs(img):
    problems = []
    m = img[1024:1536]
    dirst, bllen, nmalblks, alblksiz, alblst = u16(m, 14), u16(m, 16), u16(m, 18), u32(m, 20), u16(m, 28)
    raw = img[1024 + 64:dirst * BLK]
    amap = []
    for i in range(nmalblks):
        o = i * 3 // 2
        amap.append(((raw[o] << 4) | (raw[o + 1] >> 4)) if i % 2 == 0 else (((raw[o] & 15) << 8) | raw[o + 1]))
    owner = {}
    files = []
    for b in range(dirst, dirst + bllen):
        blk = img[b * BLK:(b + 1) * BLK]
        o = 0
        while o + 51 <= BLK and blk[o] & 0x80:
            ln = 51 + blk[o + 50]
            if o + ln > BLK:
                problems.append(f'entry crosses block {b}')
                break
            files.append(blk[o:o + ln])
            ln += ln & 1
            o += ln
        if any(blk[o:]):
            problems.append(f'directory block {b}: garbage after the last entry')
    fnums = set()
    for e in files:
        name = e[51:51 + e[50]].decode('mac_roman')
        fnum = u32(e, 18)
        if fnum in fnums:
            problems.append(f'{name}: duplicate file number {fnum}')
        fnums.add(fnum)
        if fnum >= u32(m, 30):
            problems.append(f'{name}: file number {fnum} >= drNxtFNum')
        for label, so, lo, po in (('data', 22, 24, 28), ('rsrc', 32, 34, 38)):
            start, lg, py = u16(e, so), u32(e, lo), u32(e, po)
            n = 0
            ab = start
            while ab:
                if ab < 2 or ab >= nmalblks + 2:
                    problems.append(f'{name} {label}: block {ab} out of range')
                    break
                if ab in owner:
                    problems.append(f'{name} {label}: block {ab} also used by {owner[ab]}')
                    break
                owner[ab] = name
                n += 1
                nxt = amap[ab - 2]
                if nxt == 1:
                    break
                if nxt == 0:
                    problems.append(f'{name} {label}: chain runs into a free block')
                    break
                ab = nxt
            if py != n * alblksiz:
                problems.append(f'{name} {label}: physical length {py} != {n} blocks')
            if lg > py:
                problems.append(f'{name} {label}: logical length {lg} > physical {py}')
    for i, v in enumerate(amap):
        if v and (i + 2) not in owner:
            problems.append(f'block {i + 2} marked in use but belongs to no file')
    free = sum(1 for v in amap if v == 0)
    if u16(m, 34) != free:
        problems.append(f'drFreeBks {u16(m, 34)} != {free} free blocks in the map')
    if u16(m, 12) != len(files):
        problems.append(f'drNmFls {u16(m, 12)} != {len(files)} directory entries')
    return problems, f'MFS: {len(files)} files, {free} of {nmalblks} blocks free'


# ----------------------------------------------------------------------- HFS

def name_key(name):
    return bytes(c - 32 if 0x61 <= c <= 0x7A else c for c in name)


class HfsCheck:
    def __init__(self, img):
        self.img = img
        self.m = img[1024:1536]
        m = self.m
        self.blksize = u32(m, 0x14)
        self.nmalblks = u16(m, 0x12)
        self.alblst = u16(m, 0x1C)
        self.vbmst = u16(m, 0x0E)
        self.problems = []
        self.strict_alt = True
        self.used = {}                 # allocation block -> owner

    def p(self, msg):
        self.problems.append(msg)

    def claim(self, owner, exts):
        for start, count in exts:
            for b in range(start, start + count):
                if b >= self.nmalblks:
                    self.p(f'{owner}: block {b} beyond the volume')
                elif b in self.used:
                    self.p(f'{owner}: block {b} also used by {self.used[b]}')
                else:
                    self.used[b] = owner

    @staticmethod
    def extrec(b, o):
        return [(u16(b, o + 4 * i), u16(b, o + 4 * i + 2)) for i in range(3) if u16(b, o + 4 * i + 2)]

    def read_file(self, exts, size):
        out = bytearray()
        for start, count in exts:
            off = (self.alblst + start * self.blksize // BLK) * BLK
            out += self.img[off:off + count * self.blksize]
        return bytes(out[:size])

    def btree(self, name, data, keycmp, maxkey):
        """Walk a B-tree from its root; returns the leaf records in order."""
        nnodes = len(data) // BLK
        node = lambda i: data[i * BLK:(i + 1) * BLK]
        h = node(0)
        if h[8] != 1:
            self.p(f'{name}: node 0 is not a header node')
            return []
        hr = h[14:14 + 106]
        depth, root, nrecs, fnode, lnode = u16(hr, 0), u32(hr, 2), u32(hr, 6), u32(hr, 10), u32(hr, 14)
        if u16(hr, 18) != 512 or u16(hr, 20) != maxkey or u32(hr, 22) != nnodes:
            self.p(f'{name}: header node size/key length/node count wrong')
        reachable = {0}
        mapn = u32(h, 0)
        while mapn:
            reachable.add(mapn)
            mapn = u32(node(mapn), 0)
        levels = {}
        leaves = []

        def recs(nd):
            n = u16(nd, 10)
            offs = [u16(nd, 510 - 2 * i) for i in range(n + 1)]
            if offs != sorted(offs) or offs[0] != 14 or offs[-1] > 512 - 2 * (n + 1):
                self.p(f'{name}: bad record offsets')
            out = []
            for i in range(n):
                r = nd[offs[i]:offs[i + 1]]
                kl = r[0] + 1
                out.append((r[:kl], r[kl + (kl & 1):]))
            return out

        def walk(i, height, lowkey):
            if i in reachable:
                self.p(f'{name}: node {i} reached twice')
                return
            reachable.add(i)
            nd = node(i)
            levels.setdefault(height, []).append(i)
            if nd[9] != height:
                self.p(f'{name}: node {i} has height {nd[9]}, expected {height}')
            rs = recs(nd)
            if not rs:
                self.p(f'{name}: empty node {i}')
                return
            if lowkey is not None and keycmp(rs[0][0], lowkey) != 0:
                self.p(f'{name}: index key does not match the first key of node {i}')
            if height == 1:
                if nd[8] != 0xFF:
                    self.p(f'{name}: node {i} at height 1 is not a leaf')
                leaves.extend(rs)
            else:
                if nd[8] != 0:
                    self.p(f'{name}: node {i} is not an index node')
                for k, d in rs:
                    if k[0] != maxkey:
                        self.p(f'{name}: index key length {k[0]} != {maxkey}')
                    walk(u32(d, 0), height - 1, k)

        if depth:
            walk(root, depth, None)
        for hgt, nodes in levels.items():
            for j, n in enumerate(nodes):
                nd = node(n)
                want_f = nodes[j + 1] if j + 1 < len(nodes) else 0
                want_b = nodes[j - 1] if j else 0
                if u32(nd, 0) != want_f or u32(nd, 4) != want_b:
                    self.p(f'{name}: sibling links of node {n} (height {hgt}) are wrong')
        if depth and (fnode != levels[1][0] or lnode != levels[1][-1]):
            self.p(f'{name}: first/last leaf in the header are wrong')
        if nrecs != len(leaves):
            self.p(f'{name}: header says {nrecs} leaf records, found {len(leaves)}')
        for a, b in zip(leaves, leaves[1:]):
            if keycmp(a[0], b[0]) >= 0 and keycmp(a[0], b[0], ascii_only=True) is not None:
                self.p(f'{name}: keys out of order: {a[0]!r} >= {b[0]!r}')
        bits = h[248:248 + 256]
        marked = {i for i in range(min(nnodes, 2048)) if bits[i // 8] & (0x80 >> (i % 8))}
        mapn, base = u32(h, 0), 2048
        while mapn:
            mb = node(mapn)[14:14 + 494]
            marked |= {base + i for i in range(3952) if base + i < nnodes and mb[i // 8] & (0x80 >> (i % 8))}
            base += 3952
            mapn = u32(node(mapn), 0)
        if marked != reachable:
            self.p(f'{name}: node map marks {sorted(marked - reachable)[:5]} unused / misses {sorted(reachable - marked)[:5]}')
        if u32(hr, 26) != nnodes - len(marked):
            self.p(f'{name}: free node count {u32(hr, 26)} != {nnodes - len(marked)}')
        self.depth = getattr(self, 'depth', {})
        self.depth[name] = (depth, len(reachable), nnodes)
        return leaves

    def run(self):
        m = self.m
        if u16(m, 0) != 0x4244:
            self.p('no HFS signature')
            return
        self.claim('extents file', self.extrec(m, 0x86))
        xt = self.read_file(self.extrec(m, 0x86), u32(m, 0x82))

        def xcmp(a, b, ascii_only=False):
            ka, kb = (u32(a, 2), a[1], u16(a, 6)), (u32(b, 2), b[1], u16(b, 6))
            return (ka > kb) - (ka < kb)

        xrecs = self.btree('extents', xt, xcmp, 7)
        over = {}
        for k, d in xrecs:
            over.setdefault((u32(k, 2), k[1]), []).extend(self.extrec(d, 0))
        cat_ext = self.extrec(m, 0x96) + over.get((4, 0), [])
        self.claim('catalog file', cat_ext)
        if sum(c for _, c in cat_ext) * self.blksize < u32(m, 0x92):
            self.p('catalog extents shorter than the catalog')
        ct = self.read_file(cat_ext, u32(m, 0x92))

        def ccmp(a, b, ascii_only=False):
            # Only the order of plain ASCII names is known for sure (letters
            # compare without case); names with other characters are only
            # checked for their parent ID.
            na, nb = a[7:7 + a[6]], b[7:7 + b[6]]
            if ascii_only and u32(a, 2) == u32(b, 2) and (max(na, default=0) > 0x7F or max(nb, default=0) > 0x7F):
                return None
            ka = (u32(a, 2), name_key(na))
            kb = (u32(b, 2), name_key(nb))
            return (ka > kb) - (ka < kb)

        recs = self.btree('catalog', ct, ccmp, 37)
        dirs, files, threads, children = {}, {}, {}, {}
        ids = set()
        for k, d in recs:
            par, name = u32(k, 2), k[7:7 + k[6]]
            t = d[0]
            if t == 1:
                did = u32(d, 6)
                if did in ids:
                    self.p(f'duplicate CNID {did}')
                ids.add(did)
                dirs[did] = (par, name, u16(d, 4))
                children.setdefault(par, []).append(name)
            elif t == 2:
                fid = u32(d, 20)
                if fid in ids:
                    self.p(f'duplicate CNID {fid}')
                ids.add(fid)
                files[fid] = (par, name)
                children.setdefault(par, []).append(name)
                for label, lo, po, eo, fork in (('data', 26, 30, 74, 0), ('rsrc', 36, 40, 86, 0xFF)):
                    exts = self.extrec(d, eo) + over.get((fid, fork), [])
                    self.claim(f'{name!r} {label}', exts)
                    blocks = sum(c for _, c in exts)
                    if u32(d, po) != blocks * self.blksize:
                        self.p(f'{name!r} {label}: physical length {u32(d, po)} != {blocks} blocks')
                    if u32(d, lo) > u32(d, po):
                        self.p(f'{name!r} {label}: logical length > physical length')
            elif t in (3, 4):
                if name:
                    self.p('thread record with a name in its key')
                threads[par] = (t, u32(d, 10), d[15:15 + d[14]])
            else:
                self.p(f'unknown catalog record type {t}')
        for did, (par, name, val) in dirs.items():
            th = threads.get(did)
            if not th or th[0] != 3 or th[1] != par or th[2] != name:
                self.p(f'folder {name!r} ({did}): thread record missing or wrong')
            if val != len(children.get(did, [])):
                self.p(f'folder {name!r}: valence {val} != {len(children.get(did, []))} items')
            if did != 2 and par not in dirs:
                self.p(f'folder {name!r}: parent {par} does not exist')
        for fid, (par, name) in files.items():
            if par not in dirs:
                self.p(f'file {name!r}: parent {par} does not exist')
        for tid, th in threads.items():
            if th[0] == 3 and tid not in dirs:
                self.p(f'folder thread {tid} without folder')
        if 2 not in dirs or dirs[2][0] != 1:
            self.p('root folder record missing')
        for par, names in children.items():
            folded = [name_key(n) for n in names]
            if len(set(folded)) != len(folded):
                self.p(f'duplicate names in folder {par}')
        nmfls = sum(1 for p, _ in files.values() if p == 2)
        nmrtdirs = sum(1 for p, _, _ in dirs.values() if p == 2)
        want = dict(drNmFls=(u16(m, 0x0C), nmfls), drNmRtDirs=(u16(m, 0x52), nmrtdirs),
                    drFilCnt=(u32(m, 0x54), len(files)), drDirCnt=(u32(m, 0x58), len(dirs) - 1))
        for k, (have, should) in want.items():
            if have != should:
                self.p(f'{k} {have} != {should}')
        if ids and u32(m, 0x1E) <= max(ids):
            self.p('drNxtCNID not above all CNIDs')
        # volume bitmap
        bm = self.img[self.vbmst * BLK:]
        marked = {i for i in range(self.nmalblks) if bm[i // 8] & (0x80 >> (i % 8))}
        if marked != set(self.used):
            self.p(f'bitmap: {len(marked - set(self.used))} blocks marked but unused, '
                   f'{len(set(self.used) - marked)} used but not marked')
        free = self.nmalblks - len(marked)
        if u16(m, 0x22) != free:
            self.p(f'drFreeBks {u16(m, 0x22)} != {free}')
        nsect = len(self.img) // BLK
        alt = self.img[(nsect - 2) * BLK:(nsect - 1) * BLK]
        if self.strict_alt and alt[:162] != m[:162]:
            self.p('alternate MDB differs from the MDB')
        self.summary = (f'HFS: {len(files)} files, {len(dirs) - 1} folders, {free} of {self.nmalblks} blocks free, '
                        f'catalog depth {self.depth["catalog"][0]}, {self.depth["catalog"][1]} of '
                        f'{self.depth["catalog"][2]} nodes used')


def cmd_check(args):
    foreign = '--foreign' in args
    args = [a for a in args if a != '--foreign']
    img = open(args[0], 'rb').read()
    sig = u16(img, 1024)
    if sig == 0xD2D7:
        problems, summary = check_mfs(img)
    elif sig == 0x4244:
        c = HfsCheck(img)
        c.strict_alt = not foreign
        c.run()
        problems, summary = c.problems, getattr(c, 'summary', '')
    else:
        print(f'{args[0]}: no MFS or HFS volume')
        return 1
    for p in problems:
        print(f'{args[0]}: {p}')
    if not problems:
        print(f'{args[0]}: ok ({summary})')
    return 1 if problems else 0


def cmd_mkmany(args):
    d, n = args[0], int(args[1])
    prefix = args[2] if len(args) > 2 else 'File'
    rnd = random.Random(n)
    for i in range(1, n + 1):
        name = f'{prefix} {i:03d}'
        data = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 900)))
        rsrc = bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(0, 300)))
        with open(f'{d}/{prefix}{i:03d}.bin', 'wb') as f:
            f.write(make_macbinary(name, 'TEXT', 'ttxt', 0, data, rsrc))


def cmd_tree(args):
    c = HfsCheck(open(args[0], 'rb').read())
    c.run()
    d = c.depth['catalog']
    ext = ','.join(f'{a}+{n}' for a, n in HfsCheck.extrec(c.m, 0x96))
    print(f'depth={d[0]} nodes_used={d[1]} nodes={d[2]} extents={ext}')
    return 0


if __name__ == '__main__':
    cmds = dict(mkbin=cmd_mkbin, cmpbin=cmd_cmpbin, check=cmd_check, tree=cmd_tree, mkmany=cmd_mkmany)
    if len(sys.argv) < 3 or sys.argv[1] not in cmds:
        print(__doc__)
        sys.exit(2)
    sys.exit(cmds[sys.argv[1]](sys.argv[2:]) or 0)
