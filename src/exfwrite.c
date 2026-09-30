/*
 * EXFATNT - IRP_MJ_WRITE
 *
 * ValidDataLength (VDL) is kept as exFAT keeps it on the disk: the file
 * reads as zeros from VDL to FileSize, so extending a file costs nothing
 * until data is written past VDL. Then the gap is zeroed first, through
 * the cache for cached writes and on the disk otherwise. The entry set
 * gets the new sizes when the handle is cleaned up or the file flushed.
 *
 * Only paging writes that the lazy writer did not issue may move VDL
 * forward (mapped files, flushes we start ourselves); whoever issues them
 * holds the file exclusive.
 */

#include "exfat.h"

#ifndef FSRTL_FLAG_USER_MAPPED_FILE
#define FSRTL_FLAG_USER_MAPPED_FILE     0x20
#endif

/*
 * Zeroes [Start, End) of a file whose data up to Start is valid. Through
 * the cache when this file object caches the file, else on the disk.
 */
/* Writes Length zero bytes at Offset through the cache */
static NTSTATUS
ExfCopyZeros (
    PFILE_OBJECT FileObject,
    LONGLONG Offset,
    ULONG Length
    )
{
    LARGE_INTEGER From;
    PVOID Zero;

    Zero = ExAllocatePoolWithTag(PagedPool, Length, EXF_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Zero, Length);
    From.QuadPart = Offset;

    __try {

        (VOID)CcCopyWrite(FileObject, &From, Length, TRUE, Zero);

    } __finally {

        ExFreePool(Zero);
    }

    return STATUS_SUCCESS;
}

/*
 * Zeroes [Start, End) of a file whose data up to Start is valid; End is
 * within FileSize. Through the cache when this file object caches the
 * file (CcZeroData for the whole sectors, copies for the ends), else on
 * the disk.
 */
NTSTATUS
ExfZeroFileRange (
    PEXF_FCB Fcb,
    PFILE_OBJECT FileObject,
    LONGLONG Start,
    LONGLONG End
    )
{
    PEXF_VCB Vcb = Fcb->Vcb;
    LARGE_INTEGER From;
    LARGE_INTEGER To;
    LONGLONG HeadEnd;
    LONGLONG TailStart;
    NTSTATUS Status;

    if (Start >= End) {
        return STATUS_SUCCESS;
    }

    if (FileObject == NULL || FileObject->PrivateCacheMap == NULL) {
        return ExfZeroDisk(Vcb, Fcb, Start, End);
    }

    HeadEnd = ExfRoundUp(Start, Vcb->SectorSize);
    TailStart = End & ~(LONGLONG)(Vcb->SectorSize - 1);

    if (HeadEnd >= TailStart) {
        /* Within one or two sectors: just copy zeros */
        return ExfCopyZeros(FileObject, Start, (ULONG)(End - Start));
    }

    if (Start < HeadEnd) {
        Status = ExfCopyZeros(FileObject, Start, (ULONG)(HeadEnd - Start));
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    From.QuadPart = HeadEnd;
    To.QuadPart = TailStart;

    (VOID)CcZeroData(FileObject, &From, &To, TRUE);

    if (TailStart < End) {
        return ExfCopyZeros(FileObject, TailStart, (ULONG)(End - TailStart));
    }

    return STATUS_SUCCESS;
}

/* DASD writes, only through a handle that locked or dismounted the volume */
static NTSTATUS
ExfWriteVolume (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_FCB Vfcb,
    PEXF_CCB Ccb,
    LONGLONG StartingByte,
    ULONG ByteCount
    )
{
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_VCB Vcb = Vfcb->Vcb;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Ccb == NULL || (Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject && !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (((ULONG)StartingByte & (Vcb->SectorSize - 1)) != 0 ||
            (ByteCount & (Vcb->SectorSize - 1)) != 0) {

            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (StartingByte >= Vcb->PartitionBytes) {
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        if (StartingByte + ByteCount > Vcb->PartitionBytes) {
            ByteCount = (ULONG)(Vcb->PartitionBytes - StartingByte);
        }

        Vcb->VcbState |= VCB_STATE_DASD_WRITTEN;

        ExfLockUserBuffer(Irp, IoReadAccess, ByteCount);

        Status = ExfNonCachedIo(Ctx, Vfcb, IRP_MJ_WRITE, StartingByte, ByteCount, Vcb->PartitionBytes);

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
                FileObject->CurrentByteOffset.QuadPart = StartingByte + ByteCount;
            }
        }

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    return Status;
}

/* The cache manager writing back a directory, the FAT or the bitmap */
static NTSTATUS
ExfWriteStream (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_FCB Fcb,
    LONGLONG StartingVbo,
    ULONG ByteCount
    )
{
    PIRP Irp = Ctx->Irp;
    LONGLONG FileSize;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!(Irp->Flags & IRP_PAGING_IO)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);

    __try {

        FileSize = Fcb->Header.FileSize.QuadPart;

        if (StartingVbo >= FileSize) {
            __leave;
        }

        if (StartingVbo + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingVbo);
        }

        Status = ExfNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingVbo,
                                (ULONG)ExfRoundUp(ByteCount, Fcb->Vcb->SectorSize), 0);

        if (NT_SUCCESS(Status)) {
            Irp->IoStatus.Information = ByteCount;
        }

    } __finally {

        ExfRelease(&Fcb->PagingIoResource);
    }

    return Status;
}

/* Paging writes of a file: never past FileSize */
static NTSTATUS
ExfPagingWrite (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_FCB Fcb,
    LONGLONG StartingVbo,
    ULONG ByteCount
    )
{
    PEXF_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    LONGLONG FileSize;
    LONGLONG ValidData;
    LONGLONG End;
    BOOLEAN ExtendValidData = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);

    __try {

        FileSize = Fcb->Header.FileSize.QuadPart;
        ValidData = Fcb->Header.ValidDataLength.QuadPart;

        if (StartingVbo >= FileSize) {
            __leave;
        }

        if (StartingVbo + ByteCount > FileSize) {
            ByteCount = (ULONG)(FileSize - StartingVbo);
        }

        End = StartingVbo + ByteCount;

        if (End > ValidData) {

            if (Fcb->LazyWriteThread == KeGetCurrentThread()) {

                /* A mapped page past VDL waits for the mapped page writer,
                   which may move VDL */
                if ((Fcb->Header.Flags & FSRTL_FLAG_USER_MAPPED_FILE) &&
                    End > ExfRoundUp(ValidData, EXF_PAGE_SIZE)) {

                    Status = STATUS_FILE_LOCK_CONFLICT;
                    __leave;
                }

            } else {

                ExtendValidData = TRUE;

                if (StartingVbo > ValidData) {
                    Status = ExfZeroDisk(Vcb, Fcb, ValidData, StartingVbo);
                    if (!NT_SUCCESS(Status)) {
                        __leave;
                    }
                }
            }
        }

        Status = ExfNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingVbo,
                                (ULONG)ExfRoundUp(ByteCount, Vcb->SectorSize), 0);

        if (NT_SUCCESS(Status)) {

            Irp->IoStatus.Information = ByteCount;

            if (ExtendValidData && End > Fcb->Header.ValidDataLength.QuadPart) {
                Fcb->Header.ValidDataLength.QuadPart = End;
                Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
            }
        }

    } __finally {

        ExfRelease(&Fcb->PagingIoResource);
    }

    return Status;
}

static NTSTATUS
ExfWriteFile (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_FCB Fcb,
    LARGE_INTEGER StartingByte,
    ULONG ByteCount
    )
{
    PEXF_VCB Vcb = Fcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    BOOLEAN NonCached = (BOOLEAN)((Irp->Flags & IRP_NOCACHE) != 0);
    BOOLEAN WriteToEof;
    BOOLEAN VcbAcquired = FALSE;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN PagingAcquired = FALSE;
    BOOLEAN Extended = FALSE;
    LONGLONG OldFileSize = 0;
    LONGLONG OldValidData = 0;
    LONGLONG End;
    IO_STATUS_BLOCK Iosb;
    PVOID Buffer;
    NTSTATUS Status;

    WriteToEof = (BOOLEAN)(StartingByte.LowPart == FILE_WRITE_TO_END_OF_FILE &&
                           StartingByte.HighPart == -1);

    Status = ExfVerifyWritable(Ctx, Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (NonCached &&
        ((!WriteToEof && ((ULONG)StartingByte.QuadPart & (Vcb->SectorSize - 1)) != 0) ||
         (ByteCount & (Vcb->SectorSize - 1)) != 0)) {

        return STATUS_INVALID_PARAMETER;
    }

    /* Throttle before taking anything the lazy writer may need */
    if (!NonCached) {
        (VOID)CcCanIWrite(FileObject, ByteCount, TRUE, FALSE);
    }

    __try {

        (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
        VcbAcquired = TRUE;

        (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
        FcbAcquired = TRUE;

        Status = ExfVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Fcb->FcbState & FCB_STATE_DELETED) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (WriteToEof) {
            StartingByte = Fcb->Header.FileSize;
        }

        if (NonCached && ((ULONG)StartingByte.QuadPart & (Vcb->SectorSize - 1)) != 0) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* Keep the cache coherent with what goes straight to the disk */
        if (NonCached && Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            PagingAcquired = TRUE;

            CcFlushCache(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, &Iosb);

            if (!NT_SUCCESS(Iosb.Status)) {
                Status = Iosb.Status;
                __leave;
            }

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, &StartingByte, ByteCount, FALSE);
        }

        if (!FsRtlCheckLockForWriteAccess(&Fcb->FileLock, Irp)) {
            Status = STATUS_FILE_LOCK_CONFLICT;
            __leave;
        }

        End = StartingByte.QuadPart + ByteCount;

        if (StartingByte.QuadPart < 0 || End < StartingByte.QuadPart) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        OldFileSize = Fcb->Header.FileSize.QuadPart;
        OldValidData = Fcb->Header.ValidDataLength.QuadPart;

        if (End > OldFileSize) {

            if (End > Fcb->Header.AllocationSize.QuadPart) {

                Status = ExfSetAllocation(Vcb, Fcb, (ULONGLONG)End);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            Fcb->Header.FileSize.QuadPart = End;
            Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
            Extended = TRUE;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        if (!NonCached) {

            if (FileObject->PrivateCacheMap == NULL) {

                CcInitializeCacheMap(FileObject,
                                     (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                                     FALSE,
                                     &ExfData.CacheManagerCallbacks,
                                     Fcb);
            }

            if (StartingByte.QuadPart > OldValidData) {
                Status = ExfZeroFileRange(Fcb, FileObject, OldValidData, StartingByte.QuadPart);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            if (IrpSp->MinorFunction & IRP_MN_MDL) {

                CcPrepareMdlWrite(FileObject, &StartingByte, ByteCount, &Irp->MdlAddress,
                                  &Irp->IoStatus);
                Status = Irp->IoStatus.Status;

            } else {

                Buffer = ExfMapUserBuffer(Irp);

                if (!CcCopyWrite(FileObject, &StartingByte, ByteCount, TRUE, Buffer)) {
                    Status = STATUS_UNSUCCESSFUL;
                    __leave;
                }
            }

        } else {

            if (StartingByte.QuadPart > OldValidData) {
                Status = ExfZeroDisk(Vcb, Fcb, OldValidData, StartingByte.QuadPart);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }

            ExfLockUserBuffer(Irp, IoReadAccess, ByteCount);

            Status = ExfNonCachedIo(Ctx, Fcb, IRP_MJ_WRITE, StartingByte.QuadPart, ByteCount, 0);
        }

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (End > Fcb->Header.ValidDataLength.QuadPart) {

            Fcb->Header.ValidDataLength.QuadPart = End;
            Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

            /* Tell the cache the disk now holds valid data up to here */
            if (NonCached && Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        Irp->IoStatus.Information = ByteCount;

        if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
            FileObject->CurrentByteOffset.QuadPart = End;
        }

        FileObject->Flags |= FO_FILE_MODIFIED;

        if (Extended) {
            FileObject->Flags |= FO_FILE_SIZE_CHANGED;
        }

    } __finally {

        if (Extended && (AbnormalTermination() || !NT_SUCCESS(Status))) {

            /* Pull the sizes back; the allocation goes at cleanup */
            if (!PagingAcquired) {
                (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                PagingAcquired = TRUE;
            }

            Fcb->Header.FileSize.QuadPart = OldFileSize;
            Fcb->Header.ValidDataLength.QuadPart = OldValidData;
            Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;

            if (Fcb->SectionObjectPointers.SharedCacheMap != NULL) {
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
            }
        }

        if (PagingAcquired) {
            ExfRelease(&Fcb->PagingIoResource);
        }

        if (FcbAcquired) {
            ExfRelease(&Fcb->Resource);
        }

        if (VcbAcquired) {
            ExfRelease(&Vcb->Resource);
        }
    }

    return Status;
}

NTSTATUS
ExfCommonWrite (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    PEXF_CCB Ccb = (PEXF_CCB)FileObject->FsContext2;
    ULONG ByteCount = IrpSp->Parameters.Write.Length;

    if (Fcb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
        CcMdlWriteComplete(FileObject, &IrpSp->Parameters.Write.ByteOffset, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        return STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = 0;

    if (ByteCount == 0) {
        return STATUS_SUCCESS;
    }

    if (ExfIsVfcb(Fcb)) {
        return ExfWriteVolume(Ctx, Fcb, Ccb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    if (Ccb == NULL && (ExfIsDcb(Fcb) || ExfIsMeta(Fcb))) {
        return ExfWriteStream(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    if (!ExfIsFcb(Fcb)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (Irp->Flags & IRP_PAGING_IO) {
        return ExfPagingWrite(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset.QuadPart, ByteCount);
    }

    return ExfWriteFile(Ctx, Fcb, IrpSp->Parameters.Write.ByteOffset, ByteCount);
}
