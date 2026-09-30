/*
 * EXFATNT - IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN and the VolumeDirty flag
 *
 * VolumeDirty is set in the main boot sector, written straight to the
 * disk, before the first change to the volume, and cleared again once
 * everything cached has been written: on a volume flush, lock, dismount
 * or shutdown, and on removable media when its last writer closes. A
 * volume that was dirty when mounted stays dirty for chkdsk.
 */

#include "exfat.h"

#define EXF_BOOT_VOLUME_FLAGS       106
#define EXF_BOOT_PERCENT_IN_USE     112

/* ------------------------------------------------------------------ */
/* VolumeDirty                                                         */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExfWriteVolumeFlags (
    PEXF_VCB Vcb,
    USHORT Flags,
    BOOLEAN SetPercent
    )
{
    PUCHAR Sector;
    ULONG Used;
    NTSTATUS Status;

    Sector = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Vcb->SectorSize, EXF_TAG_BUFFER);
    if (Sector == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ExfReadSectors(Vcb->TargetDeviceObject, 0, Vcb->SectorSize, Sector, FALSE);

    if (NT_SUCCESS(Status) &&
        ExfCheckBootSector((PEXFAT_BOOT_SECTOR)Sector, Vcb->SectorSize, 0) == EXF_BOOT_OK &&
        ((PEXFAT_BOOT_SECTOR)Sector)->VolumeSerialNumber == Vcb->SerialNumber) {

        /* Neither field is covered by the boot region checksum */
        *(USHORT UNALIGNED *)(Sector + EXF_BOOT_VOLUME_FLAGS) = Flags;

        if (SetPercent && Vcb->ClusterCount != 0) {
            Used = Vcb->ClusterCount - Vcb->FreeClusters;
            Sector[EXF_BOOT_PERCENT_IN_USE] = (UCHAR)((ULONGLONG)Used * 100 / Vcb->ClusterCount);
        }

        Status = ExfWriteSectors(Vcb->TargetDeviceObject, 0, Vcb->SectorSize, Sector, FALSE);

        if (NT_SUCCESS(Status)) {
            Vcb->VolumeFlags = Flags;
        }

    } else if (NT_SUCCESS(Status)) {

        Status = STATUS_DISK_CORRUPT_ERROR;
    }

    ExFreePool(Sector);

    if (!NT_SUCCESS(Status)) {
        EXF_DBG((EXF_PFX "Boot sector update failed %lX\n", Status));
    }

    return Status;
}

/* Before the first change: callers hold Vcb->Resource */
VOID
ExfMarkVolumeDirty (
    PEXF_VCB Vcb
    )
{
    if (Vcb->DirtyMarked) {
        return;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    if (!Vcb->DirtyMarked) {

        if (!(Vcb->VolumeFlags & EXFAT_VOLUME_DIRTY)) {
            (VOID)ExfWriteVolumeFlags(Vcb, (USHORT)(Vcb->VolumeFlags | EXFAT_VOLUME_DIRTY), FALSE);
        }

        Vcb->DirtyMarked = TRUE;
    }

    ExfRelease(&Vcb->AllocResource);
}

/* Once everything is on the disk: called with Vcb->Resource exclusive */
NTSTATUS
ExfMarkVolumeClean (
    PEXF_VCB Vcb
    )
{
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    if (Vcb->DirtyMarked && !(Vcb->VcbState & VCB_STATE_KEEP_DIRTY)) {

        Status = ExfWriteVolumeFlags(Vcb, (USHORT)(Vcb->VolumeFlags & ~EXFAT_VOLUME_DIRTY), TRUE);

        if (NT_SUCCESS(Status)) {
            Vcb->DirtyMarked = FALSE;
        }
    }

    ExfRelease(&Vcb->AllocResource);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Flushing                                                            */
/* ------------------------------------------------------------------ */

/* Data and entry set of one file; Vcb->Resource exclusive */
NTSTATUS
ExfFlushFile (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
            Status = Iosb.Status;

            /* Let a lazy write in progress finish */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            ExfRelease(&Fcb->PagingIoResource);
        }

        if ((Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) && !(Fcb->FcbState & FCB_STATE_DELETED)) {

            if (NT_SUCCESS(Status)) {
                Status = ExfUpdateDirent(Vcb, Fcb);
            } else {
                (VOID)ExfUpdateDirent(Vcb, Fcb);
            }
        }

    } __finally {

        ExfRelease(&Fcb->Resource);
    }

    return Status;
}

static NTSTATUS
ExfFlushStream (
    PEXF_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;

    if (Fcb == NULL || Fcb->StreamFile == NULL) {
        return STATUS_SUCCESS;
    }

    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);

    return Iosb.Status;
}

/* Directories, FAT and bitmap; Vcb->Resource exclusive */
NTSTATUS
ExfFlushMetadata (
    PEXF_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Fcb;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);

        if (ExfIsDcb(Fcb)) {
            Result = ExfFlushStream(Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }
        }
    }

    Result = ExfFlushStream(Vcb->FatFcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    Result = ExfFlushStream(Vcb->BitmapFcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    return Status;
}

/*
 * Writes everything cached for the volume, and with MarkClean clears
 * VolumeDirty afterwards. Vcb->Resource exclusive.
 */
NTSTATUS
ExfFlushVolume (
    PEXF_VCB Vcb,
    BOOLEAN MarkClean
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Fcb;
    BOOLEAN Restart;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    /* A flush can close file objects, so the list is walked with markers */
    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);

            if ((Fcb->FcbState & FCB_STATE_VISITED) || !ExfIsFcb(Fcb)) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;
            Fcb->RefCount++;

            Result = ExfFlushFile(Vcb, Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }

            ExfDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }

    Result = ExfFlushMetadata(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    if (MarkClean && NT_SUCCESS(Status)) {
        Status = ExfMarkVolumeClean(Vcb);
    }

    (VOID)ExfFlushDevice(Vcb);

    return Status;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
ExfCommonFlushBuffers (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    PEXF_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExfVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status) || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
            __leave;
        }

        if (ExfIsVfcb(Fcb)) {

            Status = ExfFlushVolume(Vcb, TRUE);

        } else {

            if (ExfIsFcb(Fcb)) {
                Status = ExfFlushFile(Vcb, Fcb);
            }

            if (NT_SUCCESS(Status)) {
                Status = ExfFlushMetadata(Vcb);
            } else {
                (VOID)ExfFlushMetadata(Vcb);
            }

            (VOID)ExfFlushDevice(Vcb);
        }

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    return Status;
}

/* Sent to the file system device: leave every volume clean */
NTSTATUS
ExfCommonShutdown (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PLIST_ENTRY Entry;
    PEXF_VCB Vcb;

    UNREFERENCED_PARAMETER(Ctx);

    (VOID)ExAcquireResourceSharedLite(&ExfData.Resource, TRUE);

    __try {

        for (Entry = ExfData.VcbList.Flink; Entry != &ExfData.VcbList; Entry = Entry->Flink) {

            Vcb = CONTAINING_RECORD(Entry, EXF_VCB, VcbLinks);

            (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

            if ((Vcb->VcbState & VCB_STATE_MOUNTED) && !(Vcb->VcbState & VCB_STATE_SHUTDOWN)) {
                (VOID)ExfFlushVolume(Vcb, TRUE);
                Vcb->VcbState |= VCB_STATE_SHUTDOWN;
            }

            ExfRelease(&Vcb->Resource);
        }

    } __finally {

        ExfRelease(&ExfData.Resource);
    }

    return STATUS_SUCCESS;
}
