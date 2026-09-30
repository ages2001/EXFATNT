/*
 * EXFATNT - FCB, CCB, internal streams, names and times
 *
 * All FCBs of a volume sit on Vcb->FcbList. A file has exactly one FCB,
 * found by (parent, name), so every open of it shares one set of section
 * object pointers. An FCB holds a reference on its parent.
 *
 * Directories, the FAT and the allocation bitmap are cached through an
 * internal stream file each. A directory's stream is opened on first use
 * and closed when the directory FCB is no longer referenced; the FCB is
 * freed only once the cache manager has let go of that stream.
 */

#include "exfat.h"

/* ------------------------------------------------------------------ */
/* Times                                                               */
/* ------------------------------------------------------------------ */

LARGE_INTEGER
ExfConvertTime (
    ULONG Timestamp,
    UCHAR Increment10ms,
    UCHAR UtcOffset
    )
{
    EXF_TIME_PARTS Parts;
    TIME_FIELDS Fields;
    LARGE_INTEGER Time;
    LONG Minutes;

    Time.QuadPart = 0;

    if (!ExfDecodeTimestamp(Timestamp, Increment10ms, &Parts)) {
        return Time;
    }

    Fields.Year         = (CSHORT)Parts.Year;
    Fields.Month        = (CSHORT)Parts.Month;
    Fields.Day          = (CSHORT)Parts.Day;
    Fields.Hour         = (CSHORT)Parts.Hour;
    Fields.Minute       = (CSHORT)Parts.Minute;
    Fields.Second       = (CSHORT)Parts.Second;
    Fields.Milliseconds = (CSHORT)Parts.Milliseconds;
    Fields.Weekday      = 0;

    if (!RtlTimeFieldsToTime(&Fields, &Time)) {
        Time.QuadPart = 0;
        return Time;
    }

    if (ExfDecodeUtcOffset(UtcOffset, &Minutes)) {
        Time.QuadPart -= (LONGLONG)Minutes * 60 * 10000000;
    } else {
        /* No offset recorded: the stamp is in local time */
        ExLocalTimeToSystemTime(&Time, &Time);
    }

    return Time;
}

/* Local time with the offset from UTC, as Windows stores it */
VOID
ExfEncodeTime (
    LARGE_INTEGER Time,
    PULONG Timestamp,
    PUCHAR Increment10ms,
    PUCHAR UtcOffset
    )
{
    LARGE_INTEGER Local;
    TIME_FIELDS Fields;
    EXF_TIME_PARTS Parts;
    LONGLONG Bias;

    ExSystemTimeToLocalTime(&Time, &Local);
    RtlTimeToTimeFields(&Local, &Fields);

    Parts.Year         = (USHORT)Fields.Year;
    Parts.Month        = (USHORT)Fields.Month;
    Parts.Day          = (USHORT)Fields.Day;
    Parts.Hour         = (USHORT)Fields.Hour;
    Parts.Minute       = (USHORT)Fields.Minute;
    Parts.Second       = (USHORT)Fields.Second;
    Parts.Milliseconds = (USHORT)Fields.Milliseconds;

    if (!ExfEncodeTimestamp(&Parts, Timestamp, Increment10ms)) {
        *Timestamp = (1UL << 21) | (1UL << 16);
        *Increment10ms = 0;
    }

    /* Minutes east of UTC */
    Bias = (Local.QuadPart - Time.QuadPart) / (60 * (LONGLONG)10000000);
    *UtcOffset = ExfEncodeUtcOffset((LONG)Bias);
}

/* The FCB's attributes, times and stream fields in on-disk form */
VOID
ExfFcbToDirent (
    PEXF_FCB Fcb,
    PEXF_DIRENT Dirent
    )
{
    UCHAR Unused;

    RtlZeroMemory(Dirent, FIELD_OFFSET(EXF_DIRENT, Name));

    Dirent->Attributes = Fcb->Attributes;
    Dirent->StreamFlags = (UCHAR)(Fcb->StreamFlags | EXFAT_STREAM_ALLOC_POSSIBLE);
    Dirent->FirstCluster = Fcb->FirstCluster;

    if (Fcb->FirstCluster == 0) {
        Dirent->StreamFlags &= ~EXFAT_STREAM_NO_FAT_CHAIN;
    }

    Dirent->DataLength = (ULONGLONG)Fcb->Header.FileSize.QuadPart;
    Dirent->ValidDataLength = (ULONGLONG)Fcb->Header.ValidDataLength.QuadPart;

    ExfEncodeTime(Fcb->CreationTime, &Dirent->CreateTimestamp, &Dirent->Create10ms,
                  &Dirent->CreateUtcOffset);
    ExfEncodeTime(Fcb->LastWriteTime, &Dirent->ModifyTimestamp, &Dirent->Modify10ms,
                  &Dirent->ModifyUtcOffset);
    ExfEncodeTime(Fcb->LastAccessTime, &Dirent->AccessTimestamp, &Unused,
                  &Dirent->AccessUtcOffset);
}

/* ------------------------------------------------------------------ */
/* FCBs                                                                */
/* ------------------------------------------------------------------ */

static PEXF_FCB
ExfAllocateFcb (
    PEXF_VCB Vcb,
    CSHORT NodeType
    )
{
    PEXF_FCB Fcb;

    Fcb = (PEXF_FCB)ExAllocatePoolWithTag(NonPagedPool, sizeof(EXF_FCB), EXF_TAG_FCB);
    if (Fcb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(EXF_FCB));

    Fcb->Header.NodeTypeCode = NodeType;
    Fcb->Header.NodeByteSize = (CSHORT)sizeof(EXF_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;

    ExInitializeResourceLite(&Fcb->Resource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);

    Fcb->Header.Resource = &Fcb->Resource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;

    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);

    Fcb->Vcb = Vcb;
    InitializeListHead(&Fcb->FcbLinks);

    return Fcb;
}

VOID
ExfDeleteFcb (
    PEXF_FCB Fcb
    )
{
    RemoveEntryList(&Fcb->FcbLinks);

    FsRtlUninitializeFileLock(&Fcb->FileLock);

    ExDeleteResourceLite(&Fcb->Resource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }

    if (Fcb->FullName.Buffer != NULL) {
        ExFreePool(Fcb->FullName.Buffer);
    }

    ExfFreeRunList(&Fcb->RunList);

    ExFreePool(Fcb);
}

/* Represents the whole volume for DASD opens */
PEXF_FCB
ExfCreateVolumeFcb (
    PEXF_VCB Vcb
    )
{
    PEXF_FCB Fcb;

    Fcb = ExfAllocateFcb(Vcb, EXF_NTC_VFCB);
    if (Fcb == NULL) {
        return NULL;
    }

    Fcb->Header.AllocationSize.QuadPart = Vcb->VolumeBytes;
    Fcb->Header.FileSize.QuadPart = Vcb->VolumeBytes;
    Fcb->Header.ValidDataLength.QuadPart = Vcb->VolumeBytes;

    return Fcb;
}

/*
 * The FAT (Lbo != 0: a linear range of the volume) or the allocation
 * bitmap (a cluster chain). Not on Vcb->FcbList.
 */
PEXF_FCB
ExfCreateMetaFcb (
    PEXF_VCB Vcb,
    LONGLONG Lbo,
    ULONG FirstCluster,
    ULONGLONG Length
    )
{
    PEXF_FCB Fcb;
    NTSTATUS Status;

    Fcb = ExfAllocateFcb(Vcb, EXF_NTC_META);
    if (Fcb == NULL) {
        return NULL;
    }

    if (Lbo != 0) {

        Fcb->MetaLbo = Lbo;
        Fcb->Header.AllocationSize.QuadPart = (LONGLONG)Length;

    } else {

        Status = ExfBuildMetadataRunList(Vcb, FirstCluster, Length, &Fcb->RunList);
        if (!NT_SUCCESS(Status)) {
            ExfDeleteFcb(Fcb);
            return NULL;
        }

        Fcb->FirstCluster = FirstCluster;
        Fcb->Header.AllocationSize.QuadPart = (LONGLONG)Fcb->RunList.Clusters << Vcb->ClusterShift;
    }

    Fcb->Header.FileSize.QuadPart = (LONGLONG)Length;
    Fcb->Header.ValidDataLength.QuadPart = (LONGLONG)Length;

    return Fcb;
}

PEXF_FCB
ExfCreateRootDcb (
    PEXF_VCB Vcb
    )
{
    PEXF_FCB Dcb;
    NTSTATUS Status;

    Dcb = ExfAllocateFcb(Vcb, EXF_NTC_DCB);
    if (Dcb == NULL) {
        return NULL;
    }

    Status = ExfBuildRunList(Vcb, Vcb->RootCluster, FALSE, 0, &Dcb->RunList);

    if (!NT_SUCCESS(Status) ||
        ((ULONGLONG)Dcb->RunList.Clusters << Vcb->ClusterShift) > EXFAT_MAX_DIR_SIZE) {

        ExfDeleteFcb(Dcb);
        return NULL;
    }

    Dcb->FcbState = FCB_STATE_ROOT;
    Dcb->Attributes = EXFAT_ATTR_DIRECTORY;
    Dcb->FirstCluster = Vcb->RootCluster;
    Dcb->DirOffset = 0xFFFFFFFF;
    Dcb->IndexNumber = Vcb->RootCluster;

    Dcb->Header.AllocationSize.QuadPart = (LONGLONG)Dcb->RunList.Clusters << Vcb->ClusterShift;
    Dcb->Header.FileSize = Dcb->Header.AllocationSize;
    Dcb->Header.ValidDataLength = Dcb->Header.AllocationSize;

    InsertTailList(&Vcb->FcbList, &Dcb->FcbLinks);

    return Dcb;
}

NTSTATUS
ExfCreateFcb (
    PEXF_VCB Vcb,
    PEXF_FCB ParentDcb,
    PEXF_DIRENT Dirent,
    PEXF_FCB *NewFcb
    )
{
    PEXF_FCB Fcb;
    BOOLEAN IsDirectory;
    ULONGLONG Clusters;
    NTSTATUS Status;

    *NewFcb = NULL;

    IsDirectory = (BOOLEAN)((Dirent->Attributes & EXFAT_ATTR_DIRECTORY) != 0);

    Clusters = (Dirent->DataLength + Vcb->ClusterSize - 1) >> Vcb->ClusterShift;

    if (Clusters > Vcb->ClusterCount) {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (IsDirectory &&
        (Dirent->FirstCluster == 0 ||
         Dirent->DataLength == 0 ||
         Dirent->DataLength > EXFAT_MAX_DIR_SIZE ||
         (Dirent->DataLength & (Vcb->ClusterSize - 1)) != 0)) {

        return STATUS_FILE_CORRUPT_ERROR;
    }

    Fcb = ExfAllocateFcb(Vcb, (CSHORT)(IsDirectory ? EXF_NTC_DCB : EXF_NTC_FCB));
    if (Fcb == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Fcb->Name.Length = (USHORT)(Dirent->NameLength * sizeof(WCHAR));
    Fcb->Name.MaximumLength = Fcb->Name.Length;
    Fcb->Name.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Fcb->Name.Length, EXF_TAG_NAME);

    if (Fcb->Name.Buffer == NULL) {
        ExfDeleteFcb(Fcb);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Fcb->Name.Buffer, Dirent->Name, Fcb->Name.Length);

    Status = ExfBuildRunList(Vcb,
                             Dirent->FirstCluster,
                             (BOOLEAN)((Dirent->StreamFlags & EXFAT_STREAM_NO_FAT_CHAIN) != 0),
                             (ULONG)Clusters,
                             &Fcb->RunList);

    if (!NT_SUCCESS(Status)) {
        ExfDeleteFcb(Fcb);
        return Status;
    }

    Fcb->ParentDcb    = ParentDcb;
    Fcb->DirOffset    = Dirent->Offset;
    Fcb->EntryCount   = Dirent->EntryCount;
    Fcb->Attributes   = Dirent->Attributes;
    Fcb->NameHash     = Dirent->NameHash;
    Fcb->StreamFlags  = (UCHAR)(Dirent->StreamFlags & EXFAT_STREAM_NO_FAT_CHAIN);
    Fcb->FirstCluster = Dirent->FirstCluster;
    Fcb->IndexNumber  = ((LONGLONG)ParentDcb->FirstCluster << 32) | Dirent->Offset;

    if (Fcb->FirstCluster == 0) {
        Fcb->StreamFlags = 0;
    }

    Fcb->Header.AllocationSize.QuadPart = (LONGLONG)Clusters << Vcb->ClusterShift;

    if (IsDirectory) {
        Fcb->Header.FileSize = Fcb->Header.AllocationSize;
        Fcb->Header.ValidDataLength = Fcb->Header.AllocationSize;
    } else {
        Fcb->Header.FileSize.QuadPart = (LONGLONG)Dirent->DataLength;
        Fcb->Header.ValidDataLength.QuadPart = (LONGLONG)Dirent->ValidDataLength;
    }

    Fcb->CreationTime = ExfConvertTime(Dirent->CreateTimestamp, Dirent->Create10ms,
                                       Dirent->CreateUtcOffset);
    Fcb->LastWriteTime = ExfConvertTime(Dirent->ModifyTimestamp, Dirent->Modify10ms,
                                        Dirent->ModifyUtcOffset);
    Fcb->LastAccessTime = ExfConvertTime(Dirent->AccessTimestamp, 0,
                                         Dirent->AccessUtcOffset);

    InsertTailList(&Vcb->FcbList, &Fcb->FcbLinks);
    ParentDcb->RefCount++;

    *NewFcb = Fcb;
    return STATUS_SUCCESS;
}

/*
 * Drops one reference; unreferenced FCBs are freed along with any parent
 * that loses its last reference. The root and volume FCBs stay until the
 * VCB goes away. A directory whose stream is still open is freed when
 * that stream's close arrives. Called with Vcb->Resource exclusive.
 */
VOID
ExfDereferenceFcb (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    PEXF_FCB Parent;

    while (Fcb != NULL) {

        Fcb->RefCount--;

        if (Fcb->RefCount != 0 || Fcb == Vcb->RootDcb || Fcb == Vcb->VolumeFcb) {
            break;
        }

        if (Fcb->FcbState & FCB_STATE_STREAM_OPEN) {

            /* ExfStreamClosed finishes the job, perhaps right away */
            Fcb->FcbState |= FCB_STATE_TEARDOWN;
            ExfCloseStream(Vcb, Fcb, (BOOLEAN)((Fcb->FcbState & FCB_STATE_DELETED) != 0));
            break;
        }

        Parent = Fcb->ParentDcb;
        ExfDeleteFcb(Fcb);
        Fcb = Parent;
    }
}

PEXF_FCB
ExfFindFcb (
    PEXF_VCB Vcb,
    PEXF_FCB ParentDcb,
    PUNICODE_STRING Name
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Fcb;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Fcb = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);

        /* Deleted files and directories on their way out do not count */
        if (Fcb->ParentDcb == ParentDcb &&
            !(Fcb->FcbState & (FCB_STATE_DELETED | FCB_STATE_TEARDOWN)) &&
            ExfNamesEqual(Vcb->Upcase,
                          Fcb->Name.Buffer, Fcb->Name.Length / sizeof(WCHAR),
                          Name->Buffer, Name->Length / sizeof(WCHAR))) {

            return Fcb;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Internal streams                                                    */
/* ------------------------------------------------------------------ */

/*
 * Opens the cached stream of a directory, the FAT or the bitmap. The
 * stream file object counts in Vcb->OpenCount but not in Fcb->RefCount.
 */
NTSTATUS
ExfOpenStream (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    PFILE_OBJECT StreamFile = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb->StreamFile != NULL) {
        return STATUS_SUCCESS;
    }

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return EXF_STATUS_DISMOUNTED;
    }

    __try {

        StreamFile = IoCreateStreamFileObject(NULL, Vcb->Vpb->RealDevice);

        StreamFile->Vpb = Vcb->Vpb;
        StreamFile->FsContext = Fcb;
        StreamFile->FsContext2 = NULL;
        StreamFile->SectionObjectPointer = &Fcb->SectionObjectPointers;
        StreamFile->ReadAccess = TRUE;
        StreamFile->WriteAccess = TRUE;
        StreamFile->DeleteAccess = TRUE;

        Vcb->OpenCount++;
        Fcb->FcbState |= FCB_STATE_STREAM_OPEN;

        CcInitializeCacheMap(StreamFile,
                             (PCC_FILE_SIZES)&Fcb->Header.AllocationSize,
                             TRUE,
                             &ExfData.MetaCacheCallbacks,
                             Fcb);

        Fcb->StreamFile = StreamFile;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();

        if (StreamFile != NULL) {
            /* Our close handler takes back the count */
            ObDereferenceObject(StreamFile);
        }
    }

    return Status;
}

/*
 * Closes the internal stream. Dirty data is written first, or thrown
 * away with Discard (the object is being deleted or the volume is gone).
 * Called with Vcb->Resource exclusive; the close can arrive before this
 * returns.
 */
VOID
ExfCloseStream (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb,
    BOOLEAN Discard
    )
{
    PFILE_OBJECT StreamFile = Fcb->StreamFile;
    IO_STATUS_BLOCK Iosb;

    UNREFERENCED_PARAMETER(Vcb);

    if (StreamFile == NULL) {
        return;
    }

    if (!Discard) {
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
    }

    (VOID)CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);

    Fcb->StreamFile = NULL;

    /* Let the section go now rather than when memory runs short, so the
       close (which may free Fcb) comes with our own dereference */
    CcUninitializeCacheMap(StreamFile, NULL, NULL);
    (VOID)MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
    ObDereferenceObject(StreamFile);
}

/* The close of an internal stream arrived */
VOID
ExfStreamClosed (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    PEXF_FCB Parent;

    Fcb->FcbState &= ~FCB_STATE_STREAM_OPEN;
    Vcb->OpenCount--;

    if ((Fcb->FcbState & FCB_STATE_TEARDOWN) && Fcb->RefCount == 0) {

        Parent = Fcb->ParentDcb;
        ExfDeleteFcb(Fcb);
        ExfDereferenceFcb(Vcb, Parent);
    }
}

/* ------------------------------------------------------------------ */
/* CCBs                                                                */
/* ------------------------------------------------------------------ */

PEXF_CCB
ExfCreateCcb (
    VOID
    )
{
    PEXF_CCB Ccb;

    Ccb = (PEXF_CCB)ExAllocatePoolWithTag(PagedPool, sizeof(EXF_CCB), EXF_TAG_CCB);
    if (Ccb == NULL) {
        return NULL;
    }

    RtlZeroMemory(Ccb, sizeof(EXF_CCB));
    Ccb->NodeTypeCode = EXF_NTC_CCB;
    Ccb->NodeByteSize = (CSHORT)sizeof(EXF_CCB);

    return Ccb;
}

VOID
ExfDeleteCcb (
    PEXF_CCB Ccb
    )
{
    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
    }

    ExFreePool(Ccb);
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* "\dir\file", kept in the FCB for change notification */
NTSTATUS
ExfBuildFullName (
    PEXF_FCB Fcb
    )
{
    PEXF_FCB Walk;
    ULONG Length = 0;
    PWSTR Buffer;
    PWSTR Write;

    if (Fcb->FullName.Buffer != NULL) {
        return STATUS_SUCCESS;
    }

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    if (Length > 0xFFFE) {
        return STATUS_NAME_TOO_LONG;
    }

    Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, EXF_TAG_NAME);
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {

        Buffer[0] = L'\\';

    } else {

        Write = (PWSTR)((PUCHAR)Buffer + Length);

        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    Fcb->FullName.Buffer = Buffer;
    Fcb->FullName.Length = (USHORT)Length;
    Fcb->FullName.MaximumLength = (USHORT)Length;

    return STATUS_SUCCESS;
}

BOOLEAN
ExfIsAncestor (
    PEXF_FCB Ancestor,
    PEXF_FCB Fcb
    )
{
    for (; Fcb != NULL; Fcb = Fcb->ParentDcb) {
        if (Fcb == Ancestor) {
            return TRUE;
        }
    }

    return FALSE;
}

/* After a rename: drop the cached full names of Fcb and all below it */
VOID
ExfForgetNames (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    PLIST_ENTRY Entry;
    PEXF_FCB Walk;

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

        Walk = CONTAINING_RECORD(Entry, EXF_FCB, FcbLinks);

        if (Walk->FullName.Buffer != NULL && ExfIsAncestor(Fcb, Walk)) {
            ExFreePool(Walk->FullName.Buffer);
            RtlZeroMemory(&Walk->FullName, sizeof(UNICODE_STRING));
        }
    }
}
