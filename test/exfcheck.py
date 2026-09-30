#!/usr/bin/env python3
"""
Strict exFAT consistency check for the tests (fsck.exfat -n misses some
of these): boot region and backup, up-case table, every entry set
(checksum, name hash, layout, end-of-directory rule, duplicate names),
cluster chains (length, end mark, cross links) and the allocation bitmap
against the clusters actually in use.

Usage: exfcheck.py IMAGE [--allow-dirty]
"""
import struct, sys


def boot_checksum(region, ssize):
    c = 0
    for i in range(ssize * 11):
        if i in (106, 107, 112):
            continue
        c = ((c >> 1) | ((c & 1) << 31)) + region[i]
        c &= 0xFFFFFFFF
    return c


def set_checksum(data):
    c = 0
    for i, b in enumerate(data):
        if i in (2, 3):
            continue
        c = (((c & 1) << 15) | (c >> 1)) + b
        c &= 0xFFFF
    return c


def table_checksum(data):
    c = 0
    for b in data:
        c = (((c & 1) << 31) | (c >> 1)) + b
        c &= 0xFFFFFFFF
    return c


class Checker:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.errors = []
        bs = self.read(0, 512)
        (self.part_off, self.vol_len, self.fat_off, self.fat_len, self.heap_off, self.clusters,
         self.root, self.serial, self.rev, self.flags, self.sshift, self.cshift, self.nfats,
         self.drive, self.percent) = struct.unpack_from('<QQIIIIIIHHBBBBB', bs, 64)
        self.ssize = 1 << self.sshift
        self.csize = 1 << (self.sshift + self.cshift)
        self.used = bytearray(self.clusters + 2)
        self.owner = {}

    def err(self, msg):
        self.errors.append(msg)

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def coff(self, c):
        return (self.heap_off << self.sshift) + (c - 2) * self.csize

    # --- boot region ---
    def check_boot(self):
        main = self.read(0, 12 * self.ssize)
        backup = self.read(12 * self.ssize, 12 * self.ssize)
        for name, reg in (('main', main), ('backup', backup)):
            want = boot_checksum(reg, self.ssize)
            sums = struct.unpack_from('<%dI' % (self.ssize // 4), reg, 11 * self.ssize)
            if any(s != want for s in sums):
                self.err('%s boot region checksum' % name)
        if main[:106] != backup[:106] or main[108:112] != backup[108:112] or main[113:11 * self.ssize] != backup[113:11 * self.ssize]:
            self.err('backup boot region differs from main')
        if self.flags & 2:
            self.err('VolumeDirty set')
        fat = self.read(self.fat_off << self.sshift, (self.clusters + 2) * 4)
        self.fat = struct.unpack('<%dI' % (self.clusters + 2), fat)
        if self.fat[0] != 0xFFFFFFF8 or self.fat[1] != 0xFFFFFFFF:
            self.err('FAT[0..1] = %08x %08x' % (self.fat[0], self.fat[1]))

    # --- chains ---
    def chain(self, first, nofat, length, what, until_end=False):
        if first == 0:
            if length:
                self.err('%s: no first cluster for %d bytes' % (what, length))
            return []
        n = (length + self.csize - 1) // self.csize
        out = []
        c = first
        if nofat:
            out = list(range(first, first + n))
            if first + n - 1 > self.clusters + 1:
                self.err('%s: contiguous run past the heap' % what)
                out = [x for x in out if x <= self.clusters + 1]
        else:
            seen = set()
            while True:
                if c < 2 or c > self.clusters + 1:
                    self.err('%s: bad cluster %#x in chain' % (what, c))
                    break
                if c in seen:
                    self.err('%s: chain loops' % what)
                    break
                seen.add(c)
                out.append(c)
                if not until_end and len(out) == n:
                    if self.fat[c] != 0xFFFFFFFF:
                        self.err('%s: chain does not end after %d clusters (next %#x)' % (what, n, self.fat[c]))
                    break
                nxt = self.fat[c]
                if nxt == 0xFFFFFFFF:
                    if not until_end:
                        self.err('%s: chain ends after %d of %d clusters' % (what, len(out), n))
                    break
                c = nxt
        for x in out:
            if self.used[x]:
                self.err('%s: cluster %d also used by %s' % (what, x, self.owner.get(x)))
            self.used[x] = 1
            self.owner[x] = what
        return out

    def dir_entries(self, clusters):
        data = b''.join(self.read(self.coff(c), self.csize) for c in clusters)
        return [data[i:i + 32] for i in range(0, len(data), 32)]

    # --- directories ---
    def walk(self, clusters, path, is_root):
        ents = self.dir_entries(clusters)
        names = {}
        i = 0
        ended = False
        children = []
        while i < len(ents):
            e = ents[i]
            t = e[0]
            if ended:
                if t != 0:
                    self.err('%s: entry %#x after end of directory at %d' % (path, t, i))
                i += 1
                continue
            if t == 0:
                ended = True
                i += 1
                continue
            if not t & 0x80:
                i += 1
                continue
            if is_root and t in (0x81, 0x82, 0x83):
                if t == 0x81:
                    first, length = struct.unpack_from('<IQ', e, 20)
                    self.bitmap_first, self.bitmap_len = first, length
                    self.bitmap_clusters = self.chain(first, False, length, 'bitmap')
                elif t == 0x82:
                    csum, = struct.unpack_from('<I', e, 4)
                    first, length = struct.unpack_from('<IQ', e, 20)
                    cl = self.chain(first, False, length, 'upcase')
                    data = b''.join(self.read(self.coff(c), self.csize) for c in cl)[:length]
                    if table_checksum(data) != csum:
                        self.err('upcase table checksum')
                    self.load_upcase(data)
                elif t == 0x83:
                    if e[1] > 11:
                        self.err('label longer than 11')
                i += 1
                continue
            if t != 0x85:
                if t in (0xC0, 0xC1):
                    self.err('%s: stray secondary entry %#x at %d' % (path, t, i))
                i += 1
                continue
            sec = e[1]
            if sec < 2 or sec > 18 or i + sec >= len(ents):
                self.err('%s: bad secondary count %d at %d' % (path, sec, i))
                i += 1
                continue
            s = b''.join(ents[i:i + sec + 1])
            if struct.unpack_from('<H', s, 2)[0] != set_checksum(s):
                self.err('%s: set checksum at %d' % (path, i))
            attr, = struct.unpack_from('<H', s, 4)
            st = s[32:64]
            if st[0] != 0xC0:
                self.err('%s: no stream entry at %d' % (path, i))
                i += sec + 1
                continue
            flags, nlen, nhash = st[1], st[3], struct.unpack_from('<H', st, 4)[0]
            vdl, = struct.unpack_from('<Q', st, 8)
            first, = struct.unpack_from('<I', st, 20)
            dlen, = struct.unpack_from('<Q', st, 24)
            nents = (nlen + 14) // 15
            if nlen == 0 or nents > sec - 1:
                self.err('%s: name length %d with %d secondaries' % (path, nlen, sec))
                i += sec + 1
                continue
            raw = b''
            for k in range(nents):
                ne = s[64 + k * 32: 96 + k * 32]
                if ne[0] != 0xC1:
                    self.err('%s: name entry type %#x' % (path, ne[0]))
                raw += ne[2:32]
            name = raw[:nlen * 2].decode('utf-16le', 'surrogatepass')
            for k in range(nents + 2, sec + 1):
                if s[k * 32] & 0xE0 != 0xE0:
                    self.err('%s/%s: unexpected secondary %#x' % (path, name, s[k * 32]))
            if (flags & 1) == 0:
                self.err('%s/%s: AllocationPossible clear' % (path, name))
            if self.upcase is not None and self.name_hash(name) != nhash:
                self.err('%s/%s: name hash' % (path, name))
            key = self.upper(name)
            if key in names:
                self.err('%s: duplicate name %s' % (path, name))
            names[key] = 1
            if vdl > dlen:
                self.err('%s/%s: VDL %d > DataLength %d' % (path, name, vdl, dlen))
            if (first == 0) != (dlen == 0):
                self.err('%s/%s: first cluster %d with DataLength %d' % (path, name, first, dlen))
            if first == 0 and flags & 2:
                self.err('%s/%s: NoFatChain without clusters' % (path, name))
            full = path + '/' + name
            cl = self.chain(first, bool(flags & 2), dlen, full)
            if attr & 0x10:
                if dlen == 0 or dlen % self.csize or vdl != dlen:
                    self.err('%s: directory sizes %d/%d' % (full, dlen, vdl))
                children.append((cl, full))
            i += sec + 1
        for cl, full in children:
            self.walk(cl, full, False)

    def load_upcase(self, data):
        src = struct.unpack('<%dH' % (len(data) // 2), data)
        table = list(range(65536))
        c = 0
        i = 0
        while i < len(src) and c < 65536:
            if src[i] == 0xFFFF and i + 1 < len(src):
                c += src[i + 1]
                i += 2
            else:
                table[c] = src[i]
                c += 1
                i += 1
        self.upcase = table

    def upper(self, name):
        u = name.encode('utf-16le', 'surrogatepass')
        units = struct.unpack('<%dH' % (len(u) // 2), u)
        if self.upcase is None:
            return units
        return tuple(self.upcase[x] for x in units)

    def name_hash(self, name):
        h = 0
        for ch in self.upper(name):
            for b in (ch & 0xFF, ch >> 8):
                h = (((h & 1) << 15) | (h >> 1)) + b
                h &= 0xFFFF
        return h

    def check(self):
        self.check_boot()
        self.upcase = None
        self.bitmap_clusters = None
        root = self.chain(self.root, False, 0, 'root', until_end=True)
        # the up-case and bitmap entries come first; hashes are checked once the table is known
        self.walk(root, '', True)
        if self.bitmap_clusters is None:
            self.err('no allocation bitmap')
            return
        bm = b''.join(self.read(self.coff(c), self.csize) for c in self.bitmap_clusters)
        leaked = missing = 0
        first_leak = first_missing = None
        for c in range(2, self.clusters + 2):
            bit = (bm[(c - 2) // 8] >> ((c - 2) % 8)) & 1
            if bit and not self.used[c]:
                leaked += 1
                first_leak = first_leak or c
            elif self.used[c] and not bit:
                missing += 1
                first_missing = first_missing or c
        if leaked:
            self.err('%d clusters marked in the bitmap but unused (first %d)' % (leaked, first_leak))
        if missing:
            self.err('%d clusters in use but free in the bitmap (first %d)' % (missing, first_missing))
        used = sum(self.used)
        if self.percent != 0xFF and not (self.flags & 2):
            want = used * 100 // self.clusters
            if abs(self.percent - want) > 1:
                self.err('PercentInUse %d, %d%% in use' % (self.percent, want))
        return used


def main():
    c = Checker(sys.argv[1])
    used = c.check()
    errs = [e for e in c.errors if not ('--allow-dirty' in sys.argv and e == 'VolumeDirty set')]
    for e in errs[:40]:
        print('exfcheck:', e)
    print('exfcheck: %s, %s clusters used of %d' % ('clean' if not errs else '%d problems' % len(errs), used, c.clusters))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
