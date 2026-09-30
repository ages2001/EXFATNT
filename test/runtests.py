#!/usr/bin/env python3
"""
EXFATNT user-mode test run (Linux, as root).

Builds the driver sources together with a fake NT kernel (ntifs.h, kern.c)
and the test driver (harn.c, wtest.c), creates exFAT images with mkfs.exfat
and the exfat-fuse driver, and runs the driver against them:

  read cases     on writable and on write-protected disks
  write cases    scripted and random operations checked against a model,
                 then the image is checked with exfcheck.py, fsck.exfat and
                 read back through exfat-fuse; for every sector size and a
                 range of cluster sizes, some with memory pressure from other
                 threads (CHAOS)
  exfmt          the format tool, checked the same way
  exfatchk       the check tool: every image written above must check clean;
                 images damaged in known ways and at random must be repaired,
                 check clean afterwards, pass exfcheck.py and fsck.exfat and
                 read through exfat-fuse

Needs: gcc (with AddressSanitizer), exfatprogs, exfat-fuse, dosfstools,
       losetup. Usage: python3 runtests.py [--quick] [work directory]
"""
import os, random, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
FMT = os.path.join(HERE, '..', 'fmt')
CHK = os.path.join(HERE, '..', 'chk')
sys.path.insert(0, HERE)
import exfpatch, exfcheck

def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw).stdout.decode()

def fnv(data):
    h = 1469598103934665603
    for x in data:
        h ^= x
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

class Mounted:
    """exFAT image attached to a loop device and mounted with exfat-fuse"""
    def __init__(self, img, mnt, sector=512, ro=False):
        self.img, self.mnt, self.sector, self.ro = img, mnt, sector, ro
    def __enter__(self):
        os.makedirs(self.mnt, exist_ok=True)
        self.dev = run('losetup', '-f', '--show', '--sector-size', str(self.sector), self.img).strip()
        run('mount.exfat-fuse', *(['-o', 'ro'] if self.ro else []), self.dev, self.mnt)
        return self.mnt
    def __exit__(self, *a):
        os.sync()
        for _ in range(20):
            if subprocess.run(['fusermount', '-u', self.mnt]).returncode == 0:
                break
            time.sleep(0.2)
        run('losetup', '-d', self.dev)

def write_ref(img, mnt):
    with open(img + '.ref', 'w') as out:
        for root, dirs, files in os.walk(mnt):
            for f in files:
                p = os.path.join(root, f)
                st = os.stat(p)
                with open(p, 'rb') as fh:
                    h = fnv(fh.read())
                out.write('F %s size=%d hash=%016x mtime=%d\n' % (p[len(mnt):], st.st_size, h, int(st.st_mtime)))
        st = os.statvfs(mnt)
        out.write('statvfs free=%d bsize=%d\n' % (st.f_bfree, st.f_bsize))

def make_image(img, size, cluster, sector, label, fill):
    with open(img, 'wb') as f:
        f.truncate(size)
    dev = run('losetup', '-f', '--show', '--sector-size', str(sector), img).strip()
    try:
        run('mkfs.exfat', '-c', str(cluster), '-L', label, dev)
    finally:
        run('losetup', '-d', dev)
    mnt = img + '.mnt'
    with Mounted(img, mnt, sector) as m:
        fill(m, cluster)
    with Mounted(img, mnt, sector, ro=True) as m:
        write_ref(img, m)
    os.rmdir(mnt)

def fill_basic(m, cl):
    rnd = random.Random(1)
    def data(n):
        return bytes(rnd.getrandbits(8) for _ in range(n))
    def put(path, content):
        with open(os.path.join(m, path), 'wb') as f:
            f.write(content)
    put('small.txt', b'hello\n')
    put('empty.bin', b'')
    put('exactcluster.bin', data(cl))
    put('odd.bin', data(1000003))
    os.makedirs(os.path.join(m, 'Türkçe İçerik'))
    os.makedirs(os.path.join(m, 'Ελληνικά'))
    os.makedirs(os.path.join(m, 'Кириллица/вложенная/глубже'))
    put('Türkçe İçerik/şğüöçı.txt', 'ş ğ ı İ\n'.encode())
    put('Ελληνικά/αβγ.TXT', b'alpha\n')
    put('Кириллица/вложенная/глубже/файл.dat', b'deep\n')
    put('L' * 239 + 'ong_name_end.txt', b'long\n')
    put('smile_\U0001F600.txt', b'emoji\n')
    os.makedirs(os.path.join(m, 'many'))
    for i in range(1, 301):
        put('many/file_with_a_reasonably_long_name_%d.txt' % i, b'%d\n' % i)
    for _ in range(40):          # interleaved appends fragment both files
        with open(os.path.join(m, 'frag1.bin'), 'ab') as f:
            f.write(data(cl * 3))
        with open(os.path.join(m, 'frag2.bin'), 'ab') as f:
            f.write(data(cl))
    for i in list(range(10, 20)) + list(range(100, 200)) + [1]:
        os.remove(os.path.join(m, 'many/file_with_a_reasonably_long_name_%d.txt' % i))
    t = time.mktime((2021, 6, 15, 12, 34, 56, 0, 0, -1)) + 0.78
    os.utime(os.path.join(m, 'small.txt'), (t, t))

def fill_frag(m, cl):
    rnd = random.Random(2)
    os.makedirs(os.path.join(m, 'dirA'))
    os.makedirs(os.path.join(m, 'dirB'))
    for i in range(1, 151):      # alternate so both directories fragment
        for d, tag in (('dirA', 'alpha'), ('dirB', 'beta')):
            with open(os.path.join(m, d, '%s_file_number_%d_with_padding_to_make_the_name_longer.txt' % (tag, i)), 'w') as f:
                f.write('%s%d\n' % (tag[0].upper(), i))
    for name, n in (('vdl.bin', 100000), ('vdl2.bin', 5000)):
        with open(os.path.join(m, name), 'wb') as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(n)))

def fill_32m(m, cl):
    with open(os.path.join(m, 'small.txt'), 'w') as f:
        f.write('hello\n')
    with open(os.path.join(m, 'three.bin'), 'wb') as f:
        f.write(os.urandom(cl * 2 + 12345))
    os.makedirs(os.path.join(m, 'dir'))
    for i in range(20):
        open(os.path.join(m, 'dir', 'empty_%d.txt' % i), 'w').close()

def fill_sweep(m, cl):
    rnd = random.Random(cl)
    with open(os.path.join(m, 'small.txt'), 'w') as f:
        f.write('hello\n')
    with open(os.path.join(m, 'multi.bin'), 'wb') as f:
        f.write(os.urandom(min(cl * 2 + 123, 80 << 20)))
    os.makedirs(os.path.join(m, 'dir'))
    for i in range(200):     # empty files: the directory grows, the heap does not
        open(os.path.join(m, 'dir', 'an_empty_file_with_a_long_name_%03d.txt' % i), 'w').close()
    for i in range(3):
        for n in ('fa.bin', 'fb.bin'):
            with open(os.path.join(m, n), 'ab') as f:
                f.write(os.urandom(min(cl, 1 << 20) + rnd.randrange(100)))

def fill_big(m, cl):
    with open(os.path.join(m, 'huge.bin'), 'w') as f:
        f.write('x\n')


def read_manifest(man):
    want = {}
    for line in open(man, encoding='utf-8'):
        kind, rest = line[0], line[2:].rstrip('\n')
        if kind == 'D':
            want[rest] = None
        else:
            p, props = rest.rsplit(' size=', 1)
            size, h, attr = props.split(' ')
            want[p] = (int(size), int(h[5:], 16))
    return want

def fuse_compare(img, sector, want):
    """the tree exfat-fuse sees against what the harness wrote; problems as text"""
    mnt = img + '.mnt'
    bad = []
    with Mounted(img, mnt, sector, ro=True) as m:
        got = {}
        for root, dirs, files in os.walk(m):
            for d in dirs:
                got[os.path.join(root, d)[len(m):]] = None
            for f in files:
                p = os.path.join(root, f)
                with open(p, 'rb') as fh:
                    data = fh.read()
                got[p[len(m):]] = (len(data), fnv(data))
    os.rmdir(mnt)
    for p in want:
        if p not in got:
            bad.append('missing on fuse: ' + p)
        elif want[p] != got[p]:
            bad.append('differs on fuse: %s %s %s' % (p, want[p], got[p]))
    bad += ['extra on fuse: ' + p for p in got if p not in want]
    return bad

EXFCHKL = None      # the check tool's test front end, once built

def check_image(img, sector):
    """exfcheck.py, fsck.exfat and exfchkl; problems as text"""
    c = exfcheck.Checker(img)
    c.check()
    bad = ['exfcheck: ' + e for e in c.errors]
    if EXFCHKL:
        r = subprocess.run([EXFCHKL, img, str(sector)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            bad += ['exfchkl: ' + l for l in r.stdout.decode(errors='replace').splitlines()[:10]]
    dev = run('losetup', '-f', '--show', '--sector-size', str(sector), img).strip()
    try:
        r = subprocess.run(['fsck.exfat', '-n', dev], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            bad.append('fsck.exfat: ' + r.stdout.decode(errors='replace').strip().splitlines()[-1])
    finally:
        run('losetup', '-d', dev)
    return bad

def fuse_readable(img, sector):
    """every file and directory can be read through exfat-fuse; problems as text"""
    mnt = img + '.mnt'
    bad = []
    try:
        with Mounted(img, mnt, sector, ro=True) as m:
            for root, dirs, files in os.walk(m, onerror=lambda e: bad.append('fuse: ' + str(e))):
                for f in files:
                    try:
                        with open(os.path.join(root, f), 'rb') as fh:
                            while fh.read(1 << 20):
                                pass
                    except OSError as e:
                        bad.append('fuse: %s: %s' % (f, e))
    except subprocess.CalledProcessError as e:
        bad.append('fuse: does not mount')
    if os.path.isdir(mnt):
        os.rmdir(mnt)
    return bad

def repair_case(exfchkl, base, work, name, sector, kind, seed, count=0):
    """damage a copy of base, repair it, check it"""
    img = os.path.join(work, name + '.img')
    shutil.copy(base, img)
    args = [sys.executable, os.path.join(HERE, 'exfcorr.py'), img, kind, str(seed)] + ([str(count)] if count else [])
    what = run(*args).strip()
    if what == 'SKIP':
        return None
    first = subprocess.run([exfchkl, img, str(sector), '-q']).returncode
    fix = subprocess.run([exfchkl, img, str(sector), '-f'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    again = subprocess.run([exfchkl, img, str(sector)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    bad = []
    # random damage can land where it changes nothing (unused bytes, FAT entries of free clusters)
    if (first, fix.returncode) != (2, 1) and not (kind == 'fuzz' and (first, fix.returncode) == (0, 0)):
        bad.append('found %d, repair %d (want 2, 1)' % (first, fix.returncode))
    if again.returncode != 0:
        bad += ['again: ' + l for l in again.stdout.decode(errors='replace').splitlines()[:8]]
    c = exfcheck.Checker(img)
    c.check()
    bad += ['exfcheck: ' + e for e in c.errors[:8]]
    dev = run('losetup', '-f', '--show', '--sector-size', str(sector), img).strip()
    try:
        if subprocess.run(['fsck.exfat', '-n', dev], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            bad.append('fsck.exfat finds problems')
    finally:
        run('losetup', '-d', dev)
    bad += fuse_readable(img, sector)
    if bad:
        bad = [l for l in fix.stdout.decode(errors='replace').splitlines()[:10]] + bad
    return bad

def write_case(harn, work, name, size, cluster, sector, seed, ops, env0, extra=None, exfmtl=None, offset=0):
    """mkfs.exfat (or exfmtl, the format tool) then the write tests and every check"""
    img = os.path.join(work, name + '.img')
    man = img + '.man'
    with open(img, 'wb') as f:
        f.truncate(size)
    if exfmtl:
        r = subprocess.run([exfmtl, img, str(sector), '-q', '-c', str(cluster), '-L', 'Test Vol', '-p', str(offset)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return ['exfmtl: ' + r.stdout.decode(errors='replace').strip()]
        bad = check_image(img, sector)
        if bad:
            return bad
    else:
        dev = run('losetup', '-f', '--show', '--sector-size', str(sector), img).strip()
        try:
            run('mkfs.exfat', '-c', str(cluster), '-L', 'Test Vol', dev)
        finally:
            run('losetup', '-d', dev)
    env = dict(env0, WRITE=str(seed), OPS=str(ops), MANIFEST=man, **(extra or {}))
    r = subprocess.run([harn, img, str(sector)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = r.stdout.decode(errors='replace')
    bad = []
    if r.returncode != 0 or 'errors=0' not in out or 'FAILED' in out or 'ERROR' in out:
        bad = [l for l in out.splitlines() if not l.startswith('op:')][-10:]
    if not bad:
        bad = check_image(img, sector) + fuse_compare(img, sector, read_manifest(man))
    return bad

def main():
    global EXFCHKL
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    quick = '--quick' in sys.argv
    work = args[0] if args else tempfile.mkdtemp(prefix='exfatnt-')
    os.makedirs(work, exist_ok=True)
    harn = os.path.join(work, 'harn')
    srcs = sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.endswith('.c'))
    fmt = [os.path.join(FMT, 'exfmtc.c'), os.path.join(FMT, 'exfupc.c')]
    chk = [os.path.join(CHK, 'exfchkc.c')]
    print('building harness')
    run('gcc', '-std=gnu99', '-fshort-wchar', '-g', '-O0', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-Wall', '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-multichar',
        '-Wno-pointer-sign', '-Wno-unused-function', '-DEXF_DEBUG', '-I' + HERE, '-I' + SRC, '-o', harn,
        os.path.join(HERE, 'harn.c'), os.path.join(HERE, 'kern.c'), *srcs, *fmt, *chk)

    exfchkl = os.path.join(work, 'exfchkl')
    run('gcc', '-std=gnu99', '-g', '-Wall', '-fsanitize=address,undefined', '-o', exfchkl,
        os.path.join(HERE, 'exfchkl.c'), *chk, os.path.join(FMT, 'exfupc.c'))
    EXFCHKL = exfchkl

    img = lambda n: os.path.join(work, n)
    print('creating images')
    make_image(img('a.img'), 64 << 20, 4096, 512, 'Test Vol', fill_basic)
    make_image(img('b.img'), 128 << 20, 32768, 512, 'Test Vol', fill_basic)
    make_image(img('c.img'), 64 << 20, 4096, 4096, 'Test Vol', fill_basic)

    make_image(img('d.img'), 32 << 20, 512, 512, 'Frag', fill_frag)
    exfpatch.Img(img('d.img')).patch_vdl('/vdl.bin', 30000)
    exfpatch.Img(img('d.img')).patch_vdl('/vdl2.bin', 0)
    with Mounted(img('d.img'), img('d.mnt'), ro=True) as m:
        write_ref(img('d.img'), m)
    os.rmdir(img('d.mnt'))

    make_image(img('m.img'), 1 << 30, 32 << 20, 512, 'Test Vol', fill_32m)
    make_image(img('e.img'), 6 << 30, 131072, 512, 'Big', fill_big)
    os.remove(img('e.img') + '.ref')
    exfpatch.Img(img('e.img')).patch_big('/huge.bin', 4831838208)

    with open(img('f.img'), 'wb') as f:
        f.truncate(64 << 20)
    run('mkfs.vfat', '-F', '32', img('f.img'))

    def copy(src, dst, drop=None):
        shutil.copy(img(src), img(dst))
        with open(img(src) + '.ref') as fi, open(img(dst) + '.ref', 'w') as fo:
            for line in fi:
                if drop is None or drop not in line:
                    fo.write(line)

    copy('a.img', 'g.img', drop='statvfs')
    exfpatch.Img(img('g.img')).patch_badboot()
    copy('a.img', 'h.img', drop='/small.txt ')
    exfpatch.Img(img('h.img')).patch_badsum('/small.txt')
    copy('a.img', 's1.img')
    shutil.copy(img('b.img'), img('s2.img'))
    copy('a.img', 'p.img')

    cases = [
        ('image a', {}, 'a.img', 512),
        ('image b (32 KB clusters)', {}, 'b.img', 512),
        ('image c (4 KB sectors)', {}, 'c.img', 4096),
        ('32 MB clusters', {'ONLYWALK': '1'}, 'm.img', 512),
        ('fragmented directories, ValidDataLength', {'LABEL': 'Frag', 'ONLYWALK': '1'}, 'd.img', 512),
        ('4.5 GB file', {'LABEL': 'Big', 'BIG': '\\huge.bin:4831838208'}, 'e.img', 512),
        ('backup boot region', {'ONLYWALK': '1'}, 'g.img', 512),
        ('bad entry set skipped', {'ONLYWALK': '1'}, 'h.img', 512),
        ('media change', {'SWAP': img('s2.img')}, 's1.img', 512),
        ('PnP removal', {'PNP': '1'}, 'p.img', 512),
    ]
    sweep = []
    for sector in (512, 1024, 2048, 4096):
        c = max(sector, 512)
        while c <= 32 << 20:
            name = 'sw%d_%d.img' % (sector, c)
            make_image(img(name), max(64 << 20, c * 16), c, sector, 'Test Vol', fill_sweep)
            sweep.append(('cluster %d, sector %d' % (c, sector), {'ONLYWALK': '1'}, name, sector))
            c *= 2
    cases += sweep

    failed = 0
    env0 = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    for name, env, image, sector in cases:
        r = subprocess.run([harn, img(image), str(sector)], env=dict(env0, **env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode(errors='replace')
        ok = r.returncode == 0 and 'errors=0' in out and 'FAILED' not in out and 'ERROR' not in out
        print('%s %s' % ('PASS' if ok else 'FAIL', name))
        if not ok:
            failed += 1
            print('\n'.join(out.splitlines()[-10:]))
    r = subprocess.run([harn, img('f.img'), '512'], env=dict(env0, ONLYWALK='1'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    ok = b'c000014f' in r.stdout
    print('%s FAT32 volume not mounted' % ('PASS' if ok else 'FAIL'))
    failed += not ok

    # the same read cases on a write-protected disk: nothing may be written
    for name, env, image, sector in cases[:3] + [cases[4]]:
        r = subprocess.run([harn, img(image), str(sector)], env=dict(env0, RO='1', **env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode(errors='replace')
        ok = r.returncode == 0 and 'errors=0' in out and 'FAILED' not in out and 'ERROR' not in out
        print('%s %s, write-protected' % ('PASS' if ok else 'FAIL', name))
        if not ok:
            failed += 1
            print('\n'.join(out.splitlines()[-10:]))

    # EnableWriteSupport = 0 on a writable disk: the same as a write-protected one
    for name, env, image, sector in cases[:3]:
        r = subprocess.run([harn, img(image), str(sector)], env=dict(env0, REGRO='1', **env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode(errors='replace')
        ok = r.returncode == 0 and 'errors=0' in out and 'FAILED' not in out and 'ERROR' not in out
        print('%s %s, EnableWriteSupport = 0' % ('PASS' if ok else 'FAIL', name))
        if not ok:
            failed += 1
            print('\n'.join(out.splitlines()[-10:]))

    # writing: (name, size, cluster, sector, seed, operations, extra environment)
    wcases = [
        ('w512', 128 << 20, 512, 512, 1, 3000, None),
        ('w4k', 128 << 20, 4096, 512, 2, 3000, None),
        ('w32k', 256 << 20, 32768, 512, 3, 3000, None),
        ('w4ks', 128 << 20, 4096, 4096, 4, 3000, None),
        ('w2ks', 128 << 20, 16384, 2048, 5, 2000, None),
        ('w1ks', 128 << 20, 1024, 1024, 6, 2000, None),
        ('w128k', 512 << 20, 131072, 512, 7, 2000, None),
        ('w1m', 1 << 30, 1 << 20, 512, 8, 1000, None),
        ('w32m', 2 << 30, 32 << 20, 512, 9, 300, {'NOSCRIPT': '1'}),
        ('chaos4k', 128 << 20, 4096, 512, 10, 2000, {'CHAOS': '3'}),
        ('chaos4ks', 128 << 20, 4096, 4096, 11, 1500, {'CHAOS': '3'}),
        ('chaos64k', 128 << 20, 65536, 512, 12, 1500, {'CHAOS': '5'}),
    ]
    # formatted again while mounted, with a file open and dirty data (as exfmt /X does)
    wcases += [
        ('reformat', 128 << 20, 4096, 512, 13, 1000, {'REFORMAT': '65536', 'CHAOS': '5'}),
        ('reformat4ks', 128 << 20, 32768, 4096, 14, 1000, {'REFORMAT': '4096'}),
    ]
    if quick:
        wcases = [c[:5] + (min(c[5], 500),) + c[6:] for c in wcases if c[0] in ('w512', 'w4ks', 'chaos4k', 'w32m', 'reformat')]
    for name, size, cl, sector, seed, ops, extra in wcases:
        bad = write_case(harn, work, name, size, cl, sector, seed, ops, env0, extra)
        print('%s write, cluster %d, sector %d, seed %d, %d operations%s' % ('FAIL' if bad else 'PASS', cl, sector, seed, ops,
              ', ' + ' '.join('%s=%s' % kv for kv in extra.items()) if extra else ''))
        if bad:
            failed += 1
            print('\n'.join(bad[:15]))
    # the format tool: every sector size and cluster size, then the write tests on the result
    exfmtl = os.path.join(work, 'exfmtl')
    run('gcc', '-std=gnu99', '-g', '-Wall', '-fsanitize=address,undefined', '-o', exfmtl,
        os.path.join(HERE, 'exfmtl.c'), *fmt)
    fcases = []
    for sector in (512, 1024, 2048, 4096):
        for cl in (512, 4096, 32768, 131072, 1 << 20, 32 << 20):
            if cl >= sector and not (quick and cl not in (4096, 32 << 20)):
                size = 2 << 30 if cl == 32 << 20 else 256 << 20
                fcases.append(('fmt%d_%d' % (sector, cl), size, cl, sector, cl % 97 + sector, 100 if cl == 32 << 20 else 300,
                               {'NOSCRIPT': '1'} if cl >= 1 << 20 else None, 63 if sector == 512 else 0))
    for name, size, cl, sector, seed, ops, extra, offset in fcases:
        bad = write_case(harn, work, name, size, cl, sector, seed, ops, env0, extra, exfmtl, offset)
        print('%s exfmt, cluster %d, sector %d, then %d operations' % ('FAIL' if bad else 'PASS', cl, sector, ops))
        if bad:
            failed += 1
            print('\n'.join(bad[:15]))
    # the check tool: known damage, then random damage in the metadata
    bases = []
    for name, size, cl, sector, seed, ops in (('cb512', 64 << 20, 512, 512, 21, 600), ('cb4k', 128 << 20, 4096, 512, 22, 800),
                                             ('cb4ks', 128 << 20, 4096, 4096, 23, 600), ('cb32k', 256 << 20, 32768, 512, 24, 600)):
        bad = write_case(harn, work, name, size, cl, sector, seed, ops, env0, {'NOSCRIPT': '1'})
        print('%s check base %s: cluster %d, sector %d' % ('FAIL' if bad else 'PASS', name, cl, sector))
        if bad:
            failed += 1
            print('\n'.join(bad[:15]))
        else:
            bases.append((os.path.join(work, name + '.img'), sector))
    kinds = run(sys.executable, os.path.join(HERE, 'exfcorr.py'), bases[0][0], 'list').split()
    for kind in kinds:
        tried = bad_kind = 0
        for k, (base, sector) in enumerate(bases[:1] if quick else bases):
            bad = repair_case(exfchkl, base, work, 'rc', sector, kind, k + 1)
            if bad is None:
                continue
            tried += 1
            if bad:
                bad_kind += 1
                print('FAIL repair %s on %s' % (kind, os.path.basename(base)))
                print('\n'.join(bad[:15]))
        failed += bad_kind
        if not bad_kind:
            print('PASS repair %s (%d images)' % (kind, tried))
    fuzz_ok = fuzz_bad = 0
    for seed in range(1, 21 if quick else 161):
        base, sector = bases[seed % len(bases)]
        bad = repair_case(exfchkl, base, work, 'rf', sector, 'fuzz', seed, 10 + seed % 50)
        if bad:
            fuzz_bad += 1
            print('FAIL random damage, seed %d on %s' % (seed, os.path.basename(base)))
            print('\n'.join(bad[:15]))
        else:
            fuzz_ok += 1
    failed += fuzz_bad
    print('%s random damage repaired: %d of %d' % ('FAIL' if fuzz_bad else 'PASS', fuzz_ok, fuzz_ok + fuzz_bad))

    print('work directory: %s' % work)
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
