#!/usr/bin/env python3
"""Small exFAT image editor for the tests: finds entry sets, patches them, fixes checksums."""
import struct

class Img:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        bs = self.read(0, 512)
        self.bs = bs
        (self.part_off, self.vol_len, self.fat_off, self.fat_len, self.heap_off, self.clusters,
         self.root, self.serial, self.rev, self.flags, self.sshift, self.cshift, self.nfats) = \
            struct.unpack_from('<QQIIIIIIHHBBB', bs, 64)
        self.ssize = 1 << self.sshift
        self.csize = 1 << (self.sshift + self.cshift)

    def read(self, off, n):
        self.f.seek(off); return self.f.read(n)

    def write(self, off, data):
        self.f.seek(off); self.f.write(data)

    def clus_off(self, c):
        return (self.heap_off << self.sshift) + (c - 2) * self.csize

    def fat(self, c):
        return struct.unpack('<I', self.read((self.fat_off << self.sshift) + c * 4, 4))[0]

    def chain(self, first, nofat=False, count=None):
        out = []; c = first
        while c >= 2 and c <= self.clusters + 1:
            out.append(c)
            if count is not None and len(out) == count: break
            c = c + 1 if nofat else self.fat(c)
        return out

    def dir_offsets(self, first, size=None, nofat=False):
        """absolute offsets of each 32-byte slot of a directory"""
        count = None if size is None else size // self.csize
        offs = []
        for c in self.chain(first, nofat, count):
            base = self.clus_off(c)
            offs.extend(base + i for i in range(0, self.csize, 32))
        return offs

    def sets(self, first, size=None, nofat=False):
        slots = self.dir_offsets(first, size, nofat)
        i = 0
        while i < len(slots):
            t = self.read(slots[i], 1)[0]
            if t == 0: break
            if t == 0x85:
                n = self.read(slots[i], 32)[1] + 1
                offs = slots[i:i + n]
                data = b''.join(self.read(o, 32) for o in offs)
                namelen = data[32 + 3]
                name = b''.join(data[64 + k * 32 + 2: 64 + k * 32 + 32] for k in range(n - 2)).decode('utf-16le')[:namelen]
                yield name, offs, data
                i += n
            else:
                i += 1

    def find(self, path):
        first, size, nofat = self.root, None, False
        parts = [p for p in path.split('/') if p]
        for k, p in enumerate(parts):
            for name, offs, data in self.sets(first, size, nofat):
                if name == p:
                    if k == len(parts) - 1:
                        return offs, bytearray(data)
                    flags, = struct.unpack_from('<B', data, 33)
                    first, = struct.unpack_from('<I', data, 32 + 20)
                    size, = struct.unpack_from('<Q', data, 32 + 24)
                    nofat = bool(flags & 2)
                    break
            else:
                raise KeyError(path)

    @staticmethod
    def checksum(data):
        c = 0
        for i, b in enumerate(data):
            if i in (2, 3): continue
            c = (((c & 1) << 15) | (c >> 1)) + b
            c &= 0xFFFF
        return c

    def store(self, offs, data, fix=True):
        if fix:
            struct.pack_into('<H', data, 2, self.checksum(data))
        for k, o in enumerate(offs):
            self.write(o, bytes(data[k * 32:(k + 1) * 32]))

    def patch_vdl(self, path, vdl):
        offs, data = self.find(path)
        struct.pack_into('<Q', data, 32 + 8, vdl)
        self.store(offs, data)

    def patch_badsum(self, path):
        offs, data = self.find(path)
        data[2] ^= 0x55
        self.store(offs, data, fix=False)

    def patch_badboot(self):
        b = bytearray(self.read(0, 512))
        b[120] ^= 0xFF
        self.write(0, bytes(b))

    def patch_big(self, path, size):
        """grow a file in place into a contiguous stream of size bytes, with marker blocks"""
        offs, data = self.find(path)
        first, = struct.unpack_from('<I', data, 32 + 20)
        data[33] |= 3
        struct.pack_into('<Q', data, 32 + 8, size)
        struct.pack_into('<Q', data, 32 + 24, size)
        self.store(offs, data)
        for mark in (0, size // 3, 0xFFFFF000, 0x100000000 + 12345, size - 4096):
            if mark < size:
                self.write(self.clus_off(first) + mark, struct.pack('<Q', mark) * 8)

    def close(self):
        self.f.close()
