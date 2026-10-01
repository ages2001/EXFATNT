/*
 * EXFATNT - fast I/O and cache manager callbacks
 */

#include "exfat.h"

UCHAR
ExfIsFastIoPossible (
    PEXF_FCB Fcb
    )
{
    if (Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FastIoIsNotPossible;
    }

    if (FsRtlAreThereCurrentFileLocks(&Fcb->FileLock)) {
        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
}

static BOOLEAN
ExfFastIoUsable (
    PEXF_FCB Fcb
    )
{
    if (Fcb == NULL || (!ExfIsFcb(Fcb) && !ExfIsDcb(Fcb))) {
        return FALSE;
    }

    if ((Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Fcb->Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {

        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NTAPI
ExfFastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    LARGE_INTEGER LargeLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExfFastIoUsable(Fcb) || !ExfIsFcb(Fcb)) {
        return FALSE;
    }

    LargeLength.QuadPart = Length;

    if (CheckForReadOperation) {
        return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                         FileObject, IoGetCurrentProcess());
    }

    if (Fcb->Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return FALSE;
    }

    return FsRtlFastCheckLockForWrite(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                      FileObject, IoGetCurrentProcess());
}

static BOOLEAN
NTAPI
ExfFastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    FILE_BASIC_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExfFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    ExfFillBasicInfo(Fcb, &Info);

    ExfRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
ExfFastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    FILE_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExfFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    ExfFillStandardInfo(Fcb, &Info);

    ExfRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);

    return TRUE;
}

#ifndef EXF_NT31     /* no such fast I/O routine on NT 3.1 */

static BOOLEAN
NTAPI
ExfFastQueryNetworkOpenInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_NETWORK_OPEN_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    FILE_NETWORK_OPEN_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!ExfFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    ExfFillNetworkOpenInfo(Fcb, &Info);

    ExfRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    return TRUE;
}

#endif

#ifdef EXF_NT31

/* NT 3.1 calls fast I/O routines without the DeviceObject argument */

static BOOLEAN
NTAPI
Exf31FastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus
    )
{
    return ExfFastIoCheckIfPossible(FileObject, FileOffset, Length, Wait, LockKey,
                                    CheckForReadOperation, IoStatus, NULL);
}

static BOOLEAN
NTAPI
Exf31FastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus
    )
{
    return ExfFastQueryBasicInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}

static BOOLEAN
NTAPI
Exf31FastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus
    )
{
    return ExfFastQueryStandardInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}

VOID
ExfInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    /* The NT 3.1 table: the size and ten routines, up to FastIoDeviceControl */
    FastIo->SizeOfFastIoDispatch = sizeof(ULONG) + 10 * sizeof(PVOID);
    FastIo->FastIoCheckIfPossible = (PFAST_IO_CHECK_IF_POSSIBLE)Exf31FastIoCheckIfPossible;
    FastIo->FastIoRead = (PFAST_IO_READ)EXF31_COPY_READ;    /* NT 3.1's own takes 7 arguments too */
    FastIo->FastIoWrite = (PFAST_IO_WRITE)EXF31_COPY_WRITE;
    FastIo->FastIoQueryBasicInfo = (PFAST_IO_QUERY_BASIC_INFO)Exf31FastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = (PFAST_IO_QUERY_STANDARD_INFO)Exf31FastQueryStandardInfo;
}

#else

VOID
ExfInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    FastIo->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible = ExfFastIoCheckIfPossible;
    FastIo->FastIoRead = FsRtlCopyRead;
    FastIo->FastIoWrite = FsRtlCopyWrite;
    FastIo->FastIoQueryBasicInfo = ExfFastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = ExfFastQueryStandardInfo;
    FastIo->FastIoQueryNetworkOpenInfo = ExfFastQueryNetworkOpenInfo;
}

#endif

/* ------------------------------------------------------------------ */
/* Cache manager callbacks                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
NTAPI
ExfAcquireForLazyWrite (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    /* Paging writes from this thread must not move VDL */
    Fcb->LazyWriteThread = KeGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
ExfReleaseFromLazyWrite (
    PVOID Context
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread = NULL;

    ExfRelease(&Fcb->PagingIoResource);
    FsRtlExitFileSystem();
}

static BOOLEAN
NTAPI
ExfAcquireForReadAhead (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
ExfReleaseFromReadAhead (
    PVOID Context
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    ExfRelease(&Fcb->Resource);
    FsRtlExitFileSystem();
}

/* Metadata streams need no synchronization with the cache manager */
static BOOLEAN
NTAPI
ExfNoOpAcquire (
    PVOID Context,
    BOOLEAN Wait
    )
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Wait);

    return TRUE;
}

static VOID
NTAPI
ExfNoOpRelease (
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

VOID
ExfInitializeCacheCallbacks (
    VOID
    )
{
    ExfData.CacheManagerCallbacks.AcquireForLazyWrite = ExfAcquireForLazyWrite;
    ExfData.CacheManagerCallbacks.ReleaseFromLazyWrite = ExfReleaseFromLazyWrite;
    ExfData.CacheManagerCallbacks.AcquireForReadAhead = ExfAcquireForReadAhead;
    ExfData.CacheManagerCallbacks.ReleaseFromReadAhead = ExfReleaseFromReadAhead;

    ExfData.MetaCacheCallbacks.AcquireForLazyWrite = ExfNoOpAcquire;
    ExfData.MetaCacheCallbacks.ReleaseFromLazyWrite = ExfNoOpRelease;
    ExfData.MetaCacheCallbacks.AcquireForReadAhead = ExfNoOpAcquire;
    ExfData.MetaCacheCallbacks.ReleaseFromReadAhead = ExfNoOpRelease;
}
