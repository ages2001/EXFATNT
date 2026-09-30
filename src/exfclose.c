/*
 * EXFATNT - IRP_MJ_CLEANUP, IRP_MJ_CLOSE, dismount and volume teardown
 *
 * Cleanup of the last handle is where a file is deleted, extra
 * allocation is given back and the entry set gets the final sizes and
 * times. A dismounted volume stays in memory until its last file object
 * is closed. Then the VPB is detached (so the next access mounts afresh)
 * and the volume device object is deleted.
 */

#include "exfat.h"

VOID
ExfUnlockVcb (
    PEXF_VCB Vcb
    )
{
    KIRQL Irql;

    Vcb->VcbState &= ~VCB_STATE_LOCKED;
    Vcb->LockFileObject = NULL;

    IoAcquireVpbSpinLock(&Irql);
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    IoReleaseVpbSpinLock(Irql);
}

/*
 * Drops cached data of files nobody has open, so that file objects held
 * only by the cache manager or memory manager get closed. Callers that
 * want the data kept flush first. The list is rescanned after each purge
 * because closes can arrive while it runs.
 */
VOID
ExfPurgeCachedFiles (
    PEXF_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Fcb;
    BOOLEAN Restart;

    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);

            if (Fcb->FcbState & FCB_STATE_VISITED) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;

            if (!ExfIsFcb(Fcb) ||
                Fcb->UncleanCount != 0 ||
                (Fcb->SectionObjectPointers.DataSectionObject == NULL &&
                 Fcb->SectionObjectPointers.ImageSectionObject == NULL)) {

                continue;
            }

            Fcb->RefCount++;

            (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
            (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);

            ExfDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }
}

/* Closes every internal stream, throwing away what is cached */
static VOID
ExfCloseAllStreams (
    PEXF_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Fcb;
    BOOLEAN Restart;

    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);

            if (Fcb->StreamFile != NULL) {
                ExfCloseStream(Vcb, Fcb, TRUE);
                Restart = TRUE;
                break;
            }
        }

    } while (Restart);

    if (Vcb->FatFcb != NULL) {
        ExfCloseStream(Vcb, Vcb->FatFcb, TRUE);
    }

    if (Vcb->BitmapFcb != NULL) {
        ExfCloseStream(Vcb, Vcb->BitmapFcb, TRUE);
    }
}

/*
 * Called with Vcb->Resource held exclusive. Callers that can still reach
 * the disk flush the volume first. Returns TRUE when the VCB can be
 * deleted right away; the caller then releases the resource and calls
 * ExfDeleteVcb.
 */
BOOLEAN
ExfDismountVcb (
    PEXF_VCB Vcb
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Fcb;

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FALSE;
    }

    EXF_DBG((EXF_PFX "Dismounting volume %08lX\n", Vcb->SerialNumber));

    Vcb->VcbState |= VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT;
    Vcb->VcbState &= ~VCB_STATE_MOUNTED;

    /* Send cached I/O on open files back through the IRP path */
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);
        Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    }

    ExfPurgeCachedFiles(Vcb);
    ExfCloseAllStreams(Vcb);

    Vcb->VcbState &= ~VCB_STATE_IN_DISMOUNT;

    return ExfTryTeardown(Vcb, 0);
}

/*
 * Called with Vcb->Resource held exclusive. InFlightReferences is the
 * VPB reference the current request itself holds (1 for a create, 0 for
 * a close).
 */
BOOLEAN
ExfTryTeardown (
    PEXF_VCB Vcb,
    ULONG InFlightReferences
    )
{
    PVPB Vpb = Vcb->Vpb;
    KIRQL Irql;
    BOOLEAN FreeVpb = FALSE;

    if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Vcb->VcbState & (VCB_STATE_IN_DISMOUNT | VCB_STATE_DELETE_PENDING)) ||
        Vcb->OpenCount != 0) {

        return FALSE;
    }

    IoAcquireVpbSpinLock(&Irql);

    if (Vpb->ReferenceCount != InFlightReferences) {
        IoReleaseVpbSpinLock(Irql);
        return FALSE;
    }

    if (Vpb->RealDevice->Vpb == Vpb) {

        Vpb->DeviceObject = NULL;
        Vpb->Flags &= ~(VPB_MOUNTED | VPB_LOCKED);

    } else if (InFlightReferences == 0) {

        /* The I/O manager replaced this VPB during a verify */
        FreeVpb = TRUE;
    }

    IoReleaseVpbSpinLock(Irql);

    Vcb->VcbState |= VCB_STATE_DELETE_PENDING;

    if (FreeVpb) {
        Vcb->VcbState |= VCB_STATE_FREE_VPB;
    }

    return TRUE;
}

/* Called without any locks held */
VOID
ExfDeleteVcb (
    PEXF_VCB Vcb
    )
{
    PEXF_FCB Fcb;
    PVPB FreeVpb = NULL;

    EXF_DBG((EXF_PFX "Deleting volume %08lX\n", Vcb->SerialNumber));

    (VOID)ExAcquireResourceExclusiveLite(&ExfData.Resource, TRUE);
    RemoveEntryList(&Vcb->VcbLinks);
    ExfRelease(&ExfData.Resource);

    while (!IsListEmpty(&Vcb->FcbList)) {
        Fcb = CONTAINING_RECORD(Vcb->FcbList.Flink, EXF_FCB, FcbLinks);
        ExfDeleteFcb(Fcb);
    }

    if (Vcb->VolumeFcb != NULL) {
        ExfDeleteFcb(Vcb->VolumeFcb);
    }

    if (Vcb->FatFcb != NULL) {
        ExfDeleteFcb(Vcb->FatFcb);
    }

    if (Vcb->BitmapFcb != NULL) {
        ExfDeleteFcb(Vcb->BitmapFcb);
    }

    if (Vcb->Upcase != NULL) {
        ExFreePool(Vcb->Upcase);
    }

    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);

    if (Vcb->VcbState & VCB_STATE_FREE_VPB) {
        FreeVpb = Vcb->Vpb;
    }

    ExDeleteResourceLite(&Vcb->AllocResource);
    ExDeleteResourceLite(&Vcb->Resource);

    ObDereferenceObject(Vcb->TargetDeviceObject);
    IoDeleteDevice(Vcb->VolumeDeviceObject);

    if (FreeVpb != NULL) {
        ExFreePool(FreeVpb);
    }
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

/* Last handle of a delete-pending file or directory: remove it */
static VOID
ExfDeleteOnCleanup (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    BOOLEAN Empty = TRUE;
    NTSTATUS Status;

    if (ExfIsDcb(Fcb)) {

        /* Entries may have come back since the delete was requested */
        (VOID)ExfIsDirectoryEmpty(Vcb, Fcb, &Empty);

        if (!Empty) {
            Fcb->FcbState &= ~FCB_STATE_DELETE_PENDING;
            return;
        }

    } else {

        /* Nothing cached may reach the clusters about to be freed */
        (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        Fcb->Header.FileSize.QuadPart = 0;
        Fcb->Header.ValidDataLength.QuadPart = 0;
        ExfRelease(&Fcb->PagingIoResource);

        (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
    }

    Status = ExfDeleteFromDisk(Vcb, Fcb);

    if (!NT_SUCCESS(Status)) {
        EXF_DBG((EXF_PFX "Delete of %wZ failed %lX\n", &Fcb->Name, Status));
    }

    ExfNotifyChange(Vcb, Fcb,
                    ExfIsDcb(Fcb) ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                    FILE_ACTION_REMOVED);
}

NTSTATUS
ExfCommonCleanup (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    PEXF_CCB Ccb = (PEXF_CCB)FileObject->FsContext2;
    PEXF_VCB Vcb = Ctx->Vcb;
    LARGE_INTEGER Now;
    LARGE_INTEGER Zero;
    PLARGE_INTEGER TruncateSize = NULL;
    ULONG Filter = 0;
    BOOLEAN FcbAcquired = FALSE;
    BOOLEAN Writable;
    BOOLEAN Modified;

    /* IoCreateStreamFileObject cleans up before FsContext is set */
    if (Fcb == NULL || Ccb == NULL) {
        return STATUS_SUCCESS;
    }

    Zero.QuadPart = 0;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Writable = (BOOLEAN)!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY |
                                               VCB_STATE_SHUTDOWN));

        if (ExfIsVfcb(Fcb)) {

            if (Vcb->LockFileObject == FileObject) {

                ExfUnlockVcb(Vcb);

                /* Metadata may have been rewritten under us */
                if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                    (VOID)ExfDismountVcb(Vcb);
                }
            }

        } else {

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;

            if (ExfIsDcb(Fcb)) {
                FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
            } else {
                FsRtlFastUnlockAll(&Fcb->FileLock, FileObject,
                                   IoGetRequestorProcess(Ctx->Irp), NULL);
            }

            if (Ccb->Flags & CCB_FLAG_DELETE_ON_CLOSE) {
                Fcb->FcbState |= FCB_STATE_DELETE_PENDING;
            }

            Modified = (BOOLEAN)((FileObject->Flags & FO_FILE_MODIFIED) != 0);

            if (Writable && !(Fcb->FcbState & FCB_STATE_DELETED)) {

                if (Modified) {

                    KeQuerySystemTime(&Now);

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_WRITE)) {
                        Fcb->LastWriteTime = Now;
                        Filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
                    }

                    if (!(Ccb->Flags & CCB_FLAG_USER_SET_ACCESS)) {
                        Fcb->LastAccessTime = Now;
                    }

                    if (!(Fcb->Attributes & EXFAT_ATTR_ARCHIVE) && ExfIsFcb(Fcb)) {
                        Fcb->Attributes |= EXFAT_ATTR_ARCHIVE;
                        Filter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
                    }

                    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
                }

                if (FileObject->Flags & FO_FILE_SIZE_CHANGED) {
                    Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;
                    Filter |= FILE_NOTIFY_CHANGE_SIZE;
                }

                if (Fcb->UncleanCount == 1 && (Fcb->FcbState & FCB_STATE_DELETE_PENDING)) {

                    ExfDeleteOnCleanup(Vcb, Fcb);

                    if (Fcb->FcbState & FCB_STATE_DELETED) {
                        TruncateSize = &Zero;
                        Filter = 0;
                    }

                } else if (Fcb->UncleanCount == 1 &&
                           (Fcb->FcbState & FCB_STATE_TRUNCATE_ON_CLOSE)) {

                    (VOID)ExfSetAllocation(Vcb, Fcb, (ULONGLONG)Fcb->Header.FileSize.QuadPart);
                    Fcb->FcbState &= ~FCB_STATE_TRUNCATE_ON_CLOSE;
                }

                if (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) {
                    (VOID)ExfUpdateDirent(Vcb, Fcb);
                }

                if (Filter != 0) {
                    ExfNotifyChange(Vcb, Fcb, Filter, FILE_ACTION_MODIFIED);
                }

                /* Removable media: this file is on the disk once closed */
                if ((Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE) && ExfIsFcb(Fcb) &&
                    (Modified || (FileObject->Flags & FO_FILE_SIZE_CHANGED)) &&
                    !(Fcb->FcbState & FCB_STATE_DELETED)) {

                    (VOID)ExfFlushFile(Vcb, Fcb);
                }
            }

            if (ExfIsFcb(Fcb)) {

                CcUninitializeCacheMap(FileObject, TruncateSize, NULL);

                /* On a dismounted volume let the last handle take the cache
                   with it, so the file objects close and the VCB can go */
                if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) && Fcb->UncleanCount == 1) {
                    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
                    (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
                }
            }

            if (FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) {
                Fcb->NonCachedUncleanCount--;
            }
        }

        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);

        Fcb->UncleanCount--;
        Vcb->UncleanCount--;

        if (Ccb->Flags & CCB_FLAG_WRITE_HANDLE) {

            Ccb->Flags &= ~CCB_FLAG_WRITE_HANDLE;
            Vcb->WriteCount--;

            /* Removable media is left clean once nothing writes to it */
            if (Vcb->WriteCount == 0 && Writable && (Vcb->VcbState & VCB_STATE_FLUSH_ON_CLOSE)) {
                (VOID)ExfFlushVolume(Vcb, TRUE);
            }
        }

        if (ExfIsFcb(Fcb)) {
            Fcb->Header.IsFastIoPossible = ExfIsFastIoPossible(Fcb);
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

    } __finally {

        if (FcbAcquired) {
            ExfRelease(&Fcb->Resource);
        }

        ExfRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Close                                                               */
/* ------------------------------------------------------------------ */

/* Called with Vcb->Resource exclusive; TRUE: the VCB can be deleted */
static BOOLEAN
ExfCloseFcb (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb,
    PEXF_CCB Ccb
    )
{
    if (Ccb == NULL) {

        /* An internal stream */
        if (ExfIsDcb(Fcb) || ExfIsMeta(Fcb)) {
            ExfStreamClosed(Vcb, Fcb);
        }

    } else {

        ExfDeleteCcb(Ccb);

        /* Mapped writes after the last cleanup can still move VDL */
        if (Fcb->RefCount == 1 && (Fcb->FcbState & FCB_STATE_DIRENT_DIRTY) &&
            !(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_READ_ONLY))) {

            (VOID)ExfUpdateDirent(Vcb, Fcb);
        }

        ExfDereferenceFcb(Vcb, Fcb);
        Vcb->OpenCount--;
    }

    return ExfTryTeardown(Vcb, 0);
}

static VOID
NTAPI
ExfCloseWorker (
    PVOID Context
    )
{
    PEXF_CLOSE_ITEM Item = (PEXF_CLOSE_ITEM)Context;
    PEXF_VCB Vcb = Item->Vcb;
    BOOLEAN Delete;

    FsRtlEnterFileSystem();

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Delete = ExfCloseFcb(Vcb, Item->Fcb, Item->Ccb);

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    if (Delete) {
        ExfDeleteVcb(Vcb);
    }

    FsRtlExitFileSystem();

    ExFreePool(Item);
}

/*
 * A close can arrive from inside the cache or memory manager while this
 * thread already holds the volume shared; waiting for it exclusive would
 * then deadlock. Such closes are finished by a worker thread.
 */
NTSTATUS
ExfCommonClose (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_FCB Fcb = (PEXF_FCB)FileObject->FsContext;
    PEXF_CCB Ccb = (PEXF_CCB)FileObject->FsContext2;
    PEXF_VCB Vcb = Ctx->Vcb;
    PEXF_CLOSE_ITEM Item;
    BOOLEAN Delete;

    if (Fcb == NULL) {
        return STATUS_SUCCESS;
    }

    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;

    if (!ExAcquireResourceExclusiveLite(&Vcb->Resource, FALSE)) {

        Item = NULL;

        if (IoGetTopLevelIrp() != Ctx->Irp) {
            Item = (PEXF_CLOSE_ITEM)ExAllocatePoolWithTag(NonPagedPool, sizeof(EXF_CLOSE_ITEM),
                                                          EXF_TAG_WORK);
        }

        if (Item != NULL) {

            Item->Vcb = Vcb;
            Item->Fcb = Fcb;
            Item->Ccb = Ccb;

            ExInitializeWorkItem(&Item->Item, ExfCloseWorker, Item);
            ExQueueWorkItem(&Item->Item, DelayedWorkQueue);

            return STATUS_SUCCESS;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
    }

    __try {

        Delete = ExfCloseFcb(Vcb, Fcb, Ccb);

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    if (Delete) {
        ExfDeleteVcb(Vcb);
    }

    return STATUS_SUCCESS;
}
