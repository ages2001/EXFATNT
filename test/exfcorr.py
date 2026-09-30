#!/usr/bin/env python3
"""
Damages an exFAT image in one chosen way, for testing exfatchk.

Usage: exfcorr.py IMAGE KIND [SEED]
       exfcorr.py IMAGE fuzz SEED [COUNT]   random bytes in the metadata
       exfcorr.py IMAGE list                the kinds

Prints what it did (the path of the file hit, when there is one).
"""
import random, struct, sys
import exfcheck


def set_checksum(data):
    return exfcheck.set_checksum(data)


class Img(exfcheck.Checker):
    def __init__(self, path):
        super().__init__(path)
        self.f = open(path, 'r+b')
        self.check_boot()
        self.upcase = None

    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def fat_get(self, c):
        return struct.unpack('<I', self.read((self.fat_off << self.sshift) + c * 4, 4))[0]

    def fat_set(self, c, v):
        self.write((self.fat_off << self.sshift) + c * 4, struct.pack('<I', v))

    def clusters_of(self, first, nofat, length, to_end=False):
        if first == 0:
            return []
        n = (length + self.csize - 1) // self.csize
        if nofat:
            return list(range(first, first + n))
        out, c = [], first
        while c >= 2 and c <= self.clusters + 1 and (to_end or len(out) < n) and len(out) <= self.clusters:
            out.append(c)
            nxt = self.fat_get(c)
            if nxt == 0xFFFFFFFF:
                break
            c = nxt
        return out

    def entry_off(self, dcl, index):
        per = self.csize // 32
        return self.coff(dcl[index // per]) + (index % per) * 32

    def sets(self):
        """every file entry set: dict with path, dir clusters, index, fields"""
        out, todo = [], [(self.clusters_of(self.root, False, 0, True), '', True)]
        root_entries = []
        while todo:
            dcl, path, is_root = todo.pop(0)
            n = len(dcl) * self.csize // 32
            i = 0
            while i < n:
                e = self.read(self.entry_off(dcl, i), 32)
                if e[0] == 0:
                    break
                if is_root and e[0] in (0x81, 0x82, 0x83):
                    root_entries.append((e[0], dcl, i, e))
                if e[0] != 0x85:
                    i += 1
                    continue
                sec = e[1]
                raw = b''.join(self.read(self.entry_off(dcl, i + k), 32) for k in range(sec + 1))
                st = raw[32:64]
                nlen = st[3]
                name = b''.join(raw[64 + k * 32 + 2: 64 + k * 32 + 32] for k in range((nlen + 14) // 15))[:nlen * 2]
                name = name.decode('utf-16le', 'surrogatepass')
                attr = struct.unpack_from('<H', raw, 4)[0]
                s = dict(path=path + '/' + name, name=name, dcl=dcl, index=i, sec=sec, raw=raw,
                         attr=attr, dir=bool(attr & 0x10), flags=st[1], vdl=struct.unpack_from('<Q', st, 8)[0],
                         first=struct.unpack_from('<I', st, 20)[0], dlen=struct.unpack_from('<Q', st, 24)[0])
                out.append(s)
                if s['dir'] and s['first']:
                    todo.append((self.clusters_of(s['first'], s['flags'] & 2, s['dlen']), s['path'], False))
                i += sec + 1
        self.root_entries = root_entries
        return out

    def put_set(self, s, raw, fix_sum=True):
        raw = bytearray(raw)
        if fix_sum:
            struct.pack_into('<H', raw, 2, set_checksum(bytes(raw)))
        for k in range(len(raw) // 32):
            self.write(self.entry_off(s['dcl'], s['index'] + k), bytes(raw[k * 32:(k + 1) * 32]))

    def bitmap_clusters(self):
        for t, dcl, i, e in self.root_entries:
            if t == 0x81:
                first, length = struct.unpack_from('<IQ', e, 20)
                return self.clusters_of(first, False, length)

    def bit(self, c, value=None):
        bm = self.bitmap_clusters()
        b = c - 2
        off = self.coff(bm[b // 8 // self.csize]) + (b // 8) % self.csize
        byte = self.read(off, 1)[0]
        if value is None:
            return (byte >> (b % 8)) & 1
        byte = (byte | (1 << (b % 8))) if value else (byte & ~(1 << (b % 8)))
        self.write(off, bytes([byte]))


def pick(rnd, sets, cond):
    c = [s for s in sets if cond(s)]
    if not c:
        return None
    return rnd.choice(c)


def corrupt(img, kind, rnd):
    sets = img.sets()
    files = lambda s: not s['dir'] and s['first'] and s['dlen']
    chained = lambda s: files(s) and not (s['flags'] & 2) and s['dlen'] > 2 * img.csize
    if kind == 'leak':
        free = [c for c in range(2, img.clusters + 2) if not img.bit(c)]
        for c in rnd.sample(free, min(5, len(free))):
            img.bit(c, 1)
        return 'bitmap'
    if kind == 'missing':
        s = pick(rnd, sets, files)
        img.bit(s['first'], 0)
        return s['path']
    if kind == 'bootmain':
        img.write(rnd.randrange(120, 400), bytes([rnd.randrange(256)]))
        return 'boot'
    if kind == 'bootbackup':
        img.write(12 * img.ssize + 100, b'\x99\x99\x99\x99')
        return 'boot'
    if kind == 'setsum':
        s = pick(rnd, sets, lambda s: True)
        raw = bytearray(s['raw']); raw[8] ^= 0x55
        img.put_set(s, raw, fix_sum=False)
        return s['path']
    if kind == 'hash':
        s = pick(rnd, sets, lambda s: True)
        raw = bytearray(s['raw']); raw[32 + 4] ^= 0x5A
        img.put_set(s, raw)
        return s['path']
    if kind == 'vdl':
        s = pick(rnd, sets, files)
        raw = bytearray(s['raw']); struct.pack_into('<Q', raw, 32 + 8, s['dlen'] + 12345)
        img.put_set(s, raw)
        return s['path']
    if kind in ('short', 'badnext', 'long'):
        s = pick(rnd, sets, chained)
        if s is None:
            return None
        cl = img.clusters_of(s['first'], False, s['dlen'])
        if kind == 'short':
            img.fat_set(cl[len(cl) // 2], 0xFFFFFFFF)
        elif kind == 'badnext':
            img.fat_set(cl[len(cl) // 2], img.clusters + 50)
        else:
            free = [c for c in range(2, img.clusters + 2) if not img.bit(c)]
            img.fat_set(cl[-1], free[0])
            img.fat_set(free[0], 0xFFFFFFFF)
        return s['path']
    if kind == 'cross':
        a = pick(rnd, sets, chained)
        b = pick(rnd, sets, lambda s: files(s) and s is not a and s['path'] != (a or {}).get('path'))
        if a is None or b is None:
            return None
        cl = img.clusters_of(a['first'], False, a['dlen'])
        img.fat_set(cl[len(cl) // 2], b['first'])
        return a['path'] + ' ' + b['path']
    if kind == 'crossnofat':
        a = pick(rnd, sets, lambda s: files(s) and (s['flags'] & 2))
        b = pick(rnd, sets, lambda s: files(s) and s is not a)
        if a is None or b is None:
            return None
        raw = bytearray(a['raw']); struct.pack_into('<I', raw, 32 + 20, b['first'])
        img.put_set(a, raw)
        return a['path']
    if kind == 'nocluster':
        s = pick(rnd, sets, files)
        raw = bytearray(s['raw']); struct.pack_into('<I', raw, 32 + 20, 0)
        img.put_set(s, raw)
        return s['path']
    if kind == 'seccount':
        s = pick(rnd, sets, lambda s: not s['dir'])
        raw = bytearray(s['raw'][:32]); raw[1] = 1
        img.put_set(s, raw, fix_sum=False)
        return s['path']
    if kind == 'stream':
        s = pick(rnd, sets, lambda s: not s['dir'])
        raw = bytearray(s['raw']); raw[32] = 0xC5
        img.put_set(s, raw)
        return s['path']
    if kind == 'aftereod':
        s = pick(rnd, sets, lambda s: s['index'] > 3 and not s['dir'])
        raw = bytearray(32)
        img.write(img.entry_off(s['dcl'], s['index'] - 1), bytes(raw))
        return s['path']
    if kind == 'dupname':
        by_dir = {}
        for s in sets:
            by_dir.setdefault(tuple(s['dcl']), []).append(s)
        cands = [(a, b) for v in by_dir.values() for a in v for b in v
                 if a is not b and not a['dir'] and not b['dir'] and len(a['name']) == len(b['name'])]
        if not cands:
            return None
        a, b = rnd.choice(cands)
        raw = bytearray(b['raw'])
        for k in range((len(a['name']) + 14) // 15):
            raw[64 + k * 32:96 + k * 32] = a['raw'][64 + k * 32:96 + k * 32]
        raw[32 + 4:32 + 6] = a['raw'][32 + 4:32 + 6]
        img.put_set(b, raw)
        return b['path']
    if kind == 'badchar':
        s = pick(rnd, sets, lambda s: not s['dir'])
        raw = bytearray(s['raw']); raw[64 + 2] = ord('*'); raw[64 + 3] = 0
        img.put_set(s, raw)
        return s['path']
    if kind == 'dirty':
        b = bytearray(img.read(106, 2)); b[0] |= 2
        img.write(106, bytes(b))
        return 'boot'
    if kind == 'percent':
        img.write(112, bytes([77]))
        return 'boot'
    if kind == 'fat01':
        img.fat_set(0, 0x12345678)
        return 'fat'
    if kind == 'upcase':
        for t, dcl, i, e in img.root_entries:
            if t == 0x82:
                first = struct.unpack_from('<I', e, 20)[0]
                img.write(img.coff(first) + 200, b'\x41\x00\x42\x00')
        return 'upcase'
    if kind == 'upcasesum':
        for t, dcl, i, e in img.root_entries:
            if t == 0x82:
                img.write(img.entry_off(dcl, i) + 4, b'\x01\x02\x03\x04')
        return 'upcase'
    if kind == 'label':
        for t, dcl, i, e in img.root_entries:
            if t == 0x83:
                img.write(img.entry_off(dcl, i) + 1, bytes([15]))
        return 'label'
    if kind == 'dirbad':
        s = pick(rnd, sets, lambda s: s['dir'] and s['first'])
        if s is None:
            return None
        raw = bytearray(s['raw']); struct.pack_into('<I', raw, 32 + 20, img.clusters + 99)
        img.put_set(s, raw)
        return s['path']
    if kind == 'dirsize':
        s = pick(rnd, sets, lambda s: s['dir'] and s['first'])
        if s is None:
            return None
        raw = bytearray(s['raw']); struct.pack_into('<Q', raw, 32 + 8, s['dlen'] - 100)
        img.put_set(s, raw)
        return s['path']
    if kind == 'allocflag':
        s = pick(rnd, sets, files)
        raw = bytearray(s['raw']); raw[33] &= ~1
        img.put_set(s, raw)
        return s['path']
    raise SystemExit('unknown kind ' + kind)


KINDS = ['leak', 'missing', 'bootmain', 'bootbackup', 'setsum', 'hash', 'vdl', 'short', 'badnext', 'long',
         'cross', 'crossnofat', 'nocluster', 'seccount', 'stream', 'aftereod', 'dupname', 'badchar', 'dirty',
         'percent', 'fat01', 'upcase', 'upcasesum', 'label', 'dirbad', 'dirsize', 'allocflag']


def fuzz(img, rnd, count):
    """random bytes in metadata: boot region, FAT, bitmap, directories"""
    sets = img.sets()
    regions = [(0, 24 * img.ssize), ((img.fat_off << img.sshift), 64 * 1024)]
    dirs = set()
    for s in sets:
        for c in s['dcl']:
            dirs.add(c)
    for c in img.clusters_of(img.root, False, 0, True):
        dirs.add(c)
    for c in list(dirs):
        regions.append((img.coff(c), img.csize))
    for c in (img.bitmap_clusters() or [])[:2]:
        regions.append((img.coff(c), min(img.csize, 4096)))
    for _ in range(count):
        base, size = rnd.choice(regions)
        off = base + rnd.randrange(size)
        img.write(off, bytes([rnd.randrange(256)]))


def main():
    img = Img(sys.argv[1])
    kind = sys.argv[2]
    if kind == 'list':
        print(' '.join(KINDS))
        return
    rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    if kind == 'fuzz':
        fuzz(img, rnd, int(sys.argv[4]) if len(sys.argv) > 4 else 20)
        print('fuzz')
        return
    what = corrupt(img, kind, rnd)
    print(what if what else 'SKIP')


if __name__ == '__main__':
    main()
