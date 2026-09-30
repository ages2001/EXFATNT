/*
 * Write tests, included by harn.c. Scripted cases first, then random
 * operations checked against an in-memory model of the tree, with lazy
 * writes, memory pressure and remounts in between. Ends with a manifest
 * of the tree for runtests.py to compare with fsck.exfat and exfat-fuse.
 */

static PDRIVER_OBJECT w_drv;
static int g_log;
#define LOG(args) do { if (g_log) { fprintf(stderr, "op: "); fprintf args; } } while (0)
static int w_base_pool, w_base_dev;

/* ---------------- requests ---------------- */

struct wr { LONGLONG off; ULONG len; PVOID buf; ULONG flags; };
static void SetupWrite(PIRP i, PIO_STACK_LOCATION sp, void *a)
{
    struct wr *w = a;
    sp->Parameters.Write.ByteOffset.QuadPart = w->off; sp->Parameters.Write.Length = w->len;
    i->UserBuffer = w->buf; i->Flags = w->flags;
}
static NTSTATUS Write(PFILE_OBJECT f, LONGLONG off, ULONG len, const void *buf, BOOLEAN nocache, ULONG_PTR *info)
{
    PIRP i; NTSTATUS st; struct wr w; ULONG_PTR dummy;
    w.off = off; w.len = len; w.buf = (PVOID)buf; w.flags = nocache ? IRP_NOCACHE : 0;
    Simple(f, IRP_MJ_WRITE, 0, &i, SetupWrite, &w);
    T(i->Completed); st = i->IoStatus.Status; *(info ? info : &dummy) = i->IoStatus.Information; IoFreeIrp(i);
    Balanced("write");
    return st;
}

struct si { ULONG cls; PVOID buf; ULONG len; PFILE_OBJECT target; BOOLEAN replace, advance; };
static void SetupSi(PIRP i, PIO_STACK_LOCATION sp, void *a)
{
    struct si *q = a;
    sp->Parameters.SetFile.Length = q->len; sp->Parameters.SetFile.FileInformationClass = q->cls;
    sp->Parameters.SetFile.FileObject = q->target; sp->Parameters.SetFile.ReplaceIfExists = q->replace;
    sp->Parameters.SetFile.AdvanceOnly = q->advance; i->AssociatedIrp.SystemBuffer = q->buf;
}
static NTSTATUS SetInfo(PFILE_OBJECT f, ULONG cls, PVOID buf, ULONG len, PFILE_OBJECT target, BOOLEAN replace)
{
    PIRP i; NTSTATUS st; struct si q;
    q.cls = cls; q.buf = buf; q.len = len; q.target = target; q.replace = replace; q.advance = FALSE;
    Simple(f, IRP_MJ_SET_INFORMATION, 0, &i, SetupSi, &q);
    T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("setinfo");
    return st;
}
static NTSTATUS SetEof(PFILE_OBJECT f, LONGLONG size)
{
    FILE_END_OF_FILE_INFORMATION e; e.EndOfFile.QuadPart = size;
    return SetInfo(f, FileEndOfFileInformation, &e, sizeof(e), NULL, FALSE);
}
static NTSTATUS SetAlloc(PFILE_OBJECT f, LONGLONG size)
{
    FILE_ALLOCATION_INFORMATION e; e.AllocationSize.QuadPart = size;
    return SetInfo(f, FileAllocationInformation, &e, sizeof(e), NULL, FALSE);
}
static NTSTATUS SetDelete(PFILE_OBJECT f, BOOLEAN del)
{
    FILE_DISPOSITION_INFORMATION d; d.DeleteFile = del;
    return SetInfo(f, FileDispositionInformation, &d, sizeof(d), NULL, FALSE);
}
static NTSTATUS SetBasic(PFILE_OBJECT f, ULONG attrs, LONGLONG ctime, LONGLONG atime, LONGLONG wtime)
{
    FILE_BASIC_INFORMATION b; memset(&b, 0, sizeof(b));
    b.FileAttributes = attrs; b.CreationTime.QuadPart = ctime; b.LastAccessTime.QuadPart = atime; b.LastWriteTime.QuadPart = wtime;
    return SetInfo(f, FileBasicInformation, &b, sizeof(b), NULL, FALSE);
}

/* As the I/O manager does it: a full path opens the target's parent first */
static NTSTATUS Rename(PFILE_OBJECT f, const char *newpath, BOOLEAN replace)
{
    union { FILE_RENAME_INFORMATION r; char b[2400]; } u; PWCHAR w; size_t n; PFILE_OBJECT t = NULL; ULONG_PTR info; NTSTATUS st;
    char p[1300]; size_t k;
    strcpy(p, newpath); for (k = 0; p[k]; k++) if (p[k] == '/') p[k] = '\\';
    memset(&u, 0, sizeof(u));
    w = Utf16(p, &n);
    u.r.ReplaceIfExists = replace; u.r.FileNameLength = (ULONG)(n * 2); memcpy(u.r.FileName, w, n * 2); free(w);
    if (p[0] == '\\') {
        st = OpenEx(p, NULL, FILE_WRITE_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0x4000, 0,
                    SL_OPEN_TARGET_DIRECTORY, &t, &info);
        if (!NT_SUCCESS(st)) return st;
        T(info == FILE_EXISTS || info == FILE_DOES_NOT_EXIST);
    }
    st = SetInfo(f, FileRenameInformation, &u, sizeof(u), t, replace);
    if (t) Close(t);
    return st;
}

static NTSTATUS SetLabel(PFILE_OBJECT f, const char *label)
{
    union { FILE_FS_LABEL_INFORMATION l; char b[256]; } u; PWCHAR w; size_t n; PIRP i; NTSTATUS st;
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f); PIO_STACK_LOCATION sp;
    w = Utf16(label, &n); u.l.VolumeLabelLength = (ULONG)(n * 2); memcpy(u.l.VolumeLabel, w, n * 2); free(w);
    i = IoAllocateIrp(d->StackSize, FALSE); sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_SET_VOLUME_INFORMATION; sp->FileObject = f;
    sp->Parameters.QueryVolume.Length = sizeof(u); sp->Parameters.QueryVolume.FsInformationClass = FileFsLabelInformation;
    i->AssociatedIrp.SystemBuffer = &u;
    IoCallDriver(d, i); T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("label");
    return st;
}

static NTSTATUS Flush(PFILE_OBJECT f)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f); PIRP i = IoAllocateIrp(d->StackSize, FALSE);
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = IRP_MJ_FLUSH_BUFFERS; sp->FileObject = f;
    IoCallDriver(d, i); T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("flush");
    return st;
}

static BOOLEAN FastWrite(PFILE_OBJECT f, LONGLONG off, ULONG len, const void *buf)
{
    IO_STATUS_BLOCK io; LARGE_INTEGER o; BOOLEAN ok;
    o.QuadPart = off;
    ok = w_drv->FastIoDispatch->FastIoWrite(f, &o, len, TRUE, 0, (PVOID)buf, &io, IoGetRelatedDeviceObject(f));
    if (ok) T(io.Status == STATUS_SUCCESS && io.Information == len);
    Balanced("fastwrite");
    return ok;
}

static LONGLONG FileSize(PFILE_OBJECT f)
{
    FILE_STANDARD_INFORMATION si; ULONG_PTR info;
    TS(QueryInfo(f, IRP_MJ_QUERY_INFORMATION, FileStandardInformation, &si, sizeof(si), &info), STATUS_SUCCESS);
    return si.EndOfFile.QuadPart;
}

static ULONGLONG FreeClusters(void)
{
    PFILE_OBJECT f; ULONG_PTR info; FILE_FS_SIZE_INFORMATION s; memset(&s, 0, sizeof(s));
    TS(OPEN_R("\\", &f), STATUS_SUCCESS); if (!f) return 0;
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsSizeInformation, &s, sizeof(s), &info), STATUS_SUCCESS);
    Close(f);
    return (ULONGLONG)s.AvailableAllocationUnits.QuadPart;
}

static ULONG g_cluster;
static int BootDirty(void)
{
    unsigned char b[2];
    fflush(g_img); fseeko(g_img, 106, SEEK_SET);
    if (fread(b, 1, 2, g_img) != 2) return -1;
    return (b[0] & 2) != 0;
}

/* Everything closed: dismount the way format does, then let it all go */
static void Remount(void)
{
    PFILE_OBJECT v;
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (!v) return;
    TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);
    T(BootDirty() == 0);                         /* locking leaves it clean */
    TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
    TS(Fsctl(v, FSCTL_UNLOCK_VOLUME), STATUS_SUCCESS);
    Close(v);
    LazyWriteAll(); RunWorkers();
    if (g_devices != w_base_dev) { fprintf(stderr, "volume still there after dismount: %d devices, %d sections\n", g_devices, CacheSections()); g_errors++; }
    if (g_pool != w_base_pool) { fprintf(stderr, "pool leak after dismount: %d blocks\n", g_pool - w_base_pool); g_errors++; }
    T(g_fo == 0); T(g_lost_dirty == 0); T(CacheDirtyPages() == 0);
    T(g_disk->Vpb->ReferenceCount == 0);
}

/* ---------------- model ---------------- */

typedef struct MNODE {
    char name[1100]; int dir; ULONG attr;
    unsigned char *data; size_t size, cap;
    int opens, delpending;
    LONGLONG ctime, wtime;                      /* times set explicitly, 0 if not */
    struct MNODE *parent, *child, *next;
} MNODE;
static MNODE g_root = { "", 1, 0x10 };

static void MPath(MNODE *n, char *out)
{
    if (!n->parent) { strcpy(out, ""); return; }
    MPath(n->parent, out); strcat(out, "/"); strcat(out, n->name);
}
static int NameEq(const char *a, const char *b)
{
    /* ASCII folds; the other names the tests use differ in more than case */
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x < 0x80 && y < 0x80) { if (towupper(x) != towupper(y)) return 0; }
        else if (x != y) return 0;
    }
    return *a == *b;
}
static MNODE *MFind(MNODE *d, const char *name) { MNODE *c; for (c = d->child; c; c = c->next) if (NameEq(c->name, name)) return c; return NULL; }
static MNODE *MAdd(MNODE *d, const char *name, int dir)
{
    MNODE *n = calloc(1, sizeof(MNODE));
    strcpy(n->name, name); n->dir = dir; n->attr = dir ? 0x10 : 0x20; n->parent = d;
    n->next = d->child; d->child = n;
    return n;
}
static void MUnlink(MNODE *n)
{
    MNODE **pp = &n->parent->child; while (*pp != n) pp = &(*pp)->next; *pp = n->next;
}
static void MFree(MNODE *n) { while (n->child) { MNODE *c = n->child; MUnlink(c); MFree(c); } free(n->data); free(n); }
static void MResize(MNODE *n, size_t size)
{
    if (size > n->cap) { n->cap = size + size / 2 + 16; n->data = realloc(n->data, n->cap); }
    if (size > n->size) memset(n->data + n->size, 0, size - n->size);
    n->size = size;
}
static void MWrite(MNODE *n, size_t off, const void *buf, size_t len)
{
    if (off + len > n->size) MResize(n, off + len);
    memcpy(n->data + off, buf, len);
}
static int MCount(MNODE *d, int dirs) { int k = 0; MNODE *c; for (c = d->child; c; c = c->next) { if (c->dir == dirs || dirs < 0) k++; if (c->dir) k += MCount(c, dirs); } return k; }
static int MOpenBelow(MNODE *d) { int k = 0; MNODE *c; for (c = d->child; c; c = c->next) { k += c->opens; if (c->dir) k += MOpenBelow(c); } return k; }

static unsigned char *g_rbuf;
static void RandBytes(unsigned char *p, size_t n) { size_t i; unsigned x = Rand(); for (i = 0; i < n; i++) { if ((i & 3) == 0) x = Rand() ^ (x << 7); p[i] = (unsigned char)(x >> ((i & 3) * 8)); } }

/* The driver's view of one directory against the model */
static void VerifyDir(MNODE *d)
{
    char path[1300]; PFILE_OBJECT f; NTSTATUS st; ULONG_PTR info; unsigned char *buf = malloc(8192);
    int first = 1, seen = 0, want = 0; MNODE *c;
    MPath(d, path);
    for (c = d->child; c; c = c->next) want++;
    st = Open(path[0] ? path : "\\", NULL, FILE_LIST_DIRECTORY_R, 7, FILE_OPEN, FILE_DIRECTORY_FILE, &f);
    TS(st, STATUS_SUCCESS); if (!f) { free(buf); return; }
    for (;;) {
        ULONG pos = 0;
        st = QueryDir(f, FileBothDirectoryInformation, buf, 8192, first ? "*" : NULL, 0, &info);
        first = 0;
        if (st == STATUS_NO_MORE_FILES || st == STATUS_NO_SUCH_FILE) break;
        TS(st, STATUS_SUCCESS); if (!NT_SUCCESS(st)) break;
        for (;;) {
            PFILE_BOTH_DIR_INFORMATION e = (PFILE_BOTH_DIR_INFORMATION)(buf + pos); char name[1100];
            ToUtf8(e->FileName, e->FileNameLength / 2, name);
            if (strcmp(name, ".") && strcmp(name, "..")) {
                c = MFind(d, name);
                seen++;
                if (!c || strcmp(c->name, name)) { fprintf(stderr, "listing of '%s': unexpected '%s'\n", path, name); g_errors++; }
                else if (!c->opens) {         /* open files get their entry at cleanup */
                    ULONG want_attr = c->attr ? c->attr : FILE_ATTRIBUTE_NORMAL;
                    if (!!(e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != c->dir) { fprintf(stderr, "'%s/%s' dir flag\n", path, name); g_errors++; }
                    if (e->FileAttributes != want_attr) { fprintf(stderr, "'%s/%s' attributes %x, want %x\n", path, name, (unsigned)e->FileAttributes, (unsigned)want_attr); g_errors++; }
                    if (!c->dir && (ULONGLONG)e->EndOfFile.QuadPart != c->size) { fprintf(stderr, "'%s/%s' listed size %lld, want %zu\n", path, name, (long long)e->EndOfFile.QuadPart, c->size); g_errors++; }
                    if (c->wtime && llabs(e->LastWriteTime.QuadPart - c->wtime) > 100000) { fprintf(stderr, "'%s/%s' write time\n", path, name); g_errors++; }
                    if (c->ctime && llabs(e->CreationTime.QuadPart - c->ctime) > 100000) { fprintf(stderr, "'%s/%s' creation time\n", path, name); g_errors++; }
                }
            }
            if (!e->NextEntryOffset) break;
            pos += e->NextEntryOffset;
        }
    }
    if (seen != want) { fprintf(stderr, "listing of '%s': %d entries, want %d\n", path, seen, want); g_errors++; }
    Close(f);
    free(buf);
}

static void VerifyFile(MNODE *n, int nocache)
{
    char path[1300]; PFILE_OBJECT f; NTSTATUS st; ULONG_PTR info; unsigned char *buf; size_t done = 0; LONGLONG sz;
    MPath(n, path);
    st = Open(path, NULL, FILE_GENERIC_READ, 7, FILE_OPEN, nocache ? 0x8 : 0, &f);
    TS(st, STATUS_SUCCESS); if (!f) return;
    sz = FileSize(f);
    if ((size_t)sz != n->size) { fprintf(stderr, "'%s' size %lld, want %zu\n", path, (long long)sz, n->size); g_errors++; }
    buf = malloc(n->size + 2 * 65536);
    while (done < n->size) {
        ULONG chunk = nocache ? g_sector * (1 + Rand() % 64) : 1 + Rand() % 100000;
        st = Read(f, (LONGLONG)done, chunk, buf + done, (BOOLEAN)nocache, &info);
        TS(st, STATUS_SUCCESS); if (!NT_SUCCESS(st) || info == 0) break;
        done += info;
    }
    if (done != n->size || (n->size && memcmp(buf, n->data, n->size))) {
        size_t k = 0; while (k < n->size && k < done && buf[k] == n->data[k]) k++;
        fprintf(stderr, "'%s' %s content differs at %zu of %zu (read %zu)\n", path, nocache ? "noncached" : "cached", k, n->size, done); g_errors++;
    }
    TS(Read(f, (LONGLONG)n->size + (nocache ? (g_sector - n->size % g_sector) % g_sector : 0), nocache ? g_sector : 1, buf, (BOOLEAN)nocache, &info), STATUS_END_OF_FILE);
    Close(f);
    free(buf);
}

static void VerifyTree(MNODE *d, int deep)
{
    MNODE *c;
    VerifyDir(d);
    for (c = d->child; c; c = c->next) {
        if (c->dir) VerifyTree(c, deep);
        else if (c->delpending) continue;                   /* cannot be opened any more */
        else if (deep || Rand() % 4 == 0) VerifyFile(c, Rand() % 3 == 0);
    }
}

static void Manifest(FILE *out, MNODE *d)
{
    MNODE *c; char path[1300];
    for (c = d->child; c; c = c->next) {
        MPath(c, path);
        if (c->dir) { fprintf(out, "D %s\n", path); Manifest(out, c); }
        else fprintf(out, "F %s size=%zu hash=%016llx attr=%x\n", path, c->size, Fnv(c->data, c->size), (unsigned)c->attr);
    }
}

/* ---------------- scripted cases ---------------- */

#define W_CREATE(p, a, sh, disp, opt, attr, f, info) OpenEx((p), NULL, (a), (sh), (disp), (opt), (attr), 0, (f), (info))
#define RW_ACCESS (FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | DELETE)

static MNODE *MNew(const char *path, int dir)
{
    char tmp[1300], *slash; MNODE *d = &g_root, *n;
    strcpy(tmp, path + 1);
    while ((slash = strchr(tmp, '/')) != NULL) {
        *slash = 0; d = MFind(d, tmp); if (!d) { fprintf(stderr, "model: no dir for %s\n", path); abort(); }
        memmove(tmp, slash + 1, strlen(slash + 1) + 1);
    }
    n = MFind(d, tmp);
    return n ? n : MAdd(d, tmp, dir);
}
static MNODE *MGet(const char *path) { MNODE *n = MNew(path, 0); return n; }

static void PutFile(const char *path, size_t size)
{
    PFILE_OBJECT f; ULONG_PTR info; MNODE *n;
    TS(W_CREATE(path, RW_ACCESS, 7, FILE_OVERWRITE_IF, 0, 0, &f, &info), STATUS_SUCCESS); if (!f) return;
    n = MGet(path); MResize(n, 0);
    if (size) {
        RandBytes(g_rbuf, size); TS(Write(f, 0, (ULONG)size, g_rbuf, FALSE, &info), STATUS_SUCCESS); T(info == size);
        MWrite(n, 0, g_rbuf, size);
    }
    n->attr = 0x20;
    Close(f);
}

static void Mkdir(const char *path)
{
    PFILE_OBJECT f; ULONG_PTR info;
    TS(W_CREATE(path, FILE_LIST_DIRECTORY_R, 7, FILE_CREATE, FILE_DIRECTORY_FILE, 0, &f, &info), STATUS_SUCCESS);
    if (f) { T(info == FILE_CREATED); Close(f); MNew(path, 1); }
    else fprintf(stderr, "  mkdir %s\n", path);
}

static void RemoveNode(const char *path)
{
    PFILE_OBJECT f; MNODE *n = MGet(path);
    TS(Open(path, NULL, DELETE | FILE_READ_ATTRIBUTES, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS); if (!f) return;
    TS(SetDelete(f, TRUE), STATUS_SUCCESS);
    Close(f);
    MUnlink(n); MFree(n);
}

static void Scripted(void)
{
    PFILE_OBJECT f, g, h; ULONG_PTR info; NTSTATUS st; int k; char p[300]; unsigned char *buf = malloc(4 << 20);
    ULONGLONG free0, free1; LONGLONG big;

    /* create, write, read back through the same and a new handle */
    T(BootDirty() == 0);
    TS(W_CREATE("\\hello.txt", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS); T(info == FILE_CREATED);
    T(BootDirty() == 1);                             /* dirty before the first change */
    TS(Write(f, 0, 6, "hello\n", FALSE, &info), STATUS_SUCCESS); T(info == 6);
    TS(Read(f, 0, 100, buf, FALSE, &info), STATUS_SUCCESS); T(info == 6 && !memcmp(buf, "hello\n", 6));
    Close(f);
    MWrite(MNew("/hello.txt", 0), 0, "hello\n", 6);
    TS(W_CREATE("\\HELLO.TXT", FILE_GENERIC_READ, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_OBJECT_NAME_COLLISION);
    TS(W_CREATE("\\HELLO.TXT", FILE_GENERIC_READ, 7, FILE_OPEN_IF, 0, 0, &f, &info), STATUS_SUCCESS); T(info == FILE_OPENED);
    if (f) { T(FileSize(f) == 6); Close(f); }

    /* directories, Unicode names, a deep path */
    Mkdir("/dir"); Mkdir("/dir/sub"); Mkdir("/dir/sub/deeper");
    Mkdir("/Türkçe klasör"); PutFile("/Türkçe klasör/şğüöçİı.txt", 1234);
    Mkdir("/Ελληνικά"); PutFile("/Ελληνικά/αρχείο.bin", 70000);
    PutFile("/dir/sub/deeper/leaf.dat", 5);
    TS(W_CREATE("\\dir", FILE_LIST_DIRECTORY_R, 7, FILE_CREATE, FILE_DIRECTORY_FILE, 0, &f, &info), STATUS_OBJECT_NAME_COLLISION);
    TS(W_CREATE("\\dir\\x\\y", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_OBJECT_PATH_NOT_FOUND);
    TS(W_CREATE("\\bad:name", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_OBJECT_NAME_INVALID);
    TS(W_CREATE("\\newdir\\", FILE_LIST_DIRECTORY_R, 7, FILE_CREATE, FILE_DIRECTORY_FILE, 0, &f, &info), STATUS_SUCCESS);
    if (f) { Close(f); MNew("/newdir", 1); }
    TS(W_CREATE("\\file\\", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_OBJECT_NAME_INVALID);
    TS(W_CREATE("\\dir", RW_ACCESS, 7, FILE_OVERWRITE_IF, 0, 0, &f, &info), STATUS_OBJECT_NAME_COLLISION);
    TS(W_CREATE("\\hello.txt", RW_ACCESS, 7, FILE_CREATE, FILE_DIRECTORY_FILE, 0, &f, &info), STATUS_OBJECT_NAME_COLLISION);
    TS(W_CREATE("\\" "L" "ong" "\\x", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_OBJECT_PATH_NOT_FOUND);
    {
        char longname[300]; memset(longname, 'n', 256); longname[0] = '\\'; longname[256] = 0;
        TS(W_CREATE(longname, RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS); if (f) { Close(f); MNew(longname, 0); }
        longname[256] = 'n'; longname[257] = 0;
        TS(W_CREATE(longname, RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_OBJECT_NAME_INVALID);
    }

    /* many entries: the directory grows over several clusters */
    Mkdir("/many");
    for (k = 0; k < 400; k++) { snprintf(p, sizeof(p), "/many/entry number %04d with a name long enough for several name entries.txt", k); PutFile(p, k % 7 == 0 ? (size_t)(k * 13) : 0); }
    for (k = 0; k < 400; k += 2) { snprintf(p, sizeof(p), "/many/entry number %04d with a name long enough for several name entries.txt", k); RemoveNode(p); }
    for (k = 0; k < 150; k++) { snprintf(p, sizeof(p), "/many/new %d", k); PutFile(p, 3); }   /* reuses the freed slots */
    VerifyDir(MGet("/many"));

    /* a big file in pieces, then two files growing in turn (FAT chains) */
    TS(W_CREATE("\\big.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS);
    TS(W_CREATE("\\frag1.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &g, &info), STATUS_SUCCESS);
    TS(W_CREATE("\\frag2.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &h, &info), STATUS_SUCCESS);
    if (f && g && h) {
        MNODE *nb = MNew("/big.bin", 0), *n1 = MNew("/frag1.bin", 0), *n2 = MNew("/frag2.bin", 0);
        for (k = 0; k < 48; k++) {
            RandBytes(g_rbuf, 65536); TS(Write(f, (LONGLONG)k * 65536, 65536, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(nb, (size_t)k * 65536, g_rbuf, 65536);
            RandBytes(g_rbuf, g_cluster + 100); TS(Write(g, (LONGLONG)n1->size, g_cluster + 100, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n1, n1->size, g_rbuf, g_cluster + 100);
            RandBytes(g_rbuf, 777); TS(Write(h, -1, 777, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n2, n2->size, g_rbuf, 777);   /* to end of file */
            if (k % 16 == 5) LazyWriteAll();
        }
        Close(f); Close(g); Close(h);
    }

    /* sparse growth: set EOF far out, write past VDL, cut back, grow again */
    TS(W_CREATE("\\sparse.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        MNODE *n = MNew("/sparse.bin", 0);
        RandBytes(g_rbuf, 1000); TS(Write(f, 0, 1000, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 0, g_rbuf, 1000);
        TS(SetEof(f, 3 * 1024 * 1024 + 17), STATUS_SUCCESS); MResize(n, 3 * 1024 * 1024 + 17);
        TS(Read(f, 500000, 4096, buf, FALSE, &info), STATUS_SUCCESS); for (k = 0; k < 4096; k++) if (buf[k]) { T(!"zero past VDL"); break; }
        RandBytes(g_rbuf, 5000); TS(Write(f, 2000000, 5000, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 2000000, g_rbuf, 5000);
        TS(SetEof(f, 1500), STATUS_SUCCESS); MResize(n, 1500);
        TS(SetEof(f, 9000), STATUS_SUCCESS); MResize(n, 9000);           /* the old bytes past 1500 must not come back */
        RandBytes(g_rbuf, 10); TS(Write(f, 8000, 10, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 8000, g_rbuf, 10);
        Close(f);
    }
    MmTrimAll(); LazyWriteAll();
    VerifyFile(MGet("/sparse.bin"), 0); VerifyFile(MGet("/sparse.bin"), 1);

    /* non-cached writes, past VDL too, with a cached handle open */
    TS(W_CREATE("\\nc.bin", RW_ACCESS, 7, FILE_CREATE, 0x8, 0, &f, &info), STATUS_SUCCESS);
    TS(W_CREATE("\\nc.bin", RW_ACCESS, 7, FILE_OPEN, 0, 0, &g, &info), STATUS_SUCCESS);
    if (f && g) {
        MNODE *n = MNew("/nc.bin", 0);
        RandBytes(g_rbuf, 3 * g_sector); TS(Write(f, 0, 3 * g_sector, g_rbuf, TRUE, &info), STATUS_SUCCESS); MWrite(n, 0, g_rbuf, 3 * g_sector);
        TS(Write(f, 1, g_sector, g_rbuf, TRUE, &info), STATUS_INVALID_PARAMETER);
        TS(Read(g, 0, 10, buf, FALSE, &info), STATUS_SUCCESS); T(!memcmp(buf, n->data, 10));        /* g caches it now */
        RandBytes(g_rbuf, 100); TS(Write(g, 5, 100, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 5, g_rbuf, 100);
        RandBytes(g_rbuf, 2 * g_sector); TS(Write(f, 10 * g_sector, 2 * g_sector, g_rbuf, TRUE, &info), STATUS_SUCCESS); MWrite(n, 10 * g_sector, g_rbuf, 2 * g_sector);
        TS(Read(g, 0, (ULONG)n->size, buf, FALSE, &info), STATUS_SUCCESS); T(info == n->size && !memcmp(buf, n->data, n->size));
        TS(Read(f, 0, (ULONG)n->size, buf, TRUE, &info), STATUS_SUCCESS); T(info == n->size && !memcmp(buf, n->data, n->size));
        Close(f); Close(g);
    }

    /* fast I/O writes inside the allocation, and a write-through handle */
    TS(W_CREATE("\\fast.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        MNODE *n = MNew("/fast.bin", 0);
        RandBytes(g_rbuf, 10); TS(Write(f, 0, 10, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 0, g_rbuf, 10);
        k = g_fastio_writes;
        RandBytes(g_rbuf, 20); if (FastWrite(f, 10, 20, g_rbuf)) MWrite(n, 10, g_rbuf, 20); else T(!"fast write");
        RandBytes(g_rbuf, 5); if (FastWrite(f, 100, 5, g_rbuf)) MWrite(n, 100, g_rbuf, 5); else T(!"fast write");   /* past VDL */
        T(!FastWrite(f, (LONGLONG)g_cluster * 3, 5, g_rbuf));   /* past the allocation: IRP path */
        T(g_fastio_writes == k + 2);
        Close(f);
    }
    TS(W_CREATE("\\wt.bin", RW_ACCESS, 7, FILE_CREATE, 0x2, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        MNODE *n = MNew("/wt.bin", 0); int w0 = g_disk_writes;
        RandBytes(g_rbuf, 3000); TS(Write(f, 0, 3000, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 0, g_rbuf, 3000);
        T(g_disk_writes > w0);                             /* written through at once */
        Close(f);
    }

    /* overwrite and supersede */
    PutFile("/ow.txt", 5000);
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 7, FILE_OVERWRITE, 0, FILE_ATTRIBUTE_HIDDEN, &f, &info), STATUS_SUCCESS);
    if (f) { T(info == FILE_OVERWRITTEN); T(FileSize(f) == 0); Close(f); MResize(MGet("/ow.txt"), 0); MGet("/ow.txt")->attr = 0x22; }
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 7, FILE_OVERWRITE_IF, 0, 0, &f, &info), STATUS_ACCESS_DENIED);   /* keeps hidden */
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 7, FILE_SUPERSEDE, 0, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, &f, &info), STATUS_SUCCESS);
    if (f) { T(info == FILE_SUPERSEDED); RandBytes(g_rbuf, 50); TS(Write(f, 0, 50, g_rbuf, FALSE, &info), STATUS_SUCCESS); Close(f); MWrite(MGet("/ow.txt"), 0, g_rbuf, 50); MGet("/ow.txt")->attr = 0x26; }
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 7, FILE_OVERWRITE, 0, 0, &f, &info), STATUS_ACCESS_DENIED);
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 7, FILE_OVERWRITE_IF, 0, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, &f, &info), STATUS_SUCCESS);
    if (f) { T(info == FILE_OVERWRITTEN); Close(f); MResize(MGet("/ow.txt"), 0); }
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 0, FILE_OPEN, 0, 0, &f, &info), STATUS_SUCCESS);
    TS(W_CREATE("\\ow.txt", RW_ACCESS, 7, FILE_OVERWRITE, 0, 0x6, &g, &info), STATUS_SHARING_VIOLATION);
    if (f) Close(f);

    /* attributes and times */
    PutFile("/attr.txt", 10);
    TS(Open("\\attr.txt", NULL, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) {
        LONGLONG ct = 130000000000000000LL + 12340000LL, wt = 131000000000000000LL + 5550000LL;
        TS(SetBasic(f, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN, ct, 0, wt), STATUS_SUCCESS);
        TS(SetBasic(f, FILE_ATTRIBUTE_DIRECTORY, 0, 0, 0), STATUS_INVALID_PARAMETER);
        Close(f); MGet("/attr.txt")->attr = 0x3; MGet("/attr.txt")->ctime = ct; MGet("/attr.txt")->wtime = wt;
    }
    TS(Open("\\attr.txt", NULL, FILE_WRITE_DATA, 7, FILE_OPEN, 0, &f), STATUS_ACCESS_DENIED);       /* read-only */
    TS(Open("\\attr.txt", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) { TS(SetDelete(f, TRUE), STATUS_CANNOT_DELETE); Close(f); }
    TS(Open("\\attr.txt", NULL, MAXIMUM_ALLOWED, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) { T(!f->WriteAccess && f->ReadAccess); Close(f); }
    TS(Open("\\attr.txt", NULL, FILE_WRITE_ATTRIBUTES, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) { TS(SetBasic(f, FILE_ATTRIBUTE_NORMAL, 0, 0, 0), STATUS_SUCCESS); Close(f); MGet("/attr.txt")->attr = 0; }
    TS(Open("\\dir", NULL, FILE_WRITE_ATTRIBUTES, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) { TS(SetBasic(f, FILE_ATTRIBUTE_HIDDEN, 0, 0, 0), STATUS_SUCCESS); Close(f); MGet("/dir")->attr = 0x12; }

    /* renames */
    PutFile("/r1.txt", 100);
    TS(Open("\\r1.txt", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) {
        MNODE *n = MGet("/r1.txt");
        TS(Rename(f, "R1-renamed.TXT", FALSE), STATUS_SUCCESS); strcpy(n->name, "R1-renamed.TXT");
        TS(Rename(f, "r1-RENAMED.txt", FALSE), STATUS_SUCCESS); strcpy(n->name, "r1-RENAMED.txt");   /* case only */
        TS(Rename(f, "hello.txt", FALSE), STATUS_OBJECT_NAME_COLLISION);
        TS(Rename(f, "\\dir\\sub\\moved.txt", FALSE), STATUS_SUCCESS);
        MUnlink(n); n->next = NULL; { MNODE *d = MGet("/dir/sub"); n->parent = d; n->next = d->child; d->child = n; } strcpy(n->name, "moved.txt");
        TS(Rename(f, "\\hello.txt", FALSE), STATUS_OBJECT_NAME_COLLISION);
        TS(Rename(f, "\\hello.txt", TRUE), STATUS_SUCCESS);                                        /* replaces */
        { MNODE *old = MGet("/hello.txt"); MUnlink(old); MFree(old); }
        MUnlink(n); n->parent = &g_root; n->next = g_root.child; g_root.child = n; strcpy(n->name, "hello.txt");
        TS(Rename(f, "bad*name", FALSE), STATUS_OBJECT_NAME_INVALID);
        Close(f);
    }
    TS(Open("\\dir\\sub", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) {
        TS(Rename(f, "\\dir\\sub\\deeper\\sub2", FALSE), STATUS_INVALID_PARAMETER);                 /* into itself */
        TS(Open("\\dir\\sub\\deeper\\leaf.dat", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, &g), STATUS_SUCCESS);
        TS(Rename(f, "sub-open", FALSE), STATUS_ACCESS_DENIED);                                     /* a child is open */
        if (g) Close(g);
        TS(Rename(f, "\\moved sub", FALSE), STATUS_SUCCESS);
        { MNODE *n = MGet("/dir/sub"); MUnlink(n); n->parent = &g_root; n->next = g_root.child; g_root.child = n; strcpy(n->name, "moved sub"); }
        Close(f);
    }
    TS(OPEN_R("\\moved sub\\deeper\\leaf.dat", &f), STATUS_SUCCESS); if (f) Close(f);
    TS(OPEN_R("\\dir\\sub\\deeper\\leaf.dat", &f), STATUS_OBJECT_PATH_NOT_FOUND);

    /* deletes */
    TS(Open("\\moved sub", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) { TS(SetDelete(f, TRUE), STATUS_DIRECTORY_NOT_EMPTY); Close(f); }
    PutFile("/del1.txt", 3000);
    TS(Open("\\del1.txt", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, &g), STATUS_SUCCESS);
    TS(Open("\\del1.txt", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f && g) {
        TS(SetDelete(f, TRUE), STATUS_SUCCESS);
        Close(f);
        TS(OPEN_R("\\del1.txt", &h), STATUS_DELETE_PENDING);
        TS(Read(g, 0, 10, buf, FALSE, &info), STATUS_SUCCESS);           /* still readable */
        Close(g);                                                          /* now gone */
        TS(OPEN_R("\\del1.txt", &h), STATUS_OBJECT_NAME_NOT_FOUND);
        { MNODE *n = MGet("/del1.txt"); MUnlink(n); MFree(n); }
    }
    PutFile("/del2.txt", 100);
    TS(Open("\\del2.txt", NULL, DELETE | FILE_GENERIC_READ, 7, FILE_OPEN, FILE_DELETE_ON_CLOSE, &f), STATUS_SUCCESS);
    if (f) { Close(f); TS(OPEN_R("\\del2.txt", &h), STATUS_OBJECT_NAME_NOT_FOUND); { MNODE *n = MGet("/del2.txt"); MUnlink(n); MFree(n); } }
    TS(Open("\\newdir", NULL, DELETE, 7, FILE_OPEN, FILE_DELETE_ON_CLOSE | FILE_DIRECTORY_FILE, &f), STATUS_SUCCESS);
    if (f) { Close(f); { MNODE *n = MGet("/newdir"); MUnlink(n); MFree(n); } }
    TS(Open("\\", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    if (f) { TS(SetDelete(f, TRUE), STATUS_CANNOT_DELETE); Close(f); }
    /* recreate a name while the old file is still held by the cache */
    PutFile("/recycled.txt", 20000); VerifyFile(MGet("/recycled.txt"), 0);
    RemoveNode("/recycled.txt"); PutFile("/recycled.txt", 7); VerifyFile(MGet("/recycled.txt"), 0);

    /* allocation */
    TS(W_CREATE("\\alloc.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        MNODE *n = MNew("/alloc.bin", 0);
        free0 = FreeClusters();
        TS(SetAlloc(f, 10 * (LONGLONG)g_cluster), STATUS_SUCCESS);
        T(FreeClusters() == free0 - 10);
        RandBytes(g_rbuf, 100); TS(Write(f, 0, 100, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 0, g_rbuf, 100);
        TS(SetAlloc(f, 50), STATUS_SUCCESS); MResize(n, 50);
        T(FreeClusters() == free0 - 1);
        TS(SetAlloc(f, 4 * (LONGLONG)g_cluster), STATUS_SUCCESS);
        Close(f);
        T(FreeClusters() == free0 - 1);                     /* the extra goes at cleanup */
    }

    /* label */
    TS(OPEN_R("\\", &f), STATUS_SUCCESS);
    if (f) {
        union { FILE_FS_VOLUME_INFORMATION v; char b[512]; } vi; char l[64];
        TS(SetLabel(f, "NEW LABEL"), STATUS_SUCCESS);
        TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsVolumeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
        ToUtf8(vi.v.VolumeLabel, vi.v.VolumeLabelLength / 2, l); T(!strcmp(l, "NEW LABEL"));
        TS(SetLabel(f, "twelve chars"), STATUS_INVALID_VOLUME_LABEL);
        TS(SetLabel(f, "a*b"), STATUS_INVALID_VOLUME_LABEL);
        TS(SetLabel(f, ""), STATUS_SUCCESS);
        TS(SetLabel(f, "Yazı"), STATUS_SUCCESS);
        Close(f);
    }

    /* flush leaves the volume clean on disk while it stays mounted */
    TS(OPEN_R("\\", &f), STATUS_SUCCESS);
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &g), STATUS_SUCCESS);
    if (f && g) { T(BootDirty() == 1); TS(Flush(g), STATUS_SUCCESS); T(BootDirty() == 0); T(CacheDirtyPages() == 0); Close(g); Close(f); }

    /* disk full */
    free0 = FreeClusters();
    TS(W_CREATE("\\full.bin", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        big = (LONGLONG)(free0 + 3) * g_cluster;
        TS(SetEof(f, big), STATUS_DISK_FULL);
        T(FileSize(f) == 0 && FreeClusters() == free0);
        big = (LONGLONG)(free0 - 2) * g_cluster;
        TS(SetEof(f, big), STATUS_SUCCESS);
        T(FreeClusters() == 2);
        RandBytes(g_rbuf, 4 * g_cluster > 65536 ? 65536 : 4 * g_cluster);
        st = Write(f, big + 10, 3 * g_cluster, g_rbuf, FALSE, &info);
        TS(st, STATUS_DISK_FULL);
        T(FileSize(f) == big);
        Close(f);
        RemoveNode("/full.bin");
        T(FreeClusters() == free0);
    }

    /* a remount reads it all back from the disk */
    Remount();
    VerifyTree(&g_root, 1);
    {
        PFILE_OBJECT r; union { FILE_FS_VOLUME_INFORMATION v; char b[512]; } vi; char l[64];
        TS(OPEN_R("\\", &r), STATUS_SUCCESS);
        if (r) { TS(QueryInfo(r, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsVolumeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
                 ToUtf8(vi.v.VolumeLabel, vi.v.VolumeLabelLength / 2, l); T(!strcmp(l, "Yazı")); Close(r); }
    }

    /* shutdown leaves it clean too */
    PutFile("/before-shutdown.txt", 123);
    T(BootDirty() == 1);
    {
        PIRP i = IoAllocateIrp(1, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
        T(g_shutdown_dev != NULL);
        sp->MajorFunction = IRP_MJ_SHUTDOWN; IoCallDriver(g_shutdown_dev, i); T(i->Completed); TS(i->IoStatus.Status, STATUS_SUCCESS); IoFreeIrp(i);
        Balanced("shutdown");
    }
    T(BootDirty() == 0);
    Remount();
    free(buf);
    (void)free1;
}

/* ---------------- media checks, removal, the dirty flag ---------------- */

static NTSTATUS FsctlOut(PFILE_OBJECT f, ULONG code, ULONG *out)
{
    PIRP i; NTSTATUS st; struct fc c; c.code = code; c.out = out; c.outlen = 4; *out = 0xDEAD;
    Simple(f, IRP_MJ_FILE_SYSTEM_CONTROL, IRP_MN_USER_FS_REQUEST, &i, SetupFc, &c);
    T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("fsctl");
    return st;
}

static int VolumeDirty(void)
{
    PFILE_OBJECT r; ULONG d = 0xDEAD;
    TS(OPEN_R("\\", &r), STATUS_SUCCESS); if (!r) return -1;
    TS(FsctlOut(r, FSCTL_IS_VOLUME_DIRTY, &d), STATUS_SUCCESS);
    Close(r);
    return (int)d;
}

/* What chkdsk would do last: clear VolumeDirty on the unmounted image */
static void ClearDirtyOnDisk(void)
{
    unsigned char b[2];
    T(g_disk->Vpb->DeviceObject == NULL);
    fflush(g_img); fseeko(g_img, 106, SEEK_SET); T(fread(b, 1, 2, g_img) == 2);
    b[0] &= ~2; fseeko(g_img, 106, SEEK_SET); fwrite(b, 1, 2, g_img); fflush(g_img);
}

/* The volume goes away with everything closed, so the next open mounts it again */
static void Unmount(void)
{
    PFILE_OBJECT v;
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (!v) return;
    TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
    Close(v);
    LazyWriteAll(); RunWorkers();
    T(g_devices == w_base_dev);
}

static void Robustness(void)
{
    PFILE_OBJECT f, v; ULONG_PTR info; MNODE *n; ULONG out; int k, before;

    /* a media check with dirty data in the cache: same volume, nothing lost */
    PutFile("/verify.bin", 3 * 65536 + 17);
    TS(W_CREATE("/verify.bin", RW_ACCESS, 7, FILE_OPEN, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        n = MGet("/verify.bin");
        RandBytes(g_rbuf, 70000); TS(Write(f, 5000, 70000, g_rbuf, FALSE, &info), STATUS_SUCCESS); MWrite(n, 5000, g_rbuf, 70000);
        T(CacheDirtyPages() > 0);
        g_disk->Flags |= DO_VERIFY_VOLUME;
        LazyWriteAll();                          /* the writes are refused until the volume is checked */
        T(CacheDirtyPages() > 0);
        RandBytes(g_rbuf, 4096); TS(Write(f, 147456, 4096, g_rbuf, TRUE, &info), STATUS_SUCCESS); MWrite(n, 147456, g_rbuf, 4096);
        T(!(g_disk->Flags & DO_VERIFY_VOLUME));
        Close(f);
    }
    LazyWriteAll();
    T(CacheDirtyPages() == 0);
    VerifyFile(MGet("/verify.bin"), 1);
    Remount();
    VerifyFile(MGet("/verify.bin"), 1);

    /* a media check that finds another volume: the old one is gone, its dirty data with it */
    PutFile("/swapped.bin", 20000);
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (v) { TS(Flush(v), STATUS_SUCCESS); Close(v); }
    TS(W_CREATE("/swapped.bin", RW_ACCESS, 7, FILE_OPEN, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        unsigned char save[4]; PVPB old = g_disk->Vpb;
        RandBytes(g_rbuf, 8000); TS(Write(f, 100, 8000, g_rbuf, FALSE, &info), STATUS_SUCCESS);   /* lost */
        /* another serial number in the boot sector makes it another medium */
        fflush(g_img); fseeko(g_img, 100, SEEK_SET); T(fread(save, 1, 4, g_img) == 4);
        fseeko(g_img, 100, SEEK_SET); fwrite("\x11\x22\x33\x44", 1, 4, g_img); fflush(g_img);
        g_disk->Flags |= DO_VERIFY_VOLUME;
        g_raw_reparse = 1;
        TS(OPEN_R("\\", &v), STATUS_REPARSE);
        g_raw_reparse = 0;
        T(g_disk->Vpb != old);
        TS(Write(f, 0, 10, g_rbuf, FALSE, &info), EXF_STATUS_DISMOUNTED_T);
        /* back to the original medium before the next mount */
        fseeko(g_img, 100, SEEK_SET); fwrite(save, 1, 4, g_img); fflush(g_img);
        Close(f);
        LazyWriteAll(); RunWorkers();
        T(g_devices == w_base_dev);
        g_lost_dirty = 0;
        if (VolumeDirty() == 1) { Unmount(); ClearDirtyOnDisk(); }
        VerifyFile(MGet("/swapped.bin"), 1);
    }

    /* surprise removal with dirty data: handles fail, the data is lost, the disk stays consistent */
    PutFile("/gone.bin", 50000);
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (v) { TS(Flush(v), STATUS_SUCCESS); Close(v); }
    T(BootDirty() == 0);
    TS(W_CREATE("/gone.bin", RW_ACCESS, 7, FILE_OPEN, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) {
        RandBytes(g_rbuf, 30000); TS(Write(f, 0, 30000, g_rbuf, FALSE, &info), STATUS_SUCCESS);
        T(CacheDirtyPages() > 0);
        TS(Pnp(IRP_MN_SURPRISE_REMOVAL), STATUS_SUCCESS);
        TS(Write(f, 0, 10, g_rbuf, FALSE, &info), EXF_STATUS_DISMOUNTED_T);
        TS(Read(f, 0, 10, g_rbuf, FALSE, &info), EXF_STATUS_DISMOUNTED_T);
        Close(f);
        LazyWriteAll(); RunWorkers();
        T(g_devices == w_base_dev); T(g_disk->Vpb->ReferenceCount == 0);
        T(CacheDirtyPages() == 0);
        g_lost_dirty = 0;
        if (VolumeDirty() == 1) { Unmount(); ClearDirtyOnDisk(); }
        VerifyFile(MGet("/gone.bin"), 1);
    }

    /* FSCTL_MARK_VOLUME_DIRTY sticks: neither a lock nor a remount clears it */
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (v) {
        TS(FsctlOut(v, FSCTL_IS_VOLUME_DIRTY, &out), STATUS_SUCCESS); T(out == 0);
        TS(Fsctl(v, FSCTL_MARK_VOLUME_DIRTY), STATUS_SUCCESS);
        TS(FsctlOut(v, FSCTL_IS_VOLUME_DIRTY, &out), STATUS_SUCCESS); T(out == 1);
        T(BootDirty() == 1);
        TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);
        T(BootDirty() == 1);
        TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
        TS(Fsctl(v, FSCTL_UNLOCK_VOLUME), STATUS_SUCCESS);
        Close(v);
        LazyWriteAll(); RunWorkers();
        T(BootDirty() == 1);
        T(VolumeDirty() == 1);                   /* mounted dirty: it stays dirty */
        PutFile("/while-dirty.txt", 999);
        TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
        if (v) { TS(Flush(v), STATUS_SUCCESS); Close(v); }
        T(BootDirty() == 1);
        Unmount(); ClearDirtyOnDisk();
        T(VolumeDirty() == 0);
    }

    /* EnableWriteSupport = 0: the next mount is read-only and writes nothing; format and check tools still write through the lock */
    Unmount();
    setenv("REG_EnableWriteSupport", "0", 1);
    {
        int w0 = g_disk_writes; union { FILE_FS_ATTRIBUTE_INFORMATION a; char b[512]; } ai; unsigned char sec[4096];
        TS(W_CREATE("/verify.bin", RW_ACCESS, 7, FILE_OPEN, 0, 0, &f, &info), STATUS_MEDIA_WRITE_PROTECTED);
        TS(W_CREATE("/new-while-ro.txt", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_MEDIA_WRITE_PROTECTED);
        TS(OPEN_R("\\verify.bin", &f), STATUS_SUCCESS);
        if (f) { VerifyFile(MGet("/verify.bin"), 0); Close(f); }
        TS(OPEN_R("\\", &f), STATUS_SUCCESS);
        if (f) {
            TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsAttributeInformation, &ai, sizeof(ai), &info), STATUS_SUCCESS);
            T(ai.a.FileSystemAttributes & FILE_READ_ONLY_VOLUME);
            Close(f);
        }
        TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
        if (v) {
            TS(Flush(v), STATUS_SUCCESS);
            TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);
            T(g_disk_writes == w0);
            TS(Read(v, 0, g_sector, sec, TRUE, &info), STATUS_SUCCESS);
            TS(Write(v, 0, g_sector, sec, TRUE, &info), STATUS_SUCCESS);
            T(g_disk_writes == w0 + 1);
            TS(Fsctl(v, FSCTL_UNLOCK_VOLUME), STATUS_SUCCESS);
            Close(v);
        }
        LazyWriteAll(); RunWorkers();
        T(g_disk_writes == w0 + 1);
    }
    unsetenv("REG_EnableWriteSupport");
    Unmount();
    PutFile("/after-ro.txt", 100);

    /* many handles, then a surprise removal: every close still arrives and the volume goes */
    before = g_devices;
    {
        PFILE_OBJECT hs[8];
        for (k = 0; k < 8; k++) {
            char p[64]; sprintf(p, "/multi%d.bin", k);
            PutFile(p, 1000 + k * 4096);
            TS(W_CREATE(p, RW_ACCESS, 7, FILE_OPEN, 0, 0, &hs[k], &info), STATUS_SUCCESS);
        }
        TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
        if (v) { TS(Flush(v), STATUS_SUCCESS); Close(v); }
        TS(Pnp(IRP_MN_SURPRISE_REMOVAL), STATUS_SUCCESS);
        for (k = 7; k >= 0; k--) if (hs[k]) Close(hs[k]);
        LazyWriteAll(); RunWorkers();
        T(g_devices == w_base_dev);
        T(VolumeDirty() == 0);
    }
    (void)before;
    Remount();
    VerifyTree(&g_root, 1);
}

/* ---------------- formatting a mounted volume, as exfmt.exe does ---------------- */

#include "../fmt/exfmt.h"

static int CountMatches(const char *dir, const char *pattern, FILE_INFORMATION_CLASS cls, ULONG buflen, ULONG flags);

static int FmtWrite(void *Context, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    ULONG_PTR info = 0;
    NTSTATUS st = Write((PFILE_OBJECT)Context, (LONGLONG)Offset, Length, Buffer, TRUE, &info);
    TS(st, STATUS_SUCCESS);
    return NT_SUCCESS(st) && info == Length;
}

static void Reformat(void)
{
    PFILE_OBJECT v, busy, f; ULONG_PTR info; EXFMT_PARAMS P; EXFMT_LAYOUT L; ULONG out;
    union { FILE_FS_VOLUME_INFORMATION v; FILE_FS_SIZE_INFORMATION s; char b[512]; } vi; char l[64];
    int r;

    memset(&P, 0, sizeof(P));
    P.SectorSize = g_sector; P.VolumeSectors = (EXU64)g_partlen / g_sector;
    P.ClusterSize = getenv("REFORMAT")[0] ? (EXU32)atoi(getenv("REFORMAT")) : 0;
    P.Serial = 0x5EED0001; P.LabelLength = 4; P.Label[0] = 'Y'; P.Label[1] = 'e'; P.Label[2] = 'n'; P.Label[3] = 'i';
    r = ExfmtLayout(&P, &L); T(r == EXFMT_OK); if (r != EXFMT_OK) return;

    /* a file still open: the lock is refused, so exfmt /X dismounts first */
    TS(W_CREATE("/busy.bin", RW_ACCESS, 7, FILE_OVERWRITE_IF, 0, 0, &busy, &info), STATUS_SUCCESS);
    if (busy) { RandBytes(g_rbuf, 5000); TS(Write(busy, 0, 5000, g_rbuf, FALSE, &info), STATUS_SUCCESS); }
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS); if (!v) return;
    TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_ACCESS_DENIED);
    TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
    Close(v);
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS); if (!v) return;
    TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);

    /* the old volume's handle, its dirty data and lazy writes must not reach the new one */
    r = ExfmtFormat(&P, &L, FmtWrite, NULL, v); T(r == EXFMT_OK);
    if (busy) { TS(Write(busy, 0, 10, g_rbuf, FALSE, &info), EXF_STATUS_DISMOUNTED_T); Close(busy); }
    LazyWriteAll(); RunWorkers();
    TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
    TS(Fsctl(v, FSCTL_UNLOCK_VOLUME), STATUS_SUCCESS);
    Close(v);
    LazyWriteAll(); RunWorkers();
    T(g_devices == w_base_dev);
    g_lost_dirty = 0;

    /* the new volume: empty, labelled, clean */
    while (g_root.child) { MNODE *c = g_root.child; MUnlink(c); MFree(c); }
    TS(OPEN_R("\\", &f), STATUS_SUCCESS); if (!f) return;
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsVolumeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
    ToUtf8(vi.v.VolumeLabel, vi.v.VolumeLabelLength / 2, l); T(!strcmp(l, "Yeni")); T(vi.v.VolumeSerialNumber == 0x5EED0001);
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsSizeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
    T(vi.s.TotalAllocationUnits.QuadPart == L.ClusterCount);
    T(vi.s.AvailableAllocationUnits.QuadPart == L.ClusterCount - L.UsedClusters);
    T(vi.s.SectorsPerAllocationUnit * vi.s.BytesPerSector == L.ClusterSize);
    TS(FsctlOut(f, FSCTL_IS_VOLUME_DIRTY, &out), STATUS_SUCCESS); T(out == 0);
    Close(f);
    T(CountMatches("\\", "*", FileNamesInformation, 4096, 0) == 0);
    g_cluster = L.ClusterSize;
    VerifyTree(&g_root, 1);
}

/* ---------------- the boot-time check, as exfachk.exe does it ---------------- */

#include "../chk/exfchk.h"

static PFILE_OBJECT g_chkvol;
static int ChkRead(void *c, EXU64 Offset, void *Buffer, EXU32 Length)
{
    ULONG_PTR info = 0; NTSTATUS st = Read(g_chkvol, (LONGLONG)Offset, Length, Buffer, TRUE, &info);
    return NT_SUCCESS(st) && info == Length;
}
static int ChkWrite(void *c, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    ULONG_PTR info = 0; NTSTATUS st = Write(g_chkvol, (LONGLONG)Offset, Length, Buffer, TRUE, &info);
    return NT_SUCCESS(st) && info == Length;
}
static void *ChkAlloc(void *c, EXU32 Size) { return calloc(1, Size); }
static void ChkFree(void *c, void *p) { free(p); }
static void ChkPrint(void *c, const EXU16 *Text, EXU32 Length) { if (g_log) fprintf(stderr, "exfchk: %u chars\n", (unsigned)Length); }

/* Opens, locks, checks and repairs, dismounts: the result of ExcCheck */
static int BootCheck(EXC_RESULT *R)
{
    EXC_PARAMS P; EXC_HOST H; int r;
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &g_chkvol), STATUS_SUCCESS);
    if (!g_chkvol) return EXC_FAILED;
    TS(Fsctl(g_chkvol, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);
    memset(&P, 0, sizeof(P)); memset(&H, 0, sizeof(H));
    P.SectorSize = g_sector; P.VolumeSectors = (EXU64)g_partlen / g_sector; P.Fix = 1;
    H.Read = ChkRead; H.Write = ChkWrite; H.Alloc = ChkAlloc; H.Free = ChkFree; H.Print = ChkPrint;
    r = ExcCheck(&P, &H, R);
    TS(Fsctl(g_chkvol, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
    TS(Fsctl(g_chkvol, FSCTL_UNLOCK_VOLUME), STATUS_SUCCESS);
    Close(g_chkvol); g_chkvol = NULL;
    LazyWriteAll(); RunWorkers();
    T(g_devices == w_base_dev);
    return r;
}

static void BootCheckTests(void)
{
    EXC_RESULT R; PFILE_OBJECT v, f; ULONG_PTR info; ULONG out; unsigned char junk[16];

    /* a clean volume: nothing to fix, nothing written */
    Unmount();
    {
        int w0 = g_disk_writes;
        T(BootCheck(&R) == EXC_CLEAN);
        T(g_disk_writes == w0);
        T(R.Files == MCount(&g_root, 0));
    }

    /* marked dirty while mounted (the "check at the next restart" answer), backup boot region damaged */
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (v) { TS(Fsctl(v, FSCTL_MARK_VOLUME_DIRTY), STATUS_SUCCESS); Close(v); }
    Unmount();
    memset(junk, 0x5A, sizeof(junk));
    fseeko(g_img, 12 * (off_t)g_sector + 200, SEEK_SET); fwrite(junk, 1, sizeof(junk), g_img); fflush(g_img);
    T(BootDirty() == 1);
    T(BootCheck(&R) == EXC_FIXED);
    T(R.Problems == 2 && R.Fixed == 2 && R.WasDirty);
    T(BootDirty() == 0);
    T(VolumeDirty() == 0);

    /* and afterwards the volume is as it was */
    VerifyTree(&g_root, 1);
    TS(W_CREATE("/after-check.txt", RW_ACCESS, 7, FILE_CREATE, 0, 0, &f, &info), STATUS_SUCCESS);
    if (f) { Close(f); MNew("/after-check.txt", 0)->attr = 0x20; }
    Unmount();
    T(BootCheck(&R) == EXC_CLEAN);
    TS(OPEN_R("\\", &v), STATUS_SUCCESS);
    if (v) { TS(FsctlOut(v, FSCTL_IS_VOLUME_DIRTY, &out), STATUS_SUCCESS); T(out == 0); Close(v); }
}

/* ---------------- random operations ---------------- */

typedef struct { PFILE_OBJECT f; MNODE *n; int nocache, wrote, sizechg; } WHANDLE;
static WHANDLE g_h[16]; static int g_nh;
static int g_names;

static MNODE *PickNode(MNODE *d, int wantdir, int depth)
{
    MNODE *c, *pick = NULL; int k = 0;
    for (c = d->child; c; c = c->next) {
        if (c->dir && depth < 6 && Rand() % 3 == 0) { MNODE *x = PickNode(c, wantdir, depth + 1); if (x) return x; }
        if (c->dir == wantdir && !c->delpending && Rand() % (++k) == 0) pick = c;
    }
    if (!pick && wantdir && Rand() % 2 == 0) return d;
    return pick;
}

static void HClose(int k)
{
    WHANDLE *h = &g_h[k]; MNODE *n = h->n;
    Close(h->f);
    if (h->wrote && !n->dir) { n->attr |= 0x20; n->wtime = 0; }
    n->opens--;
    if (n->delpending && n->opens == 0) { MUnlink(n); MFree(n); }
    g_h[k] = g_h[--g_nh];
}

static void CloseAll(void) { while (g_nh) HClose(g_nh - 1); }

static size_t MBytes(MNODE *d) { size_t k = 0; MNODE *c; for (c = d->child; c; c = c->next) k += c->dir ? MBytes(c) + 4096 : c->size + 4096; return k; }

static void RandomOp(size_t limit)
{
    unsigned op = Rand() % 100; char path[1300], name[200]; PFILE_OBJECT f; ULONG_PTR info; NTSTATUS st; MNODE *d, *n;

    if (MBytes(&g_root) > limit * 4 && op < 60) op = 70 + Rand() % 6;   /* full enough: close and delete */

    if (op < 12) {                                              /* create or open a file */
        d = PickNode(&g_root, 1, 0); if (!d) d = &g_root;
        if (d->delpending || g_nh >= 16) return;
        n = (Rand() % 4 == 0) ? PickNode(d, 0, 6) : NULL;
        if (n && n->parent != d) n = NULL;
        if (n) strcpy(name, Rand() % 2 ? n->name : n->name);
        else {
            static const char *pre[] = { "file", "Dosya", "αρχείο", "файл", "Data File", "x" };
            snprintf(name, sizeof(name), "%s %d%s", pre[Rand() % 6], g_names++, Rand() % 3 ? ".bin" : "");
        }
        MPath(d, path); strcat(path, "/"); strcat(path, name);
        {
            ULONG disp = n ? (ULONG[]){ FILE_OPEN, FILE_OPEN_IF, FILE_OVERWRITE, FILE_OVERWRITE_IF, FILE_SUPERSEDE }[Rand() % 5] : (Rand() % 2 ? FILE_CREATE : FILE_OPEN_IF);
            int nocache = Rand() % 5 == 0;
            if (n && (n->attr & 1) ) disp = FILE_OPEN;
            st = W_CREATE(path, (n && (n->attr & 1)) ? FILE_GENERIC_READ : RW_ACCESS, 7, disp, nocache ? 0x8 : 0, 0, &f, &info);
            if (n && n->dir) return;
            if (n && (n->attr & 6) && (disp == FILE_OVERWRITE || disp == FILE_OVERWRITE_IF || disp == FILE_SUPERSEDE)) { TS(st, STATUS_ACCESS_DENIED); return; }
            TS(st, STATUS_SUCCESS); if (!f) return;
            LOG((stderr, "open %s disp=%u nocache=%d -> %x\n", path, (unsigned)disp, nocache, (unsigned)st));
            if (!n) { n = MAdd(d, name, 0); T(info == FILE_CREATED); }
            else if (disp == FILE_OVERWRITE || disp == FILE_OVERWRITE_IF) { MResize(n, 0); n->attr |= 0x20; n->wtime = 0; }
            else if (disp == FILE_SUPERSEDE) { MResize(n, 0); n->attr = 0x20; n->wtime = 0; }
            n->opens++;
            g_h[g_nh].f = f; g_h[g_nh].n = n; g_h[g_nh].nocache = nocache; g_h[g_nh].wrote = 0; g_nh++;
        }
        return;
    }

    if (op < 16) {                                              /* mkdir */
        d = PickNode(&g_root, 1, 0); if (!d || d->delpending) return;
        snprintf(name, sizeof(name), "%s %d", Rand() % 2 ? "Klasör" : "dir", g_names++);
        MPath(d, path); strcat(path, "/"); strcat(path, name);
        Mkdir(path);
        return;
    }

    if (op < 45 && g_nh) {                                      /* write */
        WHANDLE *h = &g_h[Rand() % g_nh]; size_t off, len; int how = Rand() % 10;
        n = h->n; if (n->attr & 1) return;
        len = 1 + Rand() % (Rand() % 4 == 0 ? 200000 : 5000);
        switch (Rand() % 6) {
        case 0: off = n->size; break;                                        /* append */
        case 1: off = n->size + Rand() % (3 * g_cluster + 1); break;         /* past the end: a gap */
        case 2: off = n->size ? Rand() % n->size : 0; break;                /* inside */
        case 3: off = (size_t)(Rand() % 4) * g_cluster; break;
        default: off = n->size ? Rand() % (n->size + 1) : 0; break;
        }
        if (h->nocache) { off &= ~(size_t)(g_sector - 1); len = (len + g_sector - 1) & ~(size_t)(g_sector - 1); if (len > 65536) len = 65536; }
        if (off + len > limit) return;
        RandBytes(g_rbuf, len);
        if (how == 0 && !h->nocache && off + len <= ((n->size + g_cluster - 1) / g_cluster) * g_cluster && FastWrite(h->f, (LONGLONG)off, (ULONG)len, g_rbuf)) {
            LOG((stderr, "fastwrite %s off=%zu len=%zu size=%zu\n", n->name, off, len, n->size));
            MWrite(n, off, g_rbuf, len); h->wrote = 1; return;
        }
        LOG((stderr, "write %s off=%zu len=%zu size=%zu nocache=%d eof=%d\n", n->name, off, len, n->size, h->nocache, how == 1 && !h->nocache && off == n->size));
        if (how == 1 && !h->nocache && off == n->size) st = Write(h->f, -1, (ULONG)len, g_rbuf, FALSE, &info);
        else st = Write(h->f, (LONGLONG)off, (ULONG)len, g_rbuf, (BOOLEAN)h->nocache, &info);
        if (st == STATUS_DISK_FULL) return;
        TS(st, STATUS_SUCCESS); if (NT_SUCCESS(st)) { T(info == len); MWrite(n, off, g_rbuf, len); h->wrote = 1; }
        return;
    }

    if (op < 52 && g_nh) {                                      /* set size */
        WHANDLE *h = &g_h[Rand() % g_nh]; size_t size;
        n = h->n; if (n->attr & 1) return;
        size = Rand() % 3 == 0 ? n->size / 2 : n->size + Rand() % (4 * g_cluster + 1);
        if (size > limit) return;
        if (Rand() % 4 == 0 && size <= n->size) { st = SetAlloc(h->f, (LONGLONG)size); }
        else st = SetEof(h->f, (LONGLONG)size);
        LOG((stderr, "setsize %s %zu -> %zu\n", n->name, n->size, size));
        if (st == STATUS_DISK_FULL) return;
        TS(st, STATUS_SUCCESS); if (NT_SUCCESS(st)) { if (size != n->size) h->wrote = 1; MResize(n, size); }
        return;
    }

    if (op < 60 && g_nh) {                                      /* read and compare */
        WHANDLE *h = &g_h[Rand() % g_nh]; size_t off, len; unsigned char *buf;
        n = h->n; if (!n->size) return;
        off = Rand() % n->size; len = 1 + Rand() % 70000;
        if (h->nocache) { off &= ~(size_t)(g_sector - 1); len = (len + g_sector - 1) & ~(size_t)(g_sector - 1); }
        buf = malloc(len);
        st = Read(h->f, (LONGLONG)off, (ULONG)len, buf, (BOOLEAN)h->nocache, &info);
        TS(st, STATUS_SUCCESS);
        if (NT_SUCCESS(st)) {
            size_t want = off + len > n->size ? n->size - off : len;
            if (info != want || memcmp(buf, n->data + off, want)) { fprintf(stderr, "read back of '%s' at %zu differs\n", n->name, off); g_errors++; }
        }
        free(buf);
        return;
    }

    if (op < 70 && g_nh) { int k = Rand() % g_nh; LOG((stderr, "close %s\n", g_h[k].n->name)); HClose(k); return; }    /* close */

    if (op < 76) {                                              /* delete */
        int dir = Rand() % 4 == 0;
        n = PickNode(&g_root, dir, 0); if (!n || n == &g_root) return;
        MPath(n, path);
        st = Open(path, NULL, DELETE, 7, FILE_OPEN, 0, &f);
        TS(st, STATUS_SUCCESS); if (!f) return;
        st = SetDelete(f, TRUE);
        if (n->attr & 1) TS(st, STATUS_CANNOT_DELETE);
        else if (n->dir && n->child) TS(st, STATUS_DIRECTORY_NOT_EMPTY);
        else { TS(st, STATUS_SUCCESS); if (NT_SUCCESS(st)) n->delpending = 1; }
        Close(f);
        if (n->delpending && n->opens == 0) { MUnlink(n); MFree(n); }
        return;
    }

    if (op < 84) {                                              /* rename or move */
        MNODE *target; int replace = Rand() % 3 == 0;
        n = PickNode(&g_root, Rand() % 3 == 0, 0); if (!n || n == &g_root) return;
        target = PickNode(&g_root, 1, 0); if (!target || target->delpending) return;
        MPath(n, path);
        st = Open(path, NULL, DELETE, 7, FILE_OPEN, 0, &f); TS(st, STATUS_SUCCESS); if (!f) return;
        {
            char to[1300]; MNODE *clash; int inside = 0; MNODE *w;
            for (w = target; w; w = w->parent) if (w == n) inside = 1;
            if (Rand() % 4 == 0 && target->child) { MNODE *c = PickNode(target, 0, 6); if (c && c->parent == target) strcpy(name, c->name); else snprintf(name, sizeof(name), "renamed %d", g_names++); }
            else snprintf(name, sizeof(name), "%s %d", Rand() % 2 ? "Taşındı" : "renamed", g_names++);
            MPath(target, to); strcat(to, "/"); strcat(to, name);
            clash = MFind(target, name);
            st = Rename(f, to[0] ? to : "/", (BOOLEAN)replace);
            if (target == n) TS(st, STATUS_SHARING_VIOLATION);        /* its own handle denies sharing delete */
            else if (inside && n->dir) TS(st, STATUS_INVALID_PARAMETER);
            else if (n->dir && MOpenBelow(n)) TS(st, STATUS_ACCESS_DENIED);
            else if (clash && clash != n && !replace) TS(st, STATUS_OBJECT_NAME_COLLISION);
            else if (clash && clash != n && (clash->dir || (clash->attr & 1) || clash->opens)) TS(st, STATUS_ACCESS_DENIED);
            else {
                TS(st, STATUS_SUCCESS);
                if (NT_SUCCESS(st)) {
                    if (clash && clash != n) { MUnlink(clash); MFree(clash); }
                    MUnlink(n); n->parent = target; n->next = target->child; target->child = n; strcpy(n->name, name);
                }
            }
        }
        Close(f);
        return;
    }

    if (op < 88) {                                              /* attributes */
        ULONG a = (Rand() % 2 ? FILE_ATTRIBUTE_HIDDEN : 0) | (Rand() % 5 == 0 ? FILE_ATTRIBUTE_READONLY : 0) | (Rand() % 2 ? FILE_ATTRIBUTE_ARCHIVE : 0);
        n = PickNode(&g_root, Rand() % 3 == 0, 0); if (!n || n == &g_root) return;
        if (n->opens && (a & 1)) return;                         /* keep open handles writable */
        MPath(n, path);
        TS(Open(path, NULL, FILE_WRITE_ATTRIBUTES, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS); if (!f) return;
        TS(SetBasic(f, a ? a : FILE_ATTRIBUTE_NORMAL, 0, 0, 0), STATUS_SUCCESS);
        Close(f);
        n->attr = (a & 0x27) | (n->dir ? 0x10 : 0);
        return;
    }

    if (op < 92) { LOG((stderr, "lazy\n")); LazyWriteAll(); return; }
    if (op < 94) { LOG((stderr, "trim\n")); MmTrimAll(); return; }
    if (op < 96 && g_nh) { TS(Flush(g_h[Rand() % g_nh].f), STATUS_SUCCESS); return; }
    if (op < 97) { LOG((stderr, "remount\n")); CloseAll(); Remount(); return; }
    if (op < 99) { VerifyTree(&g_root, 0); return; }
}

static void WriteTests(PDRIVER_OBJECT drv, int base_pool, int base_dev)
{
    int ops = getenv("OPS") ? atoi(getenv("OPS")) : 3000, k; ULONGLONG free0; FILE *out; size_t limit;
    PFILE_OBJECT f; ULONG_PTR info; FILE_FS_SIZE_INFORMATION si;
    w_drv = drv; w_base_pool = base_pool; w_base_dev = base_dev;
    g_log = getenv("LOG") != NULL;
    g_rng = (unsigned)atoi(getenv("WRITE"));
    g_rbuf = malloc(4 << 20);

    /* the volume as mkfs left it: find the cluster size, empty the model */
    TS(OPEN_R("\\", &f), STATUS_SUCCESS);
    if (f) { TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsSizeInformation, &si, sizeof(si), &info), STATUS_SUCCESS); Close(f); }
    g_cluster = si.SectorsPerAllocationUnit * si.BytesPerSector;
    free0 = FreeClusters();
    Remount();                                                   /* the first mount changed nothing */

    if (!getenv("NOSCRIPT")) { Scripted(); Robustness(); BootCheckTests(); }

    limit = (size_t)((FreeClusters() * (ULONGLONG)g_cluster) / 6);
    if (limit > (8u << 20)) limit = 8u << 20;
    for (k = 0; k < ops; k++) {
        RandomOp(limit);
        if (g_errors > 20) { fprintf(stderr, "too many errors, stopping at op %d\n", k); break; }
    }
    CloseAll();
    LazyWriteAll();
    VerifyTree(&g_root, 1);
    Remount();
    VerifyTree(&g_root, 1);
    Remount();

    /* format it again while mounted, then keep going on the new volume */
    if (getenv("REFORMAT")) {
        Reformat();
        limit = (size_t)((FreeClusters() * (ULONGLONG)g_cluster) / 6);
        if (limit > (8u << 20)) limit = 8u << 20;
        for (k = 0; k < ops / 2; k++) {
            RandomOp(limit);
            if (g_errors > 20) { fprintf(stderr, "too many errors, stopping at op %d\n", k); break; }
        }
        CloseAll();
        LazyWriteAll();
        VerifyTree(&g_root, 1);
        Remount();
        VerifyTree(&g_root, 1);
    }

    if (getenv("MANIFEST")) {
        out = fopen(getenv("MANIFEST"), "w");
        Manifest(out, &g_root);
        fclose(out);
    }
    printf("write tests: %d ops, %d dirs, %d files, %d paging writes, %d fast writes, %d lock conflicts, %d notifications, %d deferred closes, %d chaos trims, %d switches, free %llu -> %llu\n",
           ops, MCount(&g_root, 1), MCount(&g_root, 0), g_paging_writes, g_fastio_writes, g_lock_conflicts, g_reports, g_workers_run, g_chaos_trims, g_switches,
           (unsigned long long)free0, (unsigned long long)FreeClusters());
    Remount();
    free(g_rbuf);
}
