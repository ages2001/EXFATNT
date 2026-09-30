/*
 * EXFATNT - changing directories: creating, rewriting, moving and
 * removing entry sets, growing directories, the volume label, change
 * notification
 *
 * Everything here runs with Vcb->Resource exclusive.
 */

#include "exfat.h"

/* ------------------------------------------------------------------ */
/* Entry set access                                                    */
/* ------------------------------------------------------------------ */

static VOID
ExfReadEntries (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    ULONG Offset,
    ULONG Count,
    PUCHAR Set
    )
{
    EXF_MAP Map;
    ULONG i;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (i = 0; i < Count; i++) {
            RtlCopyMemory(Set + i * EXFAT_DIRENT_SIZE,
                          ExfMapStream(Vcb, Dcb, &Map, Offset + i * EXFAT_DIRENT_SIZE,
                                       EXFAT_DIRENT_SIZE),
                          EXFAT_DIRENT_SIZE);
        }

    } __finally {

        ExfUnmap(&Map);
    }
}

/* Stores entries First..First+Count-1 of Set at their places */
static VOID
ExfWriteEntries (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    ULONG Offset,
    ULONG First,
    ULONG Count,
    PUCHAR Set
    )
{
    EXF_MAP Map;
    ULONG i;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (i = First; i < First + Count; i++) {
            RtlCopyMemory(ExfPinStream(Vcb, Dcb, &Map, Offset + i * EXFAT_DIRENT_SIZE,
                                       EXFAT_DIRENT_SIZE),
                          Set + i * EXFAT_DIRENT_SIZE,
                          EXFAT_DIRENT_SIZE);
            ExfSetDirty(&Map);
        }

    } __finally {

        ExfUnmap(&Map);
    }
}

/* Writes zeros straight to clusters no cached stream refers to yet */
static NTSTATUS
ExfZeroClusters (
    PEXF_VCB Vcb,
    ULONG Lcn,
    ULONG Count
    )
{
    PUCHAR Zero;
    LONGLONG Lbo = ExfClusterToLbo(Vcb, Lcn);
    ULONGLONG Left = (ULONGLONG)Count << Vcb->ClusterShift;
    ULONG Chunk;
    NTSTATUS Status = STATUS_SUCCESS;

    Zero = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, EXF_ZERO_CHUNK, EXF_TAG_BUFFER);
    if (Zero == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Zero, EXF_ZERO_CHUNK);

    while (Left != 0 && NT_SUCCESS(Status)) {

        Chunk = (Left > EXF_ZERO_CHUNK) ? EXF_ZERO_CHUNK : (ULONG)Left;

        Status = ExfWriteSectors(Vcb->TargetDeviceObject, Lbo, Chunk, Zero, FALSE);

        Lbo += Chunk;
        Left -= Chunk;
    }

    ExFreePool(Zero);
    return Status;
}

/* ------------------------------------------------------------------ */
/* Directory growth and free slots                                     */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExfGrowDirectory (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb
    )
{
    LONGLONG OldSize = Dcb->Header.AllocationSize.QuadPart;
    NTSTATUS Status;

    if (OldSize + Vcb->ClusterSize > EXFAT_MAX_DIR_SIZE) {
        return STATUS_CANNOT_MAKE;
    }

    Status = ExfSetAllocation(Vcb, Dcb, (ULONGLONG)OldSize + Vcb->ClusterSize);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Dcb->Header.FileSize = Dcb->Header.AllocationSize;
    Dcb->Header.ValidDataLength = Dcb->Header.AllocationSize;

    if (Dcb->StreamFile != NULL) {
        CcSetFileSizes(Dcb->StreamFile, (PCC_FILE_SIZES)&Dcb->Header.AllocationSize);
    }

    ExfZeroStream(Vcb, Dcb, OldSize, Vcb->ClusterSize);

    if (Dcb->FcbState & FCB_STATE_ROOT) {
        return STATUS_SUCCESS;
    }

    return ExfUpdateDirent(Vcb, Dcb);
}

/*
 * Finds Count consecutive unused entries in Dcb, growing it when needed,
 * and returns the offset of the first.
 */
static NTSTATUS
ExfFindFreeSlots (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    ULONG Count,
    PULONG Offset
    )
{
    EXF_MAP Map;
    ULONG Size = Dcb->Header.AllocationSize.LowPart;
    ULONG Pos;
    ULONG Run = 0;
    ULONG RunStart = 0;
    UCHAR Type;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (Pos = 0; Pos + EXFAT_DIRENT_SIZE <= Size; Pos += EXFAT_DIRENT_SIZE) {

            Type = *ExfMapStream(Vcb, Dcb, &Map, Pos, 1);

            if (Type == EXFAT_ENTRY_EOD) {

                /* Everything from here to the end is free */
                if (Run == 0) {
                    RunStart = Pos;
                }

                Run += (Size - Pos) / EXFAT_DIRENT_SIZE;
                break;
            }

            if (Type & EXFAT_TYPE_IN_USE) {
                Run = 0;
                continue;
            }

            if (Run == 0) {
                RunStart = Pos;
            }

            if (++Run == Count) {
                break;
            }
        }

    } __finally {

        ExfUnmap(&Map);
    }

    if (Run >= Count) {
        *Offset = RunStart;
        return STATUS_SUCCESS;
    }

    /* A free run that reaches the end continues into new clusters */
    if (Run == 0) {
        RunStart = Size;
    }

    while ((Dcb->Header.AllocationSize.LowPart - RunStart) / EXFAT_DIRENT_SIZE < Count) {

        Status = ExfGrowDirectory(Vcb, Dcb);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    *Offset = RunStart;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Entry sets                                                          */
/* ------------------------------------------------------------------ */

/*
 * Adds a new file or directory named Name to Dcb and returns its decoded
 * entry set. A directory gets one zeroed cluster.
 */
NTSTATUS
ExfCreateDirent (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    PUNICODE_STRING Name,
    USHORT Attributes,
    PEXF_DIRENT Dirent
    )
{
    UCHAR Set[EXFAT_MAX_SET_ENTRIES * EXFAT_DIRENT_SIZE];
    ULONG NameLength = Name->Length / sizeof(WCHAR);
    EXF_RUN_LIST Runs;
    EXF_DIRENT Info;
    LARGE_INTEGER Now;
    UCHAR Flags = 0;
    ULONG Count;
    ULONG Offset;
    UCHAR Unused;
    NTSTATUS Status;

    RtlZeroMemory(&Runs, sizeof(Runs));
    RtlZeroMemory(&Info, FIELD_OFFSET(EXF_DIRENT, Name));

    KeQuerySystemTime(&Now);

    ExfEncodeTime(Now, &Info.CreateTimestamp, &Info.Create10ms, &Info.CreateUtcOffset);
    Info.ModifyTimestamp = Info.CreateTimestamp;
    Info.Modify10ms = Info.Create10ms;
    Info.ModifyUtcOffset = Info.CreateUtcOffset;
    ExfEncodeTime(Now, &Info.AccessTimestamp, &Unused, &Info.AccessUtcOffset);

    Info.Attributes = Attributes;
    Info.StreamFlags = EXFAT_STREAM_ALLOC_POSSIBLE;

    if (Attributes & EXFAT_ATTR_DIRECTORY) {

        Status = ExfAllocateClusters(Vcb, &Runs, &Flags, 1);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        Status = ExfZeroClusters(Vcb, Runs.Runs[0].Lcn, 1);
        if (!NT_SUCCESS(Status)) {
            goto Fail;
        }

        Info.FirstCluster = Runs.Runs[0].Lcn;
        Info.StreamFlags |= Flags;
        Info.DataLength = Vcb->ClusterSize;
        Info.ValidDataLength = Vcb->ClusterSize;
    }

    Count = ExfBuildEntrySet(Vcb->Upcase, Name->Buffer, NameLength, &Info, Set);

    Status = ExfFindFreeSlots(Vcb, Dcb, Count, &Offset);
    if (!NT_SUCCESS(Status)) {
        goto Fail;
    }

    ExfWriteEntries(Vcb, Dcb, Offset, 0, Count, Set);

    if (ExfParseEntrySet(Set, Count, Dirent) != EXF_SET_OK) {
        /* Cannot happen: we just built it */
        Status = STATUS_FILE_CORRUPT_ERROR;
        ExfRemoveDirent(Vcb, Dcb, Offset, Count);
        goto Fail;
    }

    Dirent->Offset = Offset;
    ExfFreeRunList(&Runs);

    return STATUS_SUCCESS;

Fail:
    if (Runs.RunCount != 0) {
        ExfFreeClusters(Vcb, &Runs, &Flags, 0);
    }

    ExfFreeRunList(&Runs);
    return Status;
}

/* Rewrites the entry set of Fcb from its current state */
NTSTATUS
ExfUpdateDirent (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    UCHAR Set[EXFAT_MAX_SET_ENTRIES * EXFAT_DIRENT_SIZE];
    PEXFAT_FILE_ENTRY File = (PEXFAT_FILE_ENTRY)Set;
    EXF_DIRENT Info;

    if ((Fcb->FcbState & (FCB_STATE_ROOT | FCB_STATE_DELETED)) || Fcb->ParentDcb == NULL ||
        (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED))) {

        Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;
        return STATUS_SUCCESS;
    }

    if (Fcb->EntryCount < EXFAT_MIN_SECONDARY + 1 || Fcb->EntryCount > EXFAT_MAX_SET_ENTRIES) {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ExfReadEntries(Vcb, Fcb->ParentDcb, Fcb->DirOffset, Fcb->EntryCount, Set);

    if (File->EntryType != EXFAT_ENTRY_FILE ||
        (ULONG)File->SecondaryCount + 1 != Fcb->EntryCount ||
        Set[EXFAT_DIRENT_SIZE] != EXFAT_ENTRY_STREAM) {

        EXF_DBG((EXF_PFX "Entry set of %wZ moved away\n", &Fcb->Name));
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ExfFcbToDirent(Fcb, &Info);
    ExfStoreEntrySet(Set, Fcb->EntryCount, &Info);

    /* The names are unchanged; the checksum lives in the File entry */
    ExfWriteEntries(Vcb, Fcb->ParentDcb, Fcb->DirOffset, 0, 2, Set);

    Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;

    return STATUS_SUCCESS;
}

NTSTATUS
ExfRemoveDirent (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    ULONG Offset,
    ULONG Count
    )
{
    EXF_MAP Map;
    PUCHAR Entry;
    ULONG i;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (i = 0; i < Count; i++) {
            Entry = ExfPinStream(Vcb, Dcb, &Map, Offset + i * EXFAT_DIRENT_SIZE, EXFAT_DIRENT_SIZE);
            Entry[0] &= (UCHAR)~EXFAT_TYPE_IN_USE;
            ExfSetDirty(&Map);
        }

    } __finally {

        ExfUnmap(&Map);
    }

    return STATUS_SUCCESS;
}

/*
 * Gives Fcb a new name in TargetDcb (its own directory or another one):
 * the new entry set is written first, then the old one is removed. The
 * caller has made sure the name is free.
 */
NTSTATUS
ExfMoveDirent (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb,
    PEXF_FCB TargetDcb,
    PUNICODE_STRING Name
    )
{
    UCHAR Set[EXFAT_MAX_SET_ENTRIES * EXFAT_DIRENT_SIZE];
    ULONG NameLength = Name->Length / sizeof(WCHAR);
    PEXF_FCB OldParent = Fcb->ParentDcb;
    EXF_DIRENT Info;
    PWSTR NewName;
    ULONG Count;
    ULONG Offset;
    NTSTATUS Status;

    NewName = (PWSTR)ExAllocatePoolWithTag(PagedPool, Name->Length, EXF_TAG_NAME);
    if (NewName == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(NewName, Name->Buffer, Name->Length);

    ExfFcbToDirent(Fcb, &Info);
    Count = ExfBuildEntrySet(Vcb->Upcase, Name->Buffer, NameLength, &Info, Set);

    Status = ExfFindFreeSlots(Vcb, TargetDcb, Count, &Offset);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(NewName);
        return Status;
    }

    ExfWriteEntries(Vcb, TargetDcb, Offset, 0, Count, Set);
    ExfRemoveDirent(Vcb, OldParent, Fcb->DirOffset, Fcb->EntryCount);

    if (Fcb->Name.Buffer != NULL) {
        ExFreePool(Fcb->Name.Buffer);
    }

    Fcb->Name.Buffer = NewName;
    Fcb->Name.Length = Name->Length;
    Fcb->Name.MaximumLength = Name->Length;
    Fcb->NameHash = ((PEXFAT_STREAM_ENTRY)(Set + EXFAT_DIRENT_SIZE))->NameHash;
    Fcb->DirOffset = Offset;
    Fcb->EntryCount = Count;
    Fcb->IndexNumber = ((LONGLONG)TargetDcb->FirstCluster << 32) | Offset;
    Fcb->FcbState &= ~FCB_STATE_DIRENT_DIRTY;

    if (TargetDcb != OldParent) {
        TargetDcb->RefCount++;
        Fcb->ParentDcb = TargetDcb;
        ExfDereferenceFcb(Vcb, OldParent);
    }

    ExfForgetNames(Vcb, Fcb);

    return STATUS_SUCCESS;
}

/*
 * Removes a file or an empty directory from the disk: its entry set
 * first, then its clusters. The FCB stays, marked deleted, until its
 * last reference goes. A file's cached data must be gone already.
 */
NTSTATUS
ExfDeleteFromDisk (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb
    )
{
    NTSTATUS Status;

    if (Fcb->FcbState & FCB_STATE_DELETED) {
        return STATUS_SUCCESS;
    }

    if (ExfIsDcb(Fcb)) {

        /* Nothing of the directory may reach the disk any more */
        ExfCloseStream(Vcb, Fcb, TRUE);
    }

    Status = ExfRemoveDirent(Vcb, Fcb->ParentDcb, Fcb->DirOffset, Fcb->EntryCount);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Fcb->FcbState |= FCB_STATE_DELETED;
    Fcb->FcbState &= ~(FCB_STATE_DIRENT_DIRTY | FCB_STATE_TRUNCATE_ON_CLOSE);

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    Fcb->Header.FileSize.QuadPart = 0;
    Fcb->Header.ValidDataLength.QuadPart = 0;

    ExfRelease(&Fcb->PagingIoResource);

    return ExfSetAllocation(Vcb, Fcb, 0);
}

NTSTATUS
ExfIsDirectoryEmpty (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    PBOOLEAN Empty
    )
{
    EXF_MAP Map;
    ULONG Size = Dcb->Header.AllocationSize.LowPart;
    ULONG Pos;
    UCHAR Type;

    RtlZeroMemory(&Map, sizeof(Map));
    *Empty = TRUE;

    __try {

        for (Pos = 0; Pos + EXFAT_DIRENT_SIZE <= Size; Pos += EXFAT_DIRENT_SIZE) {

            Type = *ExfMapStream(Vcb, Dcb, &Map, Pos, 1);

            if (Type == EXFAT_ENTRY_EOD) {
                break;
            }

            if (Type & EXFAT_TYPE_IN_USE) {
                *Empty = FALSE;
                break;
            }
        }

    } __finally {

        ExfUnmap(&Map);
    }

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Volume label                                                        */
/* ------------------------------------------------------------------ */

NTSTATUS
ExfWriteLabel (
    PEXF_VCB Vcb,
    const WCHAR *Label,
    ULONG Length
    )
{
    PEXF_FCB Root = Vcb->RootDcb;
    UCHAR Entry[EXFAT_DIRENT_SIZE];
    PEXFAT_LABEL_ENTRY LabelEntry = (PEXFAT_LABEL_ENTRY)Entry;
    EXF_MAP Map;
    ULONG Size = Root->Header.AllocationSize.LowPart;
    ULONG Pos;
    ULONG Found = 0xFFFFFFFF;
    ULONG i;
    UCHAR Type;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (Pos = 0; Pos + EXFAT_DIRENT_SIZE <= Size; Pos += EXFAT_DIRENT_SIZE) {

            Type = *ExfMapStream(Vcb, Root, &Map, Pos, 1);

            if (Type == EXFAT_ENTRY_EOD) {
                break;
            }

            if (Type == EXFAT_ENTRY_LABEL) {
                Found = Pos;
                break;
            }
        }

    } __finally {

        ExfUnmap(&Map);
    }

    if (Found == 0xFFFFFFFF) {

        if (Length == 0) {
            Vcb->LabelLength = 0;
            return STATUS_SUCCESS;
        }

        Status = ExfFindFreeSlots(Vcb, Root, 1, &Found);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    RtlZeroMemory(Entry, sizeof(Entry));
    LabelEntry->EntryType = EXFAT_ENTRY_LABEL;
    LabelEntry->CharacterCount = (UCHAR)Length;

    for (i = 0; i < Length; i++) {
        LabelEntry->VolumeLabel[i] = Label[i];
        Vcb->Label[i] = Label[i];
    }

    Vcb->LabelLength = Length;

    ExfWriteEntries(Vcb, Root, Found, 0, 1, Entry);

    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Change notification                                                 */
/* ------------------------------------------------------------------ */

VOID
ExfNotifyChange (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb,
    ULONG Filter,
    ULONG Action
    )
{
    if (Fcb->ParentDcb == NULL || !NT_SUCCESS(ExfBuildFullName(Fcb))) {
        return;
    }

    FsRtlNotifyFullReportChange(Vcb->NotifySync,
                                &Vcb->DirNotifyList,
                                (PSTRING)&Fcb->FullName,
                                (USHORT)(Fcb->FullName.Length - Fcb->Name.Length),
                                NULL,
                                NULL,
                                Filter,
                                Action,
                                NULL);
}
