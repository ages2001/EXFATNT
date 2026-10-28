/*
 * exfres.c - runtime binding of the executive-resource API for the single
 * universal (NT 3.1 - 4.0) binary.
 *
 * The resource functions split across NT versions with no statically-
 * importable common set:
 *   - NT 3.1 exports the legacy names (ExInitializeResource,
 *     ExAcquireResourceExclusive/Shared, ExReleaseResourceForThread,
 *     ExDeleteResource) and operates the OLD ERESOURCE layout. It has no
 *     Lite forms.
 *   - NT 3.51+ export the Lite names (ExInitializeResourceLite, ...) and
 *     operate the NEW ERESOURCE layout. The legacy names still exist for
 *     source compatibility, but on NT 3.51 routing our Lite-era code through
 *     the legacy compat path mismatches the structure and faults inside the
 *     kernel's resource wait (bugcheck 0xA on a bogus wait object).
 *
 * A static import that the running kernel does not export makes the OS loader
 * reject the driver as "missing or corrupt", so neither name set can be
 * imported outright. Instead the driver imports none of them and, at
 * DriverEntry, finds ntoskrnl in memory and resolves each function by name -
 * preferring the Lite form, falling back to the legacy form. Each NT then
 * runs its own native, structurally-matched resource functions (and true
 * shared acquisition is preserved everywhere).
 */

#include "exfat.h"

#ifdef EXF_NT31

EXF_PINITRES ExfpInitRes      = NULL;
EXF_PACQRES  ExfpAcqExcl      = NULL;
EXF_PACQRES  ExfpAcqShared    = NULL;
EXF_PDELRES  ExfpDelRes       = NULL;
EXF_PRELRES  ExfpRelForThread = NULL;
EXF_PNOTIFYSYNC      ExfpNotifyInitSync = NULL;
EXF_PNOTIFYSYNC      ExfpNotifyUninitSync = NULL;
EXF_PNOTIFYCLEANUP   ExfpNotifyCleanup = NULL;
EXF_PNOTIFYCHANGEDIR ExfpNotifyChangeDir = NULL;
EXF_PNOTIFYREPORT    ExfpNotifyReport = NULL;

typedef PIRP           (NTAPI *EXF_PGETTOP)(VOID);
typedef VOID           (NTAPI *EXF_PSETTOP)(PIRP);
typedef PDEVICE_OBJECT (NTAPI *EXF_PGETVERIFY)(PETHREAD);
typedef VOID           (NTAPI *EXF_PSETVERIFY)(PETHREAD, PDEVICE_OBJECT);
typedef PEPROCESS      (NTAPI *EXF_PGETREQ)(PIRP);

static EXF_PGETTOP    ExfpGetTop;
static EXF_PSETTOP    ExfpSetTop;
static EXF_PGETVERIFY ExfpGetVerify;
static EXF_PSETVERIFY ExfpSetVerify;
static EXF_PGETREQ    ExfpGetReq;

static int ExfpStrEq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

/*
 * Walk ntoskrnl's export directory (the image is mapped at Base with sections
 * at their RVAs) and return the address of the named export, or NULL.
 */
static PVOID ExfpFindExport(ULONG Base, const char *Name)
{
    ULONG   lfanew  = *(ULONG *)(Base + 0x3C);
    ULONG nt    = Base + lfanew;
    ULONG   expRva  = *(ULONG *)(nt + 0x78);     /* DataDirectory[0].VirtualAddress */
    ULONG ed;
    ULONG   nNames, funcsRva, namesRva, ordsRva, i;

    if (*(USHORT *)Base != 0x5A4D) return NULL;   /* 'MZ' */
    if (*(ULONG  *)nt   != 0x00004550) return NULL;/* 'PE\0\0' */
    if (expRva == 0) return NULL;

    ed       = Base + expRva;
    nNames   = *(ULONG *)(ed + 0x18);
    funcsRva = *(ULONG *)(ed + 0x1C);
    namesRva = *(ULONG *)(ed + 0x20);
    ordsRva  = *(ULONG *)(ed + 0x24);

    for (i = 0; i < nNames; i++) {
        ULONG   nameRva = *(ULONG  *)(Base + namesRva + i * 4);
        USHORT  ord     = *(USHORT *)(Base + ordsRva  + i * 2);
        ULONG   funcRva;
        if (ExfpStrEq((const char *)(Base + nameRva), Name)) {
            funcRva = *(ULONG *)(Base + funcsRva + ord * 4);
            return (PVOID)(Base + funcRva);
        }
    }
    return NULL;
}

static PVOID ExfpResolve2(ULONG Base, const char *Lite, const char *Legacy)
{
    PVOID p = ExfpFindExport(Base, Lite);
    if (p == NULL) p = ExfpFindExport(Base, Legacy);
    return p;
}

/*
 * Resolve the resource API against the running ntoskrnl. Called first thing in
 * DriverEntry, before any resource is initialised. Returns FALSE if ntoskrnl
 * could not be located or a function is missing (the driver then fails to
 * load cleanly rather than faulting later).
 */
BOOLEAN ExfResolveResourceApi(VOID)
{
    /* KeInitializeEvent is imported from ntoskrnl, so its address lands inside
     * the ntoskrnl image; round down to the page and scan back to the 'MZ'. */
    ULONG p = ((ULONG)(PVOID)&KeInitializeEvent) & ~(ULONG)0xFFF;
    ULONG limit = p - 0x02000000;            /* don't scan more than 32 MB */
    ULONG base = 0;

    for (; p > limit; p -= 0x1000) {
        if (*(USHORT *)p == 0x5A4D) {            /* 'MZ' */
            ULONG lfanew = *(ULONG *)(p + 0x3C);
            if (lfanew < 0x1000 && *(ULONG *)(p + lfanew) == 0x00004550) {
                base = p;
                break;
            }
        }
    }
    if (base == 0) return FALSE;

    ExfpInitRes      = (EXF_PINITRES)ExfpResolve2(base, "ExInitializeResourceLite",        "ExInitializeResource");
    ExfpAcqExcl      = (EXF_PACQRES) ExfpResolve2(base, "ExAcquireResourceExclusiveLite",  "ExAcquireResourceExclusive");
    ExfpAcqShared    = (EXF_PACQRES) ExfpResolve2(base, "ExAcquireResourceSharedLite",     "ExAcquireResourceShared");
    ExfpDelRes       = (EXF_PDELRES) ExfpResolve2(base, "ExDeleteResourceLite",            "ExDeleteResource");
    ExfpRelForThread = (EXF_PRELRES) ExfpResolve2(base, "ExReleaseResourceForThreadLite",  "ExReleaseResourceForThread");

    /* Change notification: live on NT 3.51 (build 1057) and later, whose
     * FsRtlNotifyFull package has the NT 4.0 interface; held pending below. */
    {
        PULONG Build = (PULONG)ExfpFindExport(base, "NtBuildNumber");
        if (Build != NULL && (*Build & 0xFFFF) >= 1057) {
            ExfpNotifyInitSync   = (EXF_PNOTIFYSYNC)ExfpFindExport(base, "FsRtlNotifyInitializeSync");
            ExfpNotifyUninitSync = (EXF_PNOTIFYSYNC)ExfpFindExport(base, "FsRtlNotifyUninitializeSync");
            ExfpNotifyCleanup    = (EXF_PNOTIFYCLEANUP)ExfpFindExport(base, "FsRtlNotifyCleanup");
            ExfpNotifyReport     = (EXF_PNOTIFYREPORT)ExfpFindExport(base, "FsRtlNotifyFullReportChange");
            ExfpNotifyChangeDir  = (EXF_PNOTIFYCHANGEDIR)ExfpFindExport(base, "FsRtlNotifyFullChangeDirectory");
            if (!ExfpNotifyInitSync || !ExfpNotifyUninitSync || !ExfpNotifyCleanup || !ExfpNotifyReport)
                ExfpNotifyChangeDir = NULL;
        }
    }

    ExfpGetTop    = (EXF_PGETTOP)ExfpFindExport(base, "IoGetTopLevelIrp");
    ExfpSetTop    = (EXF_PSETTOP)ExfpFindExport(base, "IoSetTopLevelIrp");
    if (ExfpGetTop == NULL || ExfpSetTop == NULL) ExfpGetTop = NULL, ExfpSetTop = NULL;
    ExfpGetVerify = (EXF_PGETVERIFY)ExfpFindExport(base, "IoGetDeviceToVerify");
    ExfpSetVerify = (EXF_PSETVERIFY)ExfpFindExport(base, "IoSetDeviceToVerify");
    if (ExfpGetVerify == NULL || ExfpSetVerify == NULL) ExfpGetVerify = NULL, ExfpSetVerify = NULL;
    ExfpGetReq    = (EXF_PGETREQ)ExfpFindExport(base, "IoGetRequestorProcess");

    return (ExfpInitRes && ExfpAcqExcl && ExfpAcqShared &&
            ExfpDelRes && ExfpRelForThread);
}

/* ------------------------------------------------------------------ */
/* Per-thread values (see exfat.h)                                     */
/* ------------------------------------------------------------------ */


#define EXF_TOP_SLOTS   64
static struct { PVOID Thread; PIRP Irp; } ExfpTop[EXF_TOP_SLOTS];

PIRP ExfGetTopLevelIrp(VOID)
{
    PVOID Me;
    ULONG i;

    if (ExfpGetTop != NULL) return ExfpGetTop();
    Me = (PVOID)KeGetCurrentThread();
    for (i = 0; i < EXF_TOP_SLOTS; i++) {
        if (ExfpTop[i].Thread == Me) return ExfpTop[i].Irp;
    }
    return NULL;
}

VOID ExfSetTopLevelIrp(PIRP Irp)
{
    PVOID Me;
    ULONG i;

    if (ExfpSetTop != NULL) { ExfpSetTop(Irp); return; }
    Me = (PVOID)KeGetCurrentThread();
    for (i = 0; i < EXF_TOP_SLOTS; i++) {
        if (ExfpTop[i].Thread == Me) {
            if (Irp == NULL) {
                ExfpTop[i].Irp = NULL;
                ExfpTop[i].Thread = NULL;
            } else {
                ExfpTop[i].Irp = Irp;
            }
            return;
        }
    }
    if (Irp == NULL) return;
    /* a free slot; only this thread ever writes the slot it claimed */
    for (i = 0; i < EXF_TOP_SLOTS; i++) {
        if (ExfpTop[i].Thread == NULL &&
            __sync_bool_compare_and_swap(&ExfpTop[i].Thread, NULL, Me)) {
            ExfpTop[i].Irp = Irp;
            return;
        }
    }
}

PDEVICE_OBJECT ExfGetDeviceToVerify(VOID)
{
    return (ExfpGetVerify != NULL) ? ExfpGetVerify((PETHREAD)KeGetCurrentThread()) : NULL;
}

VOID ExfSetDeviceToVerify(PDEVICE_OBJECT Device)
{
    if (ExfpSetVerify != NULL) ExfpSetVerify((PETHREAD)KeGetCurrentThread(), Device);
}

PEPROCESS ExfGetRequestorProcess(PIRP Irp)
{
    return (ExfpGetReq != NULL) ? ExfpGetReq(Irp) : IoGetCurrentProcess();
}

#endif /* EXF_NT31 */
