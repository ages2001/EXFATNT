/* Runs EXFATNT against exFAT images through a fake I/O manager. */
#include "ntifs.h"
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <wctype.h>
#include "kern.h"

NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING);

static DRIVER_OBJECT g_fsdrv, g_diskdrv;
static PDEVICE_OBJECT g_disk;
static FILE *g_img;
static ULONG g_sector;
static LONGLONG g_partlen;
static int g_disk_reads, g_disk_writes, g_readonly, g_diskro, g_disk_flushes;

#if defined(EXF_NT4) || defined(EXF_NT31)
#define EXF_STATUS_DISMOUNTED_T ((NTSTATUS)0xC0000098L)    /* STATUS_FILE_INVALID */
#else
#define EXF_STATUS_DISMOUNTED_T ((NTSTATUS)0xC000026EL)    /* STATUS_VOLUME_DISMOUNTED */
#endif
#define FILE_LIST_DIRECTORY_R (FILE_READ_DATA | SYNCHRONIZE)
#define T(c) do { if (!(c)) { fprintf(stderr, "TEST FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); g_errors++; } } while (0)
#define TS(st, exp) do { NTSTATUS _s = (st); if (_s != (NTSTATUS)(exp)) { fprintf(stderr, "TEST FAILED %s:%d: %s = %08x, expected %08x\n", __FILE__, __LINE__, #st, (unsigned)_s, (unsigned)(exp)); g_errors++; } } while (0)

static void Balanced(const char *where)
{
    FinishOtherThreads();
    RunWorkers();
    if (g_crit || g_res || g_bcb) { fprintf(stderr, "UNBALANCED after %s: crit=%d res=%d bcb=%d\n", where, g_crit, g_res, g_bcb); g_errors++; g_crit = g_res = 0; }
}

/* ---------------- fake disk ---------------- */
static NTSTATUS DiskDispatch(PDEVICE_OBJECT d, PIRP i)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(i);
    NTSTATUS st = STATUS_SUCCESS;
    i->IoStatus.Information = 0;
    switch (sp->MajorFunction) {
    case IRP_MJ_READ: case IRP_MJ_WRITE: {
        LONGLONG off = sp->Parameters.Read.ByteOffset.QuadPart; ULONG len = sp->Parameters.Read.Length;
        PUCHAR buf = MmGetMdlVirtualAddress(i->MdlAddress);
        if (off % g_sector || len % g_sector) { fprintf(stderr, "unaligned disk I/O %lld %u\n", (long long)off, len); g_errors++; }
        if (len > i->MdlAddress->ByteCount) { fprintf(stderr, "disk I/O beyond MDL\n"); g_errors++; }
        if (off + len > g_partlen) { st = STATUS_INVALID_PARAMETER; break; }
        if ((d->Flags & DO_VERIFY_VOLUME) && !(sp->Flags & SL_OVERRIDE_VERIFY_VOLUME)) {
            IoSetHardErrorOrVerifyDevice(i, d); st = STATUS_VERIFY_REQUIRED; break;
        }
        if (sp->MajorFunction == IRP_MJ_WRITE && g_readonly) { fprintf(stderr, "write to a write-protected disk at %lld\n", (long long)off); g_errors++; st = STATUS_MEDIA_WRITE_PROTECTED; break; }
        fseeko(g_img, off, SEEK_SET);
        if (sp->MajorFunction == IRP_MJ_READ) { if (fread(buf, 1, len, g_img) != len) memset(buf, 0, len); g_disk_reads++; }
        else { fwrite(buf, 1, len, g_img); g_disk_writes++; }
        i->IoStatus.Information = len;
        break; }
    case IRP_MJ_DEVICE_CONTROL:
        if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
            DISK_GEOMETRY *g = i->AssociatedIrp.SystemBuffer; memset(g, 0, sizeof(*g));
            g->BytesPerSector = g_sector; g->SectorsPerTrack = 63; g->TracksPerCylinder = 255;
            g->Cylinders.QuadPart = g_partlen / (g_sector * 63 * 255);
            i->IoStatus.Information = sizeof(*g);
        } else if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_DISK_GET_PARTITION_INFO) {
            PARTITION_INFORMATION *p = i->AssociatedIrp.SystemBuffer; memset(p, 0, sizeof(*p));
            p->PartitionLength.QuadPart = g_partlen; p->PartitionType = 7; p->RecognizedPartition = 1;
            i->IoStatus.Information = sizeof(*p);
        } else if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_DISK_IS_WRITABLE) {
            st = g_diskro ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS;
        } else st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    case IRP_MJ_FLUSH_BUFFERS:
        g_disk_flushes++;
        break;
    default:
        st = STATUS_SUCCESS;   /* PnP and the rest */
    }
    i->IoStatus.Status = st;
    IoCompleteRequest(i, 0);
    return st;
}

static void SetupDisk(const char *img, ULONG sector)
{
    int k;
    VPB *vpb;
    g_img = fopen(img, "r+b"); if (!g_img) { perror(img); exit(2); }
    fseeko(g_img, 0, SEEK_END); g_partlen = ftello(g_img);
    g_sector = sector;
    for (k = 0; k < 28; k++) g_diskdrv.MajorFunction[k] = DiskDispatch;
    IoCreateDevice(&g_diskdrv, 0, NULL, FILE_DEVICE_DISK, 0, FALSE, &g_disk);
    g_disk->Flags &= ~DO_DEVICE_INITIALIZING;
    vpb = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), 'bpV'); memset(vpb, 0, sizeof(VPB)); vpb->RealDevice = g_disk; g_disk->Vpb = vpb;
}

/* ---------------- fake I/O manager ---------------- */
static NTSTATUS Mount(void)
{
    PIRP i = IoAllocateIrp(1, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL; sp->MinorFunction = IRP_MN_MOUNT_VOLUME;
    sp->Parameters.MountVolume.Vpb = g_disk->Vpb; sp->Parameters.MountVolume.DeviceObject = g_disk;
    IoCallDriver(g_fsdev, i); T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    if (NT_SUCCESS(st)) g_disk->Vpb->Flags |= VPB_MOUNTED;
    Balanced("mount");
    return st;
}

/* UTF-8 path to UTF-16 with surrogate pairs */
static PWCHAR Utf16(const char *s, size_t *out)
{
    size_t n = strlen(s), o = 0, k = 0; PWCHAR w = malloc((n + 1) * 2);
    while (k < n) {
        unsigned char c = (unsigned char)s[k]; unsigned cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; } else if (c < 0xE0) { cp = c & 0x1F; extra = 1; } else if (c < 0xF0) { cp = c & 0x0F; extra = 2; } else { cp = c & 0x07; extra = 3; }
        k++;
        while (extra--) cp = (cp << 6) | ((unsigned char)s[k++] & 0x3F);
        if (cp >= 0x10000) { cp -= 0x10000; w[o++] = (WCHAR)(0xD800 + (cp >> 10)); w[o++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); }
        else w[o++] = (WCHAR)cp;
    }
    w[o] = 0; *out = o;
    return w;
}

static int g_raw_reparse;     /* hand STATUS_REPARSE back instead of retrying like the I/O manager */
static NTSTATUS OpenOnce(const char *upath, PFILE_OBJECT related, ACCESS_MASK access, ULONG share, ULONG disp, ULONG options,
                         ULONG attrs, UCHAR spflags, PFILE_OBJECT *out, ULONG_PTR *infop);
static NTSTATUS OpenEx(const char *upath, PFILE_OBJECT related, ACCESS_MASK access, ULONG share, ULONG disp, ULONG options,
                       ULONG attrs, UCHAR spflags, PFILE_OBJECT *out, ULONG_PTR *infop)
{
    NTSTATUS st; int k;
    for (k = 0; k < 3; k++) {
        st = OpenOnce(upath, related, access, share, disp, options, attrs, spflags, out, infop);
#ifdef EXF_NT31
        /* no IO_REMOUNT: the caller tries again, as exfmt and exfatchk do */
        if (st == STATUS_WRONG_VOLUME && !g_raw_reparse) continue;
#endif
        if (st != STATUS_REPARSE || g_raw_reparse) break;
    }
    return st;
}
static NTSTATUS OpenOnce(const char *upath, PFILE_OBJECT related, ACCESS_MASK access, ULONG share, ULONG disp, ULONG options,
                         ULONG attrs, UCHAR spflags, PFILE_OBJECT *out, ULONG_PTR *infop)
{
    char path[1400]; size_t pk;
    PVPB vpb = g_disk->Vpb; PFILE_OBJECT f; PIRP i; PIO_STACK_LOCATION sp; NTSTATUS st;
    IO_SECURITY_CONTEXT sc; ACCESS_STATE as; size_t n; ULONG_PTR info;
    *out = NULL;
    strcpy(path, upath); for (pk = 0; path[pk]; pk++) if (path[pk] == '/') path[pk] = '\\';
    if (!(vpb->Flags & VPB_MOUNTED)) { st = Mount(); if (!NT_SUCCESS(st)) return st; }
    vpb->ReferenceCount++;
    f = calloc(1, sizeof(FILE_OBJECT)); f->Type = 5; f->DeviceObject = g_disk; f->Vpb = vpb; f->RefCount = 1; f->RelatedFileObject = related; g_fo++;
    f->FileName.Buffer = Utf16(path, &n); f->FileName.Length = f->FileName.MaximumLength = (USHORT)(n * 2);
    f->Flags = FO_SYNCHRONOUS_IO | ((options & 0x8) ? FO_NO_INTERMEDIATE_BUFFERING : 0) | ((options & 0x2) ? FO_WRITE_THROUGH : 0);
    memset(&as, 0, sizeof(as)); as.RemainingDesiredAccess = access;
    sc.AccessState = &as; sc.DesiredAccess = access;
    i = IoAllocateIrp(vpb->DeviceObject->StackSize, FALSE); sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_CREATE; sp->FileObject = f;
    sp->Parameters.Create.SecurityContext = &sc; sp->Parameters.Create.Options = (disp << 24) | options; sp->Parameters.Create.ShareAccess = (USHORT)share;
    sp->Parameters.Create.FileAttributes = (USHORT)attrs; sp->Flags = spflags;
    i->RequestorMode = UserMode;
    IoCallDriver(vpb->DeviceObject, i); T(i->Completed);
    st = i->IoStatus.Status; info = i->IoStatus.Information;
    if (st == STATUS_REPARSE) T(info == 1);
    else if (NT_SUCCESS(st) && disp == FILE_OPEN && !spflags) T(info == FILE_OPENED);
    if (infop) *infop = info;
    IoFreeIrp(i);
    if (!NT_SUCCESS(st) || st == STATUS_REPARSE) { vpb->ReferenceCount--; free(f->FileName.Buffer); free(f); g_fo--; }
    else *out = f;
    Balanced("create");
    return st;
}
static NTSTATUS Open(const char *upath, PFILE_OBJECT related, ACCESS_MASK access, ULONG share, ULONG disp, ULONG options, PFILE_OBJECT *out)
{
    return OpenEx(upath, related, access, share, disp, options, 0, 0, out, NULL);
}
#define OPEN_R(p, f) Open((p), NULL, FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, (f))

static void Close(PFILE_OBJECT f)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_CLEANUP; sp->FileObject = f;
    IoCallDriver(d, i); T(i->Completed); T(i->IoStatus.Status == STATUS_SUCCESS); IoFreeIrp(i);
    ObDereferenceObject(f);
    Balanced("close");
}

static NTSTATUS Simple(PFILE_OBJECT f, UCHAR mj, UCHAR mn, PIRP *keep, void (*setup)(PIRP, PIO_STACK_LOCATION, void *), void *arg)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f);
    PIRP i = IoAllocateIrp(d->StackSize, FALSE); PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(i); NTSTATUS st;
    sp->MajorFunction = mj; sp->MinorFunction = mn; sp->FileObject = f; i->RequestorMode = UserMode;
    setup(i, sp, arg);
    IoCallDriver(d, i);
    if (keep) { *keep = i; return i->IoStatus.Status; }
    T(i->Completed);
    st = i->IoStatus.Status;
    Balanced("request");
    return st;
}

struct rd { LONGLONG off; ULONG len; PVOID buf; ULONG flags; ULONG_PTR info; };
static void SetupRead(PIRP i, PIO_STACK_LOCATION sp, void *a) { struct rd *r = a; sp->Parameters.Read.ByteOffset.QuadPart = r->off; sp->Parameters.Read.Length = r->len; i->UserBuffer = r->buf; i->Flags = r->flags; }
static NTSTATUS Read(PFILE_OBJECT f, LONGLONG off, ULONG len, PVOID buf, BOOLEAN nocache, ULONG_PTR *info)
{
    PDEVICE_OBJECT d = IoGetRelatedDeviceObject(f); PIRP i; NTSTATUS st; struct rd r;
    r.off = off; r.len = len; r.buf = buf; r.flags = nocache ? IRP_NOCACHE : 0;
    Simple(f, IRP_MJ_READ, 0, &i, SetupRead, &r);
    T(i->Completed); st = i->IoStatus.Status; *info = i->IoStatus.Information; IoFreeIrp(i);
    (void)d;
    Balanced("read");
    return st;
}

struct qd { ULONG len; PVOID buf; PUNICODE_STRING name; FILE_INFORMATION_CLASS cls; ULONG flags; ULONG index; };
static void SetupQd(PIRP i, PIO_STACK_LOCATION sp, void *a)
{
    struct qd *q = a;
    sp->Parameters.QueryDirectory.Length = q->len; sp->Parameters.QueryDirectory.FileName = q->name;
    sp->Parameters.QueryDirectory.FileInformationClass = q->cls; sp->Parameters.QueryDirectory.FileIndex = q->index;
    sp->Flags = (UCHAR)q->flags; i->UserBuffer = q->buf;
}
static NTSTATUS QueryDir(PFILE_OBJECT f, FILE_INFORMATION_CLASS cls, PVOID buf, ULONG len, const char *pattern, ULONG flags, ULONG_PTR *info)
{
    PIRP i; NTSTATUS st; struct qd q; UNICODE_STRING u; size_t n;
    q.len = len; q.buf = buf; q.cls = cls; q.flags = flags; q.index = 0; q.name = NULL;
    if (pattern) { u.Buffer = Utf16(pattern, &n); u.Length = u.MaximumLength = (USHORT)(n * 2); q.name = &u; }
    Simple(f, IRP_MJ_DIRECTORY_CONTROL, IRP_MN_QUERY_DIRECTORY, &i, SetupQd, &q);
    T(i->Completed); st = i->IoStatus.Status; *info = i->IoStatus.Information; IoFreeIrp(i);
    if (pattern) free(u.Buffer);
    Balanced("querydir");
    return st;
}

struct qi { ULONG len; PVOID buf; ULONG cls; };
static void SetupQi(PIRP i, PIO_STACK_LOCATION sp, void *a) { struct qi *q = a; sp->Parameters.QueryFile.Length = q->len; sp->Parameters.QueryFile.FileInformationClass = q->cls; i->AssociatedIrp.SystemBuffer = q->buf; }
static NTSTATUS QueryInfo(PFILE_OBJECT f, UCHAR mj, ULONG cls, PVOID buf, ULONG len, ULONG_PTR *info)
{
    PIRP i; NTSTATUS st; struct qi q; q.len = len; q.buf = buf; q.cls = cls;
    Simple(f, mj, 0, &i, SetupQi, &q);
    T(i->Completed); st = i->IoStatus.Status; *info = i->IoStatus.Information; IoFreeIrp(i);
    Balanced("queryinfo");
    return st;
}

struct fc { ULONG code; PVOID out; ULONG outlen; };
static void SetupFc(PIRP i, PIO_STACK_LOCATION sp, void *a) { struct fc *c = a; sp->Parameters.FileSystemControl.FsControlCode = c->code; sp->Parameters.FileSystemControl.OutputBufferLength = c->outlen; i->AssociatedIrp.SystemBuffer = c->out; }
static NTSTATUS Fsctl(PFILE_OBJECT f, ULONG code)
{
    PIRP i; NTSTATUS st; struct fc c; ULONG out = 0; c.code = code; c.out = &out; c.outlen = 4;
    Simple(f, IRP_MJ_FILE_SYSTEM_CONTROL, IRP_MN_USER_FS_REQUEST, &i, SetupFc, &c);
    T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("fsctl");
    return st;
}

static NTSTATUS Pnp(UCHAR minor)
{
    PDEVICE_OBJECT d = g_disk->Vpb->DeviceObject; PIRP i; PIO_STACK_LOCATION sp; NTSTATUS st;
    if (!d) return STATUS_NO_SUCH_DEVICE;
    i = IoAllocateIrp(d->StackSize, FALSE); sp = IoGetNextIrpStackLocation(i);
    sp->MajorFunction = IRP_MJ_PNP; sp->MinorFunction = minor;
    IoCallDriver(d, i); T(i->Completed); st = i->IoStatus.Status; IoFreeIrp(i);
    Balanced("pnp");
    return st;
}

/* ---------------- reference data ---------------- */
typedef struct { char path[1200]; unsigned long long size, hash; long long mtime; int seen; } REF;
static REF *g_ref; static int g_nref; static unsigned long long g_ref_free;

static void LoadRef(const char *fn)
{
    FILE *f = fopen(fn, "r"); char line[1400];
    if (!f) { g_ref = calloc(1, sizeof(REF)); return; }
    g_ref = calloc(4000, sizeof(REF));
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "F ", 2)) {
            REF *r = &g_ref[g_nref++]; char *p = strstr(line, " size=");
            *p = 0; strcpy(r->path, line + 2);
            sscanf(p + 1, "size=%llu hash=%llx mtime=%lld", &r->size, &r->hash, &r->mtime);
        } else if (!strncmp(line, "statvfs", 7)) sscanf(line, "statvfs free=%llu", &g_ref_free);
    }
    fclose(f);
}
static REF *FindRef(const char *p) { int k; for (k = 0; k < g_nref; k++) if (!strcmp(g_ref[k].path, p)) return &g_ref[k]; return NULL; }

static unsigned long long Fnv(const unsigned char *p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL; size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
static void ToUtf8(const WCHAR *w, size_t n, char *out)
{
    size_t i; char *o = out;
    for (i = 0; i < n; i++) {
        unsigned c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | (c >> 6)); *o++ = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { *o++ = (char)(0xE0 | (c >> 12)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
        else { *o++ = (char)(0xF0 | (c >> 18)); *o++ = (char)(0x80 | ((c >> 12) & 0x3F)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    }
    *o = 0;
}

/* ---------------- tests ---------------- */
static unsigned g_rng = 12345;
static unsigned Rand(void) { g_rng = g_rng * 1103515245 + 12345; return (g_rng >> 8) & 0xFFFFFF; }

static void CheckFile(const char *path, REF *r)
{
    PFILE_OBJECT f, g; ULONG_PTR info; NTSTATUS st; unsigned char *buf; LONGLONG off; char np[1300];
    FILE_STANDARD_INFORMATION si; FILE_BASIC_INFORMATION bi; union { FILE_NAME_INFORMATION n; char b[4096]; } ni;
    st = OPEN_R(path, &f); TS(st, STATUS_SUCCESS); if (!f) return;
    TS(QueryInfo(f, IRP_MJ_QUERY_INFORMATION, FileStandardInformation, &si, sizeof(si), &info), STATUS_SUCCESS);
    T(si.EndOfFile.QuadPart == (LONGLONG)r->size); T(!si.Directory);
    TS(QueryInfo(f, IRP_MJ_QUERY_INFORMATION, FileBasicInformation, &bi, sizeof(bi), &info), STATUS_SUCCESS);
    {
        long long unix_t = bi.LastWriteTime.QuadPart / 10000000LL - 11644473600LL;
        if (llabs(unix_t - r->mtime) > 2) { fprintf(stderr, "mtime %s: %lld vs %lld\n", path, unix_t, r->mtime); g_errors++; }
    }
    TS(QueryInfo(f, IRP_MJ_QUERY_INFORMATION, FileNameInformation, &ni, sizeof(ni), &info), STATUS_SUCCESS);
    ToUtf8(ni.n.FileName, ni.n.FileNameLength / 2, np);
    { char want[1300]; size_t k; strcpy(want, path); for (k = 0; want[k]; k++) if (want[k] == '/') want[k] = '\\';
      if (strcmp(np, want)) { fprintf(stderr, "name info '%s' vs '%s'\n", np, want); g_errors++; } }

    buf = malloc(r->size + 70000);
    /* cached reads in odd chunk sizes */
    for (off = 0; off < (LONGLONG)r->size; ) {
        ULONG chunk = 1 + Rand() % 70000;
        st = Read(f, off, chunk, buf + off, FALSE, &info);
        TS(st, STATUS_SUCCESS);
        if (!NT_SUCCESS(st)) break;
        T(info == (ULONG)((LONGLONG)r->size - off < chunk ? (LONGLONG)r->size - off : chunk));
        off += info;
    }
    if (Fnv(buf, r->size) != r->hash) { fprintf(stderr, "cached content mismatch %s\n", path); g_errors++; }
    TS(Read(f, r->size, 10, buf, FALSE, &info), STATUS_END_OF_FILE);
    Close(f);

    /* non-cached, sector aligned */
    st = Open(path, NULL, FILE_GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, 0x8, &g); TS(st, STATUS_SUCCESS); if (!g) { free(buf); return; }
    memset(buf, 0x5A, r->size + 70000);
    for (off = 0; off < (LONGLONG)r->size; ) {
        ULONG chunk = g_sector * (1 + Rand() % 40);
        st = Read(g, off, chunk, buf + off, TRUE, &info);
        TS(st, STATUS_SUCCESS);
        if (!NT_SUCCESS(st)) break;
        off += info;
        if (info < chunk) break;
    }
    if (Fnv(buf, r->size) != r->hash) { fprintf(stderr, "noncached content mismatch %s\n", path); g_errors++; }
    { ULONG tail = (ULONG)(((r->size + g_sector - 1) & ~(unsigned long long)(g_sector - 1)) - r->size), k;
      for (k = 0; k < tail; k++) if (buf[r->size + k] != 0) { fprintf(stderr, "noncached tail not zero %s\n", path); g_errors++; break; } }
    if (r->size % g_sector) TS(Read(g, 1, g_sector, buf, TRUE, &info), STATUS_INVALID_PARAMETER);
    Close(g);
    free(buf);
}

static int g_listed;
static void Walk(const char *dir, int depth)
{
    PFILE_OBJECT f; NTSTATUS st; ULONG_PTR info; unsigned char *buf = malloc(3000); int first = 1, dots = 0;

    st = Open(dir[0] ? dir : "\\", NULL, FILE_LIST_DIRECTORY_R, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_DIRECTORY_FILE, &f);
    TS(st, STATUS_SUCCESS); if (!f) { free(buf); return; }
    for (;;) {
        PFILE_BOTH_DIR_INFORMATION e; ULONG pos = 0;
        st = QueryDir(f, FileBothDirectoryInformation, buf, 3000, first ? "*" : NULL, 0, &info);
        first = 0;
        if (st == STATUS_NO_MORE_FILES) break;
        TS(st, STATUS_SUCCESS); if (!NT_SUCCESS(st)) break;
        for (;;) {
            char name[1100], full[1300];
            e = (PFILE_BOTH_DIR_INFORMATION)(buf + pos);
            T(((ULONG_PTR)e & 7) == 0);
            T(pos + FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + e->FileNameLength <= info);
            ToUtf8(e->FileName, e->FileNameLength / 2, name);
            if (!strcmp(name, ".") || !strcmp(name, "..")) dots++;
            else {
                snprintf(full, sizeof(full), "%s/%s", dir, name);
                if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 8) Walk(full, depth + 1); }
                else {
                    REF *r = FindRef(full);
                    g_listed++;
                    if (!r) { fprintf(stderr, "unexpected file %s\n", full); g_errors++; }
                    else { r->seen++; T((unsigned long long)e->EndOfFile.QuadPart == r->size); CheckFile(full, r); }
                }
            }
            if (!e->NextEntryOffset) break;
            pos += e->NextEntryOffset;
        }
    }
    T(dots == (dir[0] ? 2 : 0));
    Close(f);
    free(buf);
}

#include "wtest.c"

static int CountMatches(const char *dir, const char *pattern, FILE_INFORMATION_CLASS cls, ULONG buflen, ULONG flags)
{
    PFILE_OBJECT f; NTSTATUS st; ULONG_PTR info; unsigned char *buf = malloc(buflen + 16); int n = 0, first = 1;
    st = Open(dir, NULL, FILE_LIST_DIRECTORY_R, 7, FILE_OPEN, FILE_DIRECTORY_FILE, &f); TS(st, STATUS_SUCCESS);
    for (;;) {
        ULONG pos = 0;
        st = QueryDir(f, cls, buf, buflen, first ? pattern : NULL, flags, &info);
        if (st == STATUS_NO_MORE_FILES || st == STATUS_NO_SUCH_FILE) { if (st == STATUS_NO_SUCH_FILE) T(first); break; }
        first = 0;
        if (st == STATUS_BUFFER_OVERFLOW) { n++; continue; }
        TS(st, STATUS_SUCCESS); if (!NT_SUCCESS(st)) break;
        for (;;) { ULONG next = *(PULONG)(buf + pos); n++; if (!next) break; pos += next; }
    }
    Close(f); free(buf);
    return n;
}

int main(int argc, char **argv)
{
    DRIVER_OBJECT *drv = &g_fsdrv; PFILE_OBJECT f, v, d1; ULONG_PTR info; NTSTATUS st; int k, base_pool, base_dev;
    union { FILE_FS_VOLUME_INFORMATION v; FILE_FS_SIZE_INFORMATION s; FILE_FS_ATTRIBUTE_INFORMATION a; char b[512]; } vi;
    char ref[600];
    setlocale(LC_ALL, "C.UTF-8");
    if (argc < 3) { fprintf(stderr, "harn IMAGE SECTOR\n"); return 2; }
    g_verbose = getenv("V") != NULL;
    /* RO: a write-protected disk; REGRO: EnableWriteSupport = 0 on a writable one */
    g_readonly = getenv("RO") != NULL || getenv("REGRO") != NULL;
    g_diskro = getenv("RO") != NULL;
    if (getenv("REGRO")) setenv("REG_EnableWriteSupport", "0", 1);
    g_chaos = getenv("CHAOS") ? (atoi(getenv("CHAOS")) > 0 ? atoi(getenv("CHAOS")) : 40) : 0;
    snprintf(ref, sizeof(ref), "%s.ref", argv[1]); LoadRef(ref);
    SetupDisk(argv[1], (ULONG)atoi(argv[2]));
    {
        static WCHAR path[] = L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\exfatnt";
        UNICODE_STRING rp; rp.Buffer = path; rp.Length = rp.MaximumLength = (USHORT)(sizeof(path) - 2);
        TS(DriverEntry(drv, &rp), STATUS_SUCCESS);
    }
    base_pool = g_pool; base_dev = g_devices;

    /* volume information */
    TS(OPEN_R("\\", &f), STATUS_SUCCESS);
    T(g_disk->Vpb->Flags & VPB_MOUNTED); T(g_disk->Vpb->DeviceObject != NULL);
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsVolumeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
    { char l[64]; ToUtf8(vi.v.VolumeLabel, vi.v.VolumeLabelLength / 2, l); T(!strcmp(l, getenv("LABEL") ? getenv("LABEL") : "Test Vol")); }
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsSizeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
    if (g_ref_free && (unsigned long long)vi.s.AvailableAllocationUnits.QuadPart != g_ref_free) { fprintf(stderr, "free %lld vs %llu\n", (long long)vi.s.AvailableAllocationUnits.QuadPart, g_ref_free); g_errors++; }
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsAttributeInformation, &vi, sizeof(vi), &info), STATUS_SUCCESS);
    { char l[64]; ToUtf8(vi.a.FileSystemName, vi.a.FileSystemNameLength / 2, l); T(!strcmp(l, "exFAT")); }
    TS(QueryInfo(f, IRP_MJ_QUERY_VOLUME_INFORMATION, FileFsVolumeInformation, &vi, FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + 4, &info), STATUS_BUFFER_OVERFLOW);
    Close(f);

    if (getenv("SWAP")) {
        PFILE_OBJECT old; unsigned char c[16]; PVPB oldvpb;
        TS(OPEN_R("\\small.txt", &old), STATUS_SUCCESS);
        oldvpb = g_disk->Vpb;
        fclose(g_img); g_img = fopen(getenv("SWAP"), "r+b"); fseeko(g_img, 0, SEEK_END); g_partlen = ftello(g_img);
        g_disk->Flags |= DO_VERIFY_VOLUME;
#ifndef EXF_NT31
        g_raw_reparse = 1;
        TS(OPEN_R("\\small.txt", &f), STATUS_REPARSE);
        g_raw_reparse = 0;
#else
        /* no IO_REMOUNT on NT 3.1: this open fails, the next one mounts */
        g_raw_reparse = 1;
        TS(OPEN_R("\\small.txt", &f), STATUS_WRONG_VOLUME);
        g_raw_reparse = 0;
#endif
        T(g_disk->Vpb != oldvpb);                         /* fresh VPB for the new medium */
        TS(Read(old, 0, 6, c, FALSE, &info), EXF_STATUS_DISMOUNTED_T);
        k = g_pool;
        Close(old);                                       /* last reference: the old volume goes */
        T(g_devices == base_dev);
        T(g_pool < k);
        TS(OPEN_R("\\small.txt", &f), STATUS_SUCCESS);   /* remounts the new medium */
        T(g_devices == base_dev + 1);
        if (f) { TS(Read(f, 0, 6, c, FALSE, &info), STATUS_SUCCESS); Close(f); }
        goto done;
    }

    if (getenv("PNP")) {
        PFILE_OBJECT h; unsigned char c[16];
        /* query-remove is vetoed while a file is open */
        TS(OPEN_R("\\small.txt", &h), STATUS_SUCCESS);
        TS(Pnp(IRP_MN_QUERY_REMOVE_DEVICE), STATUS_ACCESS_DENIED);
        TS(Read(h, 0, 6, c, FALSE, &info), STATUS_SUCCESS);
        /* surprise removal: open handle fails, last close tears down */
        TS(Pnp(IRP_MN_SURPRISE_REMOVAL), STATUS_SUCCESS);
        TS(Read(h, 0, 6, c, TRUE, &info), EXF_STATUS_DISMOUNTED_T);
        TS(Pnp(0x07 /* query device relations */), STATUS_NO_SUCH_DEVICE);
        Close(h);
        T(g_devices == base_dev); T(g_disk->ReferenceCount == 0); T(!(g_disk->Vpb->Flags & VPB_MOUNTED));
        /* remount, then an idle query-remove / remove */
        TS(OPEN_R("\\small.txt", &h), STATUS_SUCCESS); if (h) Close(h);
        k = g_devices;
        TS(Pnp(IRP_MN_QUERY_REMOVE_DEVICE), STATUS_SUCCESS);
        /* an idle volume is gone right away; the remove then goes to the disk alone */
        T(g_devices == base_dev); T(g_disk->Vpb->DeviceObject == NULL);
        TS(OPEN_R("\\small.txt", &h), STATUS_SUCCESS); if (h) Close(h);
        goto done;
    }

    if (getenv("BIG")) {
        unsigned long long size = strtoull(strchr(getenv("BIG"), ':') + 1, NULL, 10), marks[5], m;
        char path[256]; unsigned char blk[8192], want[64]; int j, q;
        FILE_STANDARD_INFORMATION si; PFILE_OBJECT g;
        strcpy(path, getenv("BIG")); *strchr(path, ':') = 0;
        marks[0] = 0; marks[1] = size / 3; marks[2] = 0xFFFFF000ULL; marks[3] = 0x100000000ULL + 12345; marks[4] = size - 4096;
        TS(OPEN_R(path, &f), STATUS_SUCCESS);
        TS(Open(path, NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0x8, &g), STATUS_SUCCESS);
        TS(QueryInfo(f, IRP_MJ_QUERY_INFORMATION, FileStandardInformation, &si, sizeof(si), &info), STATUS_SUCCESS);
        T((unsigned long long)si.EndOfFile.QuadPart == size);
        for (j = 0; j < 5; j++) {
            m = marks[j]; for (q = 0; q < 8; q++) memcpy(want + q * 8, &m, 8);
            TS(Read(f, (LONGLONG)m, 64, blk, FALSE, &info), STATUS_SUCCESS); T(info == 64 && !memcmp(blk, want, 64));
            TS(Read(g, (LONGLONG)(m & ~511ULL), 1024, blk, TRUE, &info), STATUS_SUCCESS); T(!memcmp(blk + (m & 511), want, 64));
        }
        TS(Read(f, (LONGLONG)size - 10, 100, blk, FALSE, &info), STATUS_SUCCESS); T(info == 10);
        TS(Read(f, (LONGLONG)size, 100, blk, FALSE, &info), STATUS_END_OF_FILE);
        TS(Read(g, (LONGLONG)size - 512, 4096, blk, TRUE, &info), STATUS_SUCCESS); T(info == 512);
        Close(f); Close(g);
        goto done;
    }

    if (getenv("WRITE")) {
        WriteTests(drv, base_pool, base_dev);
        goto end;
    }

    /* full tree: listing, content, sizes, times, names */
    Walk("", 0);
    for (k = 0; k < g_nref; k++) if (g_ref[k].seen != 1) { fprintf(stderr, "file seen %d times: %s\n", g_ref[k].seen, g_ref[k].path); g_errors++; }
    printf("listed %d files, %d paging reads, %d disk reads\n", g_listed, g_paging_reads, g_disk_reads);
    if (getenv("ONLYWALK")) goto done;

    /* patterns and odd buffers */
    T(CountMatches("\\many", "*", FileNamesInformation, 4096, 0) == CountMatches("\\many", NULL, FileDirectoryInformation, 200, 0));
    T(CountMatches("\\many", "*", FileBothDirectoryInformation, 100000, SL_RETURN_SINGLE_ENTRY) == CountMatches("\\many", "*", FileFullDirectoryInformation, 100000, 0));
    k = CountMatches("\\many", "FILE_WITH_A_REASONABLY_LONG_NAME_2?.TXT", FileNamesInformation, 4096, 0); T(k == 10);
    k = CountMatches("\\many", "*_3??.txt", FileNamesInformation, 4096, 0); T(k == 1);
    k = CountMatches("\\", "*.bin", FileDirectoryInformation, 4096, 0); T(k == 5);
    k = CountMatches("\\", "nothing*", FileDirectoryInformation, 4096, 0); T(k == 0);
    k = CountMatches("\\", "SMALL.TXT", FileDirectoryInformation, 4096, 0); T(k == 1);
    k = CountMatches("\\many", "*", FileBothDirectoryInformation, 0x5E + 10, 0); T(k == CountMatches("\\many", "*", FileNamesInformation, 4096, 0));

    /* names and errors */
    TS(OPEN_R("\\SMALL.TXT", &f), STATUS_SUCCESS); if (f) Close(f);
    TS(OPEN_R("\\ΕΛΛΗΝΙΚΆ\\ΑΒΓ.txt", &f), STATUS_SUCCESS); if (f) Close(f);
    TS(OPEN_R("\\КИРИЛЛИЦА\\ВЛОЖЕННАЯ\\ГЛУБЖЕ\\ФАЙЛ.DAT", &f), STATUS_SUCCESS); if (f) Close(f);
    TS(OPEN_R("\\nope.txt", &f), STATUS_OBJECT_NAME_NOT_FOUND);
    TS(OPEN_R("\\nope\\x.txt", &f), STATUS_OBJECT_PATH_NOT_FOUND);
    TS(OPEN_R("\\small.txt\\x", &f), STATUS_OBJECT_PATH_NOT_FOUND);
    TS(OPEN_R("\\small.txt\\", &f), STATUS_OBJECT_NAME_INVALID);
    TS(OPEN_R("\\sm*ll.txt", &f), STATUS_OBJECT_NAME_INVALID);
    TS(OPEN_R("\\many\\\\x", &f), STATUS_OBJECT_NAME_INVALID);
    TS(OPEN_R("small.txt", &f), STATUS_OBJECT_NAME_INVALID);
    TS(Open("\\small.txt", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, FILE_DIRECTORY_FILE, &f), STATUS_NOT_A_DIRECTORY);
    TS(Open("\\many", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &f), STATUS_FILE_IS_A_DIRECTORY);
    if (g_readonly) {
        TS(Open("\\small.txt", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &f), STATUS_MEDIA_WRITE_PROTECTED);
        TS(Open("\\new.txt", NULL, FILE_GENERIC_READ, 7, FILE_OPEN_IF, 0, &f), STATUS_MEDIA_WRITE_PROTECTED);
        TS(Open("\\small.txt", NULL, DELETE, 7, FILE_OPEN, 0, &f), STATUS_MEDIA_WRITE_PROTECTED);
    } else {
        /* a writable handle that writes nothing changes nothing */
        TS(Open("\\small.txt", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
        if (f) Close(f);
    }
    TS(Open("\\small.txt", NULL, FILE_GENERIC_READ, 7, FILE_CREATE, 0, &f), STATUS_OBJECT_NAME_COLLISION);
    TS(Open("\\small.txt", NULL, MAXIMUM_ALLOWED, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS); if (f) { T(f->ReadAccess); Close(f); }
    TS(Open("\\small.txt", NULL, FILE_GENERIC_READ, 0, FILE_OPEN, 0, &f), STATUS_SUCCESS);
    TS(Open("\\small.txt", NULL, FILE_GENERIC_READ, 7, FILE_OPEN, 0, &v), (NTSTATUS)0xC0000043L);
    if (f) Close(f);
    /* relative open */
    TS(Open("\\many", NULL, FILE_LIST_DIRECTORY_R, 7, FILE_OPEN, 0, &d1), STATUS_SUCCESS);
    if (d1) {
        TS(Open("FILE_WITH_A_REASONABLY_LONG_NAME_300.TXT", d1, FILE_GENERIC_READ, 7, FILE_OPEN, 0, &f), STATUS_SUCCESS);
        if (f) { char c[8]; TS(Read(f, 0, 8, c, FALSE, &info), STATUS_SUCCESS); T(info == 4 && !memcmp(c, "300\n", 4)); Close(f); }
        TS(Open("\\x", d1, FILE_GENERIC_READ, 7, FILE_OPEN, 0, &f), STATUS_OBJECT_NAME_INVALID);
        Close(d1);
    }
    /* fast I/O read path */
    TS(OPEN_R("\\odd.bin", &f), STATUS_SUCCESS);
    if (f) {
        unsigned char a[5000], b[5000]; IO_STATUS_BLOCK io; LARGE_INTEGER o; PDEVICE_OBJECT vd = IoGetRelatedDeviceObject(f);
        TS(Read(f, 12345, 5000, a, FALSE, &info), STATUS_SUCCESS);
        o.QuadPart = 12345;
        T(drv->FastIoDispatch->FastIoRead(f, &o, 5000, TRUE, 0, b, &io FIO_DEV(vd))); T(io.Status == STATUS_SUCCESS && io.Information == 5000);
        T(!memcmp(a, b, 5000)); T(g_fastio_reads == 1);
#ifndef EXF_NT31
        { FILE_NETWORK_OPEN_INFORMATION no; T(drv->FastIoDispatch->FastIoQueryNetworkOpenInfo(f, TRUE, &no, &io, vd)); T(no.EndOfFile.QuadPart == 1000003); }
#else
        T(drv->FastIoDispatch->FastIoQueryNetworkOpenInfo == NULL);
#endif
        Balanced("fastio");
        Close(f);
    }
    /* directory change notification stays pending until cleanup */
    TS(Open("\\many", NULL, FILE_LIST_DIRECTORY_R, 7, FILE_OPEN, 0, &d1), STATUS_SUCCESS);
    if (d1) {
        PIRP ni; struct qd q; memset(&q, 0, sizeof(q));
#ifndef EXF_NT31
        Simple(d1, IRP_MJ_DIRECTORY_CONTROL, IRP_MN_NOTIFY_CHANGE_DIRECTORY, &ni, SetupQd, &q);
        T(!ni->Completed); T(g_pending_notify == ni); Balanced("notify");
        Close(d1);
        T(ni->Completed); IoFreeIrp(ni);
#else
        /* not offered on NT 3.1 */
        TS(Simple(d1, IRP_MJ_DIRECTORY_CONTROL, IRP_MN_NOTIFY_CHANGE_DIRECTORY, &ni, SetupQd, &q), STATUS_INVALID_DEVICE_REQUEST);
        T(ni->Completed); T(g_pending_notify == NULL); IoFreeIrp(ni); Balanced("notify");
        Close(d1);
#endif
    }

    /* lock, dismount, teardown, remount */
    TS(Open("", NULL, FILE_GENERIC_READ | FILE_WRITE_DATA, 7, FILE_OPEN, 0, &v), STATUS_SUCCESS);
    if (v) {
        unsigned char s0[4096];
        TS(OPEN_R("\\small.txt", &f), STATUS_SUCCESS);
        TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_ACCESS_DENIED);
        Close(f);
        TS(Fsctl(v, FSCTL_LOCK_VOLUME), STATUS_SUCCESS);
        T(g_disk->Vpb->Flags & VPB_LOCKED);
        TS(OPEN_R("\\small.txt", &f), STATUS_ACCESS_DENIED);
        TS(Read(v, 0, g_sector, s0, TRUE, &info), STATUS_SUCCESS); T(!memcmp(s0 + 3, "EXFAT   ", 8));
        TS(Read(v, 1, g_sector, s0, TRUE, &info), STATUS_INVALID_PARAMETER);
        TS(Fsctl(v, FSCTL_DISMOUNT_VOLUME), STATUS_SUCCESS);
        TS(Read(v, 0, g_sector, s0, TRUE, &info), STATUS_SUCCESS);
        TS(Fsctl(v, FSCTL_UNLOCK_VOLUME), STATUS_SUCCESS);
        T(g_devices == base_dev + 1);
        Close(v);
        /* cached files keep FCBs referenced only through Cc until the dismount purge */
        T(g_devices == base_dev);
        T(g_disk->Vpb->DeviceObject == NULL); T(!(g_disk->Vpb->Flags & (VPB_MOUNTED | VPB_LOCKED)));
        T(g_disk->Vpb->ReferenceCount == 0);
        if (g_pool != base_pool) { fprintf(stderr, "pool leak after teardown: %d blocks\n", g_pool - base_pool); g_errors++; }
        T(g_fo == 0); T(g_mdl == 0); T(g_irp == 0); T(g_disk->ReferenceCount == 0);
    }
    TS(OPEN_R("\\odd.bin", &f), STATUS_SUCCESS);
    T(g_devices == base_dev + 1);
    if (f) Close(f);

    /* media change: same volume verifies, and the request is retried */
    g_disk->Flags |= DO_VERIFY_VOLUME;
    TS(OPEN_R("\\small.txt", &f), STATUS_SUCCESS);
    T(!(g_disk->Flags & DO_VERIFY_VOLUME));
    if (f) Close(f);

done:
    if (g_disk_writes != 0) { fprintf(stderr, "reading wrote %d times to the disk\n", g_disk_writes); g_errors++; }
end:
    printf("pool=%d devices=%d fo=%d errors=%d\n", g_pool - base_pool, g_devices, g_fo, g_errors);
    return g_errors != 0;
}
