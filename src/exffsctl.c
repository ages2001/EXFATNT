/*
 * EXFATNT - IRP_MJ_FILE_SYSTEM_CONTROL (mount, verify, lock/unlock,
 * dismount, queries) and IRP_MJ_PNP
 */

#include "exfat.h"

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static BOOLEAN
ExfBootRegionValid (
    PUCHAR Region,
    ULONG SectorSize
    )
{
    PULONG Checksums = (PULONG)(Region + EXFAT_BOOT_CHECKSUM_SECTOR * SectorSize);
    ULONG Checksum;
    ULONG i;

    Checksum = ExfBootChecksum(Region, SectorSize);

    for (i = 0; i < SectorSize / sizeof(ULONG); i++) {
        if (Checksums[i] != Checksum) {
            return FALSE;
        }
    }

    return TRUE;
}

/* Allocation bitmap, up-case table and label from the root directory */
static NTSTATUS
ExfLoadRootMetadata (
    PEXF_VCB Vcb
    )
{
    PEXF_FCB Root = Vcb->RootDcb;
    PEXF_FCB UpcaseFcb = NULL;
    EXF_MAP Map;
    PUCHAR Entry;
    PEXFAT_BITMAP_ENTRY Bitmap;
    PEXFAT_UPCASE_ENTRY Upcase;
    PEXFAT_LABEL_ENTRY Label;
    ULONG Offset;
    ULONG Size = Root->Header.AllocationSize.LowPart;
    ULONG ActiveBitmap;
    BOOLEAN HaveBitmap = FALSE;
    BOOLEAN HaveUpcase = FALSE;
    ULONG UpcaseCluster = 0;
    ULONG UpcaseLength = 0;
    ULONG UpcaseChecksum = 0;
    ULONG Chunk;
    PUCHAR Raw = NULL;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Map, sizeof(Map));

    ActiveBitmap = (Vcb->NumberOfFats == 2 && (Vcb->VolumeFlags & EXFAT_VOLUME_ACTIVE_FAT)) ? 1 : 0;

    __try {

        for (Offset = 0; Offset + EXFAT_DIRENT_SIZE <= Size; Offset += EXFAT_DIRENT_SIZE) {

            Entry = ExfMapStream(Vcb, Root, &Map, Offset, EXFAT_DIRENT_SIZE);

            if (Entry[0] == EXFAT_ENTRY_EOD) {
                break;
            }

            switch (Entry[0]) {

            case EXFAT_ENTRY_BITMAP:
                Bitmap = (PEXFAT_BITMAP_ENTRY)Entry;
                if (!HaveBitmap && (ULONG)(Bitmap->BitmapFlags & 1) == ActiveBitmap) {
                    Vcb->BitmapCluster = Bitmap->FirstCluster;
                    Vcb->BitmapLength = Bitmap->DataLength;
                    HaveBitmap = TRUE;
                }
                break;

            case EXFAT_ENTRY_UPCASE:
                Upcase = (PEXFAT_UPCASE_ENTRY)Entry;
                if (!HaveUpcase && Upcase->DataLength != 0 &&
                    Upcase->DataLength <= EXFAT_UPCASE_MAX_BYTES &&
                    (Upcase->DataLength & 1) == 0) {

                    UpcaseCluster = Upcase->FirstCluster;
                    UpcaseLength = (ULONG)Upcase->DataLength;
                    UpcaseChecksum = Upcase->TableChecksum;
                    HaveUpcase = TRUE;
                }
                break;

            case EXFAT_ENTRY_LABEL:
                Label = (PEXFAT_LABEL_ENTRY)Entry;
                Vcb->LabelLength = (Label->CharacterCount <= EXFAT_MAX_LABEL) ?
                                   Label->CharacterCount : EXFAT_MAX_LABEL;
                for (i = 0; i < Vcb->LabelLength; i++) {
                    Vcb->Label[i] = Label->VolumeLabel[i];
                }
                break;
            }
        }

        ExfUnmap(&Map);

        if (!HaveBitmap || !HaveUpcase) {
            EXF_DBG((EXF_PFX "Root directory lacks bitmap or up-case table\n"));
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }

        /* The up-case table is read once, through a stream of its own */
        UpcaseFcb = ExfCreateMetaFcb(Vcb, 0, UpcaseCluster, UpcaseLength);

        Raw = (PUCHAR)ExAllocatePoolWithTag(PagedPool, UpcaseLength, EXF_TAG_BUFFER);
        Vcb->Upcase = (PUSHORT)ExAllocatePoolWithTag(PagedPool, EXFAT_UPCASE_CHARS * sizeof(USHORT),
                                                     EXF_TAG_UPCASE);

        if (UpcaseFcb == NULL || Raw == NULL || Vcb->Upcase == NULL) {
            Status = (UpcaseFcb == NULL) ? STATUS_DISK_CORRUPT_ERROR : STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        for (Offset = 0; Offset < UpcaseLength; Offset += Chunk) {

            Chunk = EXF_MAP_UNIT - (Offset & (EXF_MAP_UNIT - 1));
            if (Chunk > UpcaseLength - Offset) {
                Chunk = UpcaseLength - Offset;
            }

            RtlCopyMemory(Raw + Offset, ExfMapStream(Vcb, UpcaseFcb, &Map, Offset, Chunk), Chunk);
        }

        ExfUnmap(&Map);

        if (ExfTableChecksum(Raw, UpcaseLength) != UpcaseChecksum) {
            EXF_DBG((EXF_PFX "Up-case table checksum mismatch\n"));
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }

        ExfExpandUpcase((const USHORT *)Raw, UpcaseLength / sizeof(USHORT), Vcb->Upcase);

    } __finally {

        ExfUnmap(&Map);

        if (UpcaseFcb != NULL) {
            ExfDeleteFcb(UpcaseFcb);
        }

        if (Raw != NULL) {
            ExFreePool(Raw);
        }
    }

    return Status;
}

/* Hot-plug devices (W2K) get the removable-media flush policy */
#define EXF_IOCTL_GET_HOTPLUG_INFO  CTL_CODE(0x0000002d, 0x0305, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _EXF_HOTPLUG_INFO {
    ULONG       Size;
    BOOLEAN     MediaRemovable;
    BOOLEAN     MediaHotplug;
    BOOLEAN     DeviceHotplug;
    BOOLEAN     WriteCacheEnableOverride;
} EXF_HOTPLUG_INFO;

static VOID
ExfCheckDeviceState (
    PEXF_VCB Vcb,
    PDEVICE_OBJECT TargetDevice
    )
{
    EXF_HOTPLUG_INFO Hotplug;
    NTSTATUS Status;

    Status = ExfDeviceIoctl(TargetDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, TRUE, NULL);

    /* Write-protected media, or writing turned off in the registry */
    if (Status == STATUS_MEDIA_WRITE_PROTECTED || !ExfWriteSupportEnabled()) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    if (Vcb->VcbState & VCB_STATE_REMOVABLE) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }

    RtlZeroMemory(&Hotplug, sizeof(Hotplug));

    Status = ExfDeviceIoctl(TargetDevice, EXF_IOCTL_GET_HOTPLUG_INFO, NULL, 0,
                            &Hotplug, sizeof(Hotplug), TRUE, NULL);

    if (NT_SUCCESS(Status) && (Hotplug.MediaRemovable || Hotplug.DeviceHotplug)) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }
}

static NTSTATUS
ExfMountVolume (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT RealDevice = Vpb->RealDevice;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PEXF_VCB Vcb = NULL;
    PEXFAT_BOOT_SECTOR Boot;
    PUCHAR Region = NULL;
    ULONG SectorSize;
    ULONG RegionSize;
    ULONG Check;
    ULONG i;
    LONGLONG PartitionBytes;
    BOOLEAN ClearedVerify = FALSE;
    NTSTATUS Status;

    Status = ExfDeviceIoctl(TargetDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                            &Geometry, sizeof(Geometry), TRUE, NULL);
    if (!NT_SUCCESS(Status)) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    SectorSize = Geometry.BytesPerSector;

    if (SectorSize < 512 || SectorSize > 4096 || (SectorSize & (SectorSize - 1)) != 0) {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Status = ExfDeviceIoctl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                            &Partition, sizeof(Partition), TRUE, NULL);

    /* Unknown (0) when the device has no partition information */
    PartitionBytes = NT_SUCCESS(Status) ? Partition.PartitionLength.QuadPart : 0;

    RegionSize = EXFAT_BOOT_REGION_SECTORS * SectorSize;

    Region = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, RegionSize * 2, EXF_TAG_BUFFER);
    if (Region == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        Status = ExfReadSectors(TargetDevice, 0, RegionSize * 2, Region, TRUE);
        if (!NT_SUCCESS(Status)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Boot = (PEXFAT_BOOT_SECTOR)Region;

        Check = ExfCheckBootSector(Boot, SectorSize, (ULONGLONG)PartitionBytes / SectorSize);

        if (Check == EXF_BOOT_OK && !ExfBootRegionValid(Region, SectorSize)) {

            /* Main boot region damaged: try the backup */
            Boot = (PEXFAT_BOOT_SECTOR)(Region + RegionSize);
            Check = ExfCheckBootSector(Boot, SectorSize, (ULONGLONG)PartitionBytes / SectorSize);

            if (Check == EXF_BOOT_OK && !ExfBootRegionValid(Region + RegionSize, SectorSize)) {
                Check = EXF_BOOT_BAD_LAYOUT;
            }

            EXF_DBG((EXF_PFX "Main boot region checksum bad, backup %s\n",
                     Check == EXF_BOOT_OK ? "used" : "bad too"));
        }

        if (Check != EXF_BOOT_OK) {

            if (Check != EXF_BOOT_NOT_EXFAT) {
                EXF_DBG((EXF_PFX "exFAT boot sector rejected (%lu)\n", Check));
            }

            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Status = IoCreateDevice(ExfData.DriverObject,
                                sizeof(EXF_VCB),
                                NULL,
                                FILE_DEVICE_DISK_FILE_SYSTEM,
                                0,
                                FALSE,
                                &VolumeDevice);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);

        if (TargetDevice->AlignmentRequirement > VolumeDevice->AlignmentRequirement) {
            VolumeDevice->AlignmentRequirement = TargetDevice->AlignmentRequirement;
        }

#ifndef EXF_NT31
        VolumeDevice->SectorSize = (USHORT)SectorSize;  /* a spare field on NT 3.1 */
#endif

#ifdef DO_DEVICE_INITIALIZING
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

        Vcb = (PEXF_VCB)VolumeDevice->DeviceExtension;
        RtlZeroMemory(Vcb, sizeof(EXF_VCB));

        Vcb->NodeTypeCode = EXF_NTC_VCB;
        Vcb->NodeByteSize = (CSHORT)sizeof(EXF_VCB);
        ExInitializeResourceLite(&Vcb->Resource);
        ExInitializeResourceLite(&Vcb->AllocResource);
        InitializeListHead(&Vcb->FcbList);
        InitializeListHead(&Vcb->DirNotifyList);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);

        Vcb->Vpb = Vpb;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->VolumeDeviceObject = VolumeDevice;
        Vcb->VerifyThread = KeGetCurrentThread();
        Vcb->AllocHint = EXFAT_FIRST_CLUSTER;

        Vcb->SectorShift = Boot->BytesPerSectorShift;
        Vcb->SectorSize = (ULONG)1 << Vcb->SectorShift;
        Vcb->SectorsPerClusterShift = Boot->SectorsPerClusterShift;
        Vcb->ClusterShift = Vcb->SectorShift + Vcb->SectorsPerClusterShift;
        Vcb->ClusterSize = (ULONG)1 << Vcb->ClusterShift;
        Vcb->VolumeBytes = (LONGLONG)(Boot->VolumeLength << Vcb->SectorShift);
        Vcb->PartitionBytes = (PartitionBytes > Vcb->VolumeBytes) ? PartitionBytes : Vcb->VolumeBytes;
        Vcb->NumberOfFats = Boot->NumberOfFats;
        Vcb->VolumeFlags = Boot->VolumeFlags;
        Vcb->FatLength = Boot->FatLength;
        Vcb->FatSector = Boot->FatOffset;
        if (Vcb->NumberOfFats == 2 && (Vcb->VolumeFlags & EXFAT_VOLUME_ACTIVE_FAT)) {
            Vcb->FatSector += Boot->FatLength;
        }
        Vcb->ClusterHeapSector = Boot->ClusterHeapOffset;
        Vcb->ClusterCount = Boot->ClusterCount;
        Vcb->RootCluster = Boot->FirstClusterOfRootDirectory;
        Vcb->SerialNumber = Boot->VolumeSerialNumber;
        Vcb->Revision = Boot->FileSystemRevision;

        /* Left dirty by someone else: it stays so until checked */
        if (Vcb->VolumeFlags & EXFAT_VOLUME_DIRTY) {
            Vcb->VcbState |= VCB_STATE_KEEP_DIRTY;
        }

        /* TexFAT keeps two FATs and bitmaps in step; only read those */
        if (Vcb->NumberOfFats != 1) {
            Vcb->VcbState |= VCB_STATE_READ_ONLY;
        }

        if (RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
            Vcb->VcbState |= VCB_STATE_REMOVABLE;
        }

        ExfCheckDeviceState(Vcb, TargetDevice);

        if (RealDevice->Flags & DO_VERIFY_VOLUME) {
            RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            ClearedVerify = TRUE;
        }

        /* Everything below reads straight from the disk */
        Vcb->FatFcb = ExfCreateMetaFcb(Vcb, (LONGLONG)Vcb->FatSector << Vcb->SectorShift, 0,
                                       (ULONGLONG)Vcb->FatLength << Vcb->SectorShift);
        if (Vcb->FatFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Vcb->RootDcb = ExfCreateRootDcb(Vcb);
        if (Vcb->RootDcb == NULL) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }

        Status = ExfLoadRootMetadata(Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vcb->BitmapFcb = ExfCreateMetaFcb(Vcb, 0, Vcb->BitmapCluster, Vcb->BitmapLength);
        if (Vcb->BitmapFcb == NULL) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            __leave;
        }

        Status = ExfCountFreeClusters(Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vcb->VolumeFcb = ExfCreateVolumeFcb(Vcb);
        if (Vcb->VolumeFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* From here on paging I/O on the metadata streams must reach us */
        Vpb->DeviceObject = VolumeDevice;
        Vcb->VcbState |= VCB_STATE_MOUNTED;

        Status = ExfOpenStream(Vcb, Vcb->FatFcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vpb->SerialNumber = Vcb->SerialNumber;
        Vpb->VolumeLabelLength = (USHORT)(Vcb->LabelLength * sizeof(WCHAR));
        for (i = 0; i < Vcb->LabelLength; i++) {
            Vpb->VolumeLabel[i] = Vcb->Label[i];
        }

        Vcb->VerifyThread = NULL;

        /* Keep the target alive until the VCB goes, even after a removal.
           ObReferenceObject is a 4.0 export, this one is older. */
        (VOID)ObReferenceObjectByPointer(TargetDevice, 0, NULL, KernelMode);

        (VOID)ExAcquireResourceExclusiveLite(&ExfData.Resource, TRUE);
        InsertTailList(&ExfData.VcbList, &Vcb->VcbLinks);
        ExfRelease(&ExfData.Resource);

        EXF_DBG((EXF_PFX "Mounted %08lX: %lu clusters of %lu bytes, %lu free%s%s\n",
                 Vcb->SerialNumber, Vcb->ClusterCount, Vcb->ClusterSize, Vcb->FreeClusters,
                 (Vcb->VolumeFlags & EXFAT_VOLUME_DIRTY) ? ", dirty" : "",
                 (Vcb->VcbState & VCB_STATE_READ_ONLY) ? ", read-only" : ""));

    } __finally {

        ExFreePool(Region);

        if (AbnormalTermination() || !NT_SUCCESS(Status)) {

            if (Vcb != NULL) {

                Vpb->DeviceObject = NULL;

                if (ClearedVerify) {
                    RealDevice->Flags |= DO_VERIFY_VOLUME;
                }

                InitializeListHead(&Vcb->VcbLinks);

                while (!IsListEmpty(&Vcb->FcbList)) {
                    ExfDeleteFcb(CONTAINING_RECORD(Vcb->FcbList.Flink, EXF_FCB, FcbLinks));
                }

                if (Vcb->VolumeFcb != NULL) {
                    ExfDeleteFcb(Vcb->VolumeFcb);
                }

                /* A stream that failed to open may still be closing */
                if (Vcb->FatFcb != NULL && !(Vcb->FatFcb->FcbState & FCB_STATE_STREAM_OPEN)) {
                    ExfDeleteFcb(Vcb->FatFcb);
                }

                if (Vcb->BitmapFcb != NULL) {
                    ExfDeleteFcb(Vcb->BitmapFcb);
                }

                if (Vcb->Upcase != NULL) {
                    ExFreePool(Vcb->Upcase);
                }

                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
                ExDeleteResourceLite(&Vcb->AllocResource);
                ExDeleteResourceLite(&Vcb->Resource);
            }

            if (VolumeDevice != NULL) {
                IoDeleteDevice(VolumeDevice);
            }
        }
    }

#ifndef EXF_NT4
    if (NT_SUCCESS(Status)) {
        (VOID)FsRtlNotifyVolumeEvent(Vcb->FatFcb->StreamFile, FSRTL_VOLUME_MOUNT);
    }
#endif

    return Status;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExfVerifyVolume (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PVPB Vpb = IrpSp->Parameters.VerifyVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = IrpSp->Parameters.VerifyVolume.DeviceObject;
    PEXF_VCB Vcb;
    PEXFAT_BOOT_SECTOR Boot = NULL;
    BOOLEAN Same = FALSE;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (VolumeDevice == NULL || VolumeDevice->DriverObject != ExfData.DriverObject ||
        VolumeDevice == ExfData.FileSystemDeviceObject) {

        return STATUS_WRONG_VOLUME;
    }

    Vcb = (PEXF_VCB)VolumeDevice->DeviceExtension;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        if (!(Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {
            __leave;
        }

        Vcb->VerifyThread = KeGetCurrentThread();

        Boot = (PEXFAT_BOOT_SECTOR)ExAllocatePoolWithTag(NonPagedPool, Vcb->SectorSize, EXF_TAG_BUFFER);
        if (Boot == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = ExfReadSectors(Vcb->TargetDeviceObject, 0, Vcb->SectorSize, Boot, TRUE);

        if (NT_SUCCESS(Status) &&
            ExfCheckBootSector(Boot, Vcb->SectorSize, 0) == EXF_BOOT_OK &&
            Boot->VolumeSerialNumber == Vcb->SerialNumber &&
            Boot->ClusterCount == Vcb->ClusterCount &&
            Boot->FirstClusterOfRootDirectory == Vcb->RootCluster &&
            (LONGLONG)(Boot->VolumeLength << Vcb->SectorShift) == Vcb->VolumeBytes) {

            Same = TRUE;
        }

        if (Same) {

            Status = STATUS_SUCCESS;

        } else {

            EXF_DBG((EXF_PFX "Verify: volume %08lX is gone\n", Vcb->SerialNumber));

            Delete = ExfDismountVcb(Vcb);
            Status = STATUS_WRONG_VOLUME;
        }

        Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    } __finally {

        if (Boot != NULL) {
            ExFreePool(Boot);
        }

        Vcb->VerifyThread = NULL;
        ExfRelease(&Vcb->Resource);
    }

    if (Delete) {
        ExfDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* User requests                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExfGetVolumeOpen (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_CCB *Ccb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_FCB Fcb;

    if (FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PEXF_FCB)FileObject->FsContext;
    *Ccb = (PEXF_CCB)FileObject->FsContext2;

    if (Fcb == NULL || *Ccb == NULL || !ExfIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
ExfNotifyVolume (
    PFILE_OBJECT FileObject,
    ULONG Event
    )
{
#ifndef EXF_NT4
    (VOID)FsRtlNotifyVolumeEvent(FileObject, Event);
#else
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Event);
#endif
}

#ifndef FSRTL_VOLUME_DISMOUNT
#define FSRTL_VOLUME_DISMOUNT       1
#endif
#ifndef FSRTL_VOLUME_LOCK
#define FSRTL_VOLUME_LOCK           3
#endif
#ifndef FSRTL_VOLUME_LOCK_FAILED
#define FSRTL_VOLUME_LOCK_FAILED    4
#endif
#ifndef FSRTL_VOLUME_UNLOCK
#define FSRTL_VOLUME_UNLOCK         5
#endif

static NTSTATUS
ExfLockVolume (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_CCB Ccb;
    KIRQL Irql;
    NTSTATUS Status;

    Status = ExfGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ExfNotifyVolume(FileObject, FSRTL_VOLUME_LOCK);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* Whoever locks is about to work on the disk itself */
        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)ExfFlushVolume(Vcb, TRUE);
            ExfPurgeCachedFiles(Vcb);
        }

        /* Only the caller's handle may be open */
        if (Vcb->UncleanCount != 1) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;

        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    if (!NT_SUCCESS(Status)) {
        ExfNotifyVolume(FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    return Status;
}

static NTSTATUS
ExfUnlockVolume (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_CCB Ccb;
    NTSTATUS Status;

    Status = ExfGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject) {

            Status = STATUS_NOT_LOCKED;

        } else {

            ExfUnlockVcb(Vcb);

            /* Metadata may have been rewritten under us */
            if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                (VOID)ExfDismountVcb(Vcb);
            }
        }

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    if (NT_SUCCESS(Status)) {
        ExfNotifyVolume(FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return Status;
}

static NTSTATUS
ExfDismountVolume (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_CCB Ccb;
    NTSTATUS Status;

    Status = ExfGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ExfNotifyVolume(FileObject, FSRTL_VOLUME_DISMOUNT);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)ExfFlushVolume(Vcb, TRUE);
        }

        /* The caller's handle stays open, so the VCB cannot go away here */
        Ccb->Flags |= CCB_FLAG_DISMOUNTED_VOLUME;
        (VOID)ExfDismountVcb(Vcb);

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
ExfIsVolumeDirty (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PULONG Result = (PULONG)Irp->AssociatedIrp.SystemBuffer;

    if (ExfXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Dirty for chkdsk, not merely in use */
    *Result = (Vcb->VcbState & VCB_STATE_KEEP_DIRTY) ? VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS
ExfMarkVolumeDirtyRequest (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExfVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_KEEP_DIRTY;
        ExfMarkVolumeDirty(Vcb);

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

    return Status;
}

static NTSTATUS
ExfUserFsRequest (
    PEXF_IRP_CONTEXT Ctx
    )
{
    ULONG Code = ExfXSp(Ctx->IrpSp)->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {

    case FSCTL_LOCK_VOLUME:
        return ExfLockVolume(Ctx);

    case FSCTL_UNLOCK_VOLUME:
        return ExfUnlockVolume(Ctx);

    case FSCTL_DISMOUNT_VOLUME:
        return ExfDismountVolume(Ctx);

    case FSCTL_IS_VOLUME_MOUNTED:
        return ExfVerifyVcb(Ctx, Ctx->Vcb);

    case FSCTL_IS_PATHNAME_VALID:
        return STATUS_SUCCESS;

    case FSCTL_IS_VOLUME_DIRTY:
        return ExfIsVolumeDirty(Ctx);

    case FSCTL_MARK_VOLUME_DIRTY:
        return ExfMarkVolumeDirtyRequest(Ctx);

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return STATUS_OPLOCK_NOT_GRANTED;

    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        return STATUS_INVALID_OPLOCK_PROTOCOL;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
ExfCommonFileSystemControl (
    PEXF_IRP_CONTEXT Ctx
    )
{
    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_MOUNT_VOLUME:
        if (Ctx->Vcb != NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return ExfMountVolume(Ctx);

    case IRP_MN_VERIFY_VOLUME:
        return ExfVerifyVolume(Ctx);

    case IRP_MN_USER_FS_REQUEST:
#ifdef IRP_MN_KERNEL_CALL
    case IRP_MN_KERNEL_CALL:
#endif
        if (Ctx->Vcb == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return ExfUserFsRequest(Ctx);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Plug and Play (Windows 2000)                                        */
/* ------------------------------------------------------------------ */

#ifndef EXF_NT4

static NTSTATUS
NTAPI
ExfPnpCompletion (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Sends the IRP down and waits; the caller completes it */
static NTSTATUS
ExfForwardAndWait (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_VCB Vcb
    )
{
    PIRP Irp = Ctx->Irp;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    ExfCopyStackToNext(Irp);
    IoSetCompletionRoutine(Irp, ExfPnpCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Vcb->TargetDeviceObject, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
ExfCommonPnp (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status;

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_REMOVE_DEVICE:

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)ExfFlushVolume(Vcb, TRUE);
            ExfPurgeCachedFiles(Vcb);
        }

        if (Vcb->UncleanCount != 0 || (Vcb->VcbState & VCB_STATE_LOCKED)) {
            ExfRelease(&Vcb->Resource);
            return STATUS_ACCESS_DENIED;
        }

        Vcb->VcbState |= VCB_STATE_PNP_LOCKED;
        ExfRelease(&Vcb->Resource);

        Status = ExfForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (NT_SUCCESS(Status)) {
            Delete = ExfDismountVcb(Vcb);
        } else {
            Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        }

        ExfRelease(&Vcb->Resource);
        break;

    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:

        Status = ExfForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        Vcb->VcbState |= VCB_STATE_REMOVED;
        Delete = ExfDismountVcb(Vcb);
        ExfRelease(&Vcb->Resource);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        Status = ExfForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        ExfRelease(&Vcb->Resource);
        break;

    default:

        if (Vcb->VcbState & VCB_STATE_REMOVED) {
            return STATUS_NO_SUCH_DEVICE;
        }

        ExfSkipStack(Irp);
        Ctx->Flags |= EXF_CTX_NO_COMPLETE;
        return IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }

    if (Delete) {
        ExfDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

#else

NTSTATUS
ExfCommonPnp (
    PEXF_IRP_CONTEXT Ctx
    )
{
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#endif
