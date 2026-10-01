/*
 * EXFATNT - directory scanning and IRP_MJ_DIRECTORY_CONTROL
 */

#include "exfat.h"

PEXF_SCAN
ExfAllocateScan (
    VOID
    )
{
    PEXF_SCAN Scan;

    Scan = (PEXF_SCAN)ExAllocatePoolWithTag(PagedPool, sizeof(EXF_SCAN), EXF_TAG_BUFFER);
    if (Scan != NULL) {
        RtlZeroMemory(&Scan->Map, sizeof(EXF_MAP));
    }

    return Scan;
}

VOID
ExfFreeScan (
    PEXF_SCAN Scan
    )
{
    if (Scan != NULL) {
        ExfUnmap(&Scan->Map);
        ExFreePool(Scan);
    }
}

#define ExfMapDirEntry(Vcb, Dcb, Offset, Map) \
    ExfMapStream((Vcb), (Dcb), (Map), (Offset), EXFAT_DIRENT_SIZE)

/*
 * Finds the next valid file entry set at or after *Offset and decodes it
 * into Scan->Dirent. With WantHash, sets whose name hash differs are
 * skipped without decoding. *Offset moves past the returned set.
 */
BOOLEAN
ExfNextFileEntry (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    PULONG Offset,
    const USHORT *WantHash,
    PEXF_SCAN Scan
    )
{
    ULONG Size = Dcb->Header.AllocationSize.LowPart;
    ULONG Current = *Offset;
    ULONG Count;
    ULONG i;
    PUCHAR Entry;
    PEXFAT_STREAM_ENTRY Stream;

    while (Current + EXFAT_DIRENT_SIZE <= Size) {

        Entry = ExfMapDirEntry(Vcb, Dcb, Current, &Scan->Map);

        if (Entry[0] == EXFAT_ENTRY_EOD) {
            break;
        }

        if (Entry[0] != EXFAT_ENTRY_FILE) {
            Current += EXFAT_DIRENT_SIZE;
            continue;
        }

        Count = (ULONG)Entry[1] + 1;

        if (Count < EXFAT_MIN_SECONDARY + 1 || Count > EXFAT_MAX_SET_ENTRIES ||
            Current + Count * EXFAT_DIRENT_SIZE > Size) {

            Current += EXFAT_DIRENT_SIZE;
            continue;
        }

        RtlCopyMemory(Scan->Set, Entry, EXFAT_DIRENT_SIZE);

        Entry = ExfMapDirEntry(Vcb, Dcb, Current + EXFAT_DIRENT_SIZE, &Scan->Map);

        if (WantHash != NULL) {

            Stream = (PEXFAT_STREAM_ENTRY)Entry;

            if (Stream->EntryType == EXFAT_ENTRY_STREAM && Stream->NameHash != *WantHash) {
                Current += Count * EXFAT_DIRENT_SIZE;
                continue;
            }
        }

        RtlCopyMemory(Scan->Set + EXFAT_DIRENT_SIZE, Entry, EXFAT_DIRENT_SIZE);

        for (i = 2; i < Count; i++) {
            Entry = ExfMapDirEntry(Vcb, Dcb, Current + i * EXFAT_DIRENT_SIZE, &Scan->Map);
            RtlCopyMemory(Scan->Set + i * EXFAT_DIRENT_SIZE, Entry, EXFAT_DIRENT_SIZE);
        }

        if (ExfParseEntrySet(Scan->Set, Count, &Scan->Dirent) != EXF_SET_OK) {

            EXF_DBG((EXF_PFX "Bad entry set at %lX in directory %lu\n",
                     Current, Dcb->FirstCluster));

            Current += EXFAT_DIRENT_SIZE;
            continue;
        }

        Scan->Dirent.Offset = Current;
        *Offset = Current + Count * EXFAT_DIRENT_SIZE;
        return TRUE;
    }

    *Offset = Current;
    return FALSE;
}

NTSTATUS
ExfLookupName (
    PEXF_VCB Vcb,
    PEXF_FCB Dcb,
    PUNICODE_STRING Name,
    PEXF_SCAN Scan
    )
{
    ULONG Length = Name->Length / sizeof(WCHAR);
    ULONG Offset = 0;
    USHORT Hash;

    Hash = ExfNameHash(Vcb->Upcase, Name->Buffer, Length);

    while (ExfNextFileEntry(Vcb, Dcb, &Offset, &Hash, Scan)) {

        if (ExfNamesEqual(Vcb->Upcase, Scan->Dirent.Name, Scan->Dirent.NameLength,
                          Name->Buffer, Length)) {

            return STATUS_SUCCESS;
        }
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_QUERY_DIRECTORY                                              */
/* ------------------------------------------------------------------ */

/* What one directory listing entry needs */
typedef struct _EXF_LIST_ENTRY {
    ULONG           FileIndex;
    LARGE_INTEGER   CreationTime;
    LARGE_INTEGER   LastAccessTime;
    LARGE_INTEGER   LastWriteTime;
    LARGE_INTEGER   EndOfFile;
    LARGE_INTEGER   AllocationSize;
    ULONG           Attributes;
    UNICODE_STRING  Name;
} EXF_LIST_ENTRY, *PEXF_LIST_ENTRY;

static VOID
ExfListFromFcb (
    PEXF_FCB Fcb,
    PEXF_LIST_ENTRY List,
    PWSTR Name,
    USHORT NameLength,
    ULONG FileIndex
    )
{
    List->FileIndex = FileIndex;
    List->CreationTime = Fcb->CreationTime;
    List->LastAccessTime = Fcb->LastAccessTime;
    List->LastWriteTime = Fcb->LastWriteTime;
    List->EndOfFile.QuadPart = 0;
    List->AllocationSize.QuadPart = 0;
    List->Attributes = FILE_ATTRIBUTE_DIRECTORY;
    List->Name.Buffer = Name;
    List->Name.Length = NameLength;
    List->Name.MaximumLength = NameLength;
}

static VOID
ExfListFromDirent (
    PEXF_VCB Vcb,
    PEXF_DIRENT Dirent,
    PEXF_LIST_ENTRY List
    )
{
    List->FileIndex = 2 + (Dirent->Offset >> EXFAT_DIRENT_SHIFT);
    List->CreationTime = ExfConvertTime(Dirent->CreateTimestamp, Dirent->Create10ms,
                                        Dirent->CreateUtcOffset);
    List->LastWriteTime = ExfConvertTime(Dirent->ModifyTimestamp, Dirent->Modify10ms,
                                         Dirent->ModifyUtcOffset);
    List->LastAccessTime = ExfConvertTime(Dirent->AccessTimestamp, 0, Dirent->AccessUtcOffset);
    List->Attributes = ExfDirentNtAttributes(Dirent->Attributes);

    if (Dirent->Attributes & EXFAT_ATTR_DIRECTORY) {
        List->EndOfFile.QuadPart = 0;
        List->AllocationSize.QuadPart = 0;
    } else {
        List->EndOfFile.QuadPart = (LONGLONG)Dirent->DataLength;
        List->AllocationSize.QuadPart = ExfRoundUp((LONGLONG)Dirent->DataLength, Vcb->ClusterSize);
    }

    List->Name.Buffer = Dirent->Name;
    List->Name.Length = (USHORT)(Dirent->NameLength * sizeof(WCHAR));
    List->Name.MaximumLength = List->Name.Length;
}

static ULONG
ExfListBaseLength (
    FILE_INFORMATION_CLASS Class
    )
{
    switch (Class) {
    case FileDirectoryInformation:
        return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
    case FileFullDirectoryInformation:
        return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
    case FileBothDirectoryInformation:
        return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
    case FileNamesInformation:
        return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);

    default:
        return 0;
    }
}

/* Writes one entry; returns the bytes of name actually copied */
static ULONG
ExfFillListEntry (
    FILE_INFORMATION_CLASS Class,
    PUCHAR Buffer,
    ULONG Room,
    PEXF_LIST_ENTRY List
    )
{
    PFILE_DIRECTORY_INFORMATION Dir;
    PFILE_FULL_DIR_INFORMATION Full;
    PFILE_BOTH_DIR_INFORMATION Both;
    PFILE_NAMES_INFORMATION Names;
    ULONG Base = ExfListBaseLength(Class);
    ULONG Copy;

    RtlZeroMemory(Buffer, Base);

    Copy = List->Name.Length;
    if (Copy > Room - Base) {
        Copy = Room - Base;
    }

    switch (Class) {

    case FileDirectoryInformation:
        Dir = (PFILE_DIRECTORY_INFORMATION)Buffer;
        Dir->FileIndex = List->FileIndex;
        Dir->CreationTime = List->CreationTime;
        Dir->LastAccessTime = List->LastAccessTime;
        Dir->LastWriteTime = List->LastWriteTime;
        Dir->ChangeTime = List->LastWriteTime;
        Dir->EndOfFile = List->EndOfFile;
        Dir->AllocationSize = List->AllocationSize;
        Dir->FileAttributes = List->Attributes;
        Dir->FileNameLength = List->Name.Length;
        RtlCopyMemory(Dir->FileName, List->Name.Buffer, Copy);
        break;

    case FileFullDirectoryInformation:
        Full = (PFILE_FULL_DIR_INFORMATION)Buffer;
        Full->FileIndex = List->FileIndex;
        Full->CreationTime = List->CreationTime;
        Full->LastAccessTime = List->LastAccessTime;
        Full->LastWriteTime = List->LastWriteTime;
        Full->ChangeTime = List->LastWriteTime;
        Full->EndOfFile = List->EndOfFile;
        Full->AllocationSize = List->AllocationSize;
        Full->FileAttributes = List->Attributes;
        Full->FileNameLength = List->Name.Length;
        RtlCopyMemory(Full->FileName, List->Name.Buffer, Copy);
        break;

    case FileBothDirectoryInformation:
        Both = (PFILE_BOTH_DIR_INFORMATION)Buffer;
        Both->FileIndex = List->FileIndex;
        Both->CreationTime = List->CreationTime;
        Both->LastAccessTime = List->LastAccessTime;
        Both->LastWriteTime = List->LastWriteTime;
        Both->ChangeTime = List->LastWriteTime;
        Both->EndOfFile = List->EndOfFile;
        Both->AllocationSize = List->AllocationSize;
        Both->FileAttributes = List->Attributes;
        Both->FileNameLength = List->Name.Length;
        RtlCopyMemory(Both->FileName, List->Name.Buffer, Copy);
        break;

    case FileNamesInformation:
        Names = (PFILE_NAMES_INFORMATION)Buffer;
        Names->FileIndex = List->FileIndex;
        Names->FileNameLength = List->Name.Length;
        RtlCopyMemory(Names->FileName, List->Name.Buffer, Copy);
        break;

    default:
        break;
    }

    return Copy;
}

static BOOLEAN
ExfListMatches (
    PEXF_VCB Vcb,
    PEXF_CCB Ccb,
    PUNICODE_STRING Name
    )
{
    if (Ccb->Flags & CCB_FLAG_MATCH_ALL) {
        return TRUE;
    }

    if (Ccb->Flags & CCB_FLAG_WILDCARD) {
        return FsRtlIsNameInExpression(&Ccb->Pattern, Name, TRUE, NULL);
    }

    return ExfNamesEqual(Vcb->Upcase,
                         Ccb->Pattern.Buffer, Ccb->Pattern.Length / sizeof(WCHAR),
                         Name->Buffer, Name->Length / sizeof(WCHAR));
}

static NTSTATUS
ExfSetPattern (
    PEXF_CCB Ccb,
    PUNICODE_STRING FileName
    )
{
    UNICODE_STRING Pattern;
    NTSTATUS Status;

    if (Ccb->Pattern.Buffer != NULL) {
        ExFreePool(Ccb->Pattern.Buffer);
        RtlZeroMemory(&Ccb->Pattern, sizeof(UNICODE_STRING));
    }

    Ccb->Flags &= ~(CCB_FLAG_WILDCARD | CCB_FLAG_MATCH_ALL);

    if (FileName == NULL || FileName->Length == 0 ||
        (FileName->Length == sizeof(WCHAR) && FileName->Buffer[0] == L'*')) {

        Ccb->Flags |= CCB_FLAG_MATCH_ALL | CCB_FLAG_PATTERN_SET;
        return STATUS_SUCCESS;
    }

    Pattern.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, FileName->Length, EXF_TAG_NAME);
    if (Pattern.Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Pattern.Length = 0;
    Pattern.MaximumLength = FileName->Length;

    /* FsRtlIsNameInExpression wants the expression upcased */
    Status = RtlUpcaseUnicodeString(&Pattern, FileName, FALSE);
    if (!NT_SUCCESS(Status)) {
        ExFreePool(Pattern.Buffer);
        return Status;
    }

    Ccb->Pattern = Pattern;

    if (FsRtlDoesNameContainWildCards(&Ccb->Pattern)) {
        Ccb->Flags |= CCB_FLAG_WILDCARD;
    }

    Ccb->Flags |= CCB_FLAG_PATTERN_SET;
    return STATUS_SUCCESS;
}

static NTSTATUS
ExfQueryDirectory (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_FCB Dcb,
    PEXF_CCB Ccb
    )
{
    PEXF_VCB Vcb = Dcb->Vcb;
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    FILE_INFORMATION_CLASS Class;
    PUNICODE_STRING FileName;
    ULONG Length;
    ULONG FileIndex;
    PUCHAR Buffer;
    ULONG Base;
    ULONG EntryLength;
    ULONG Room;
    ULONG Copied;
    ULONG NextOffset = 0;
    ULONG LastOffset = 0;
    ULONG Returned = 0;
    ULONG Information = 0;
    BOOLEAN InitialQuery = FALSE;
    BOOLEAN Found;
    BOOLEAN IsRoot = (BOOLEAN)(Dcb->ParentDcb == NULL);
    ULONG SavedState;
    ULONG SavedOffset;
    EXF_LIST_ENTRY List;
    PEXF_SCAN Scan = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    Class     = ExfXSp(IrpSp)->Parameters.QueryDirectory.FileInformationClass;
    FileName  = ExfXSp(IrpSp)->Parameters.QueryDirectory.FileName;
    Length    = ExfXSp(IrpSp)->Parameters.QueryDirectory.Length;
    FileIndex = ExfXSp(IrpSp)->Parameters.QueryDirectory.FileIndex;

    Base = ExfListBaseLength(Class);
    if (Base == 0) {
        return STATUS_INVALID_INFO_CLASS;
    }

    if (Length < Base) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    Buffer = (PUCHAR)ExfMapUserBuffer(Irp);

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceExclusiveLite(&Dcb->Resource, TRUE);

    __try {

        Status = ExfVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (!(Ccb->Flags & CCB_FLAG_PATTERN_SET) ||
            ((IrpSp->Flags & SL_RESTART_SCAN) && FileName != NULL && FileName->Length != 0)) {

            Status = ExfSetPattern(Ccb, FileName);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            InitialQuery = TRUE;
            Ccb->QueryState = 0;
            Ccb->QueryOffset = 0;
        }

        if (IrpSp->Flags & SL_RESTART_SCAN) {
            Ccb->QueryState = 0;
            Ccb->QueryOffset = 0;
        }

        if (IrpSp->Flags & SL_INDEX_SPECIFIED) {

            /* Resume after the entry FileIndex named */
            if (FileIndex == 0) {
                Ccb->QueryState = 1;
                Ccb->QueryOffset = 0;
            } else {
                Ccb->QueryState = 2;
                Ccb->QueryOffset = (FileIndex < 2) ? 0 :
                                   ((FileIndex - 2) << EXFAT_DIRENT_SHIFT) + EXFAT_DIRENT_SIZE;
            }
        }

        if (IsRoot && Ccb->QueryState < 2) {
            Ccb->QueryState = 2;
        }

        Scan = ExfAllocateScan();
        if (Scan == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        for (;;) {

            SavedState = Ccb->QueryState;
            SavedOffset = Ccb->QueryOffset;

            if (Ccb->QueryState == 0) {

                ExfListFromFcb(Dcb, &List, L".", sizeof(WCHAR), 0);
                Ccb->QueryState = 1;

            } else if (Ccb->QueryState == 1) {

                /* The root has no times of its own: below it, ".." shows the directory's */
                ExfListFromFcb(Dcb->ParentDcb->ParentDcb != NULL ? Dcb->ParentDcb : Dcb,
                               &List, L"..", 2 * sizeof(WCHAR), 1);
                Ccb->QueryState = 2;

            } else {

                Found = ExfNextFileEntry(Vcb, Dcb, &Ccb->QueryOffset, NULL, Scan);
                if (!Found) {
                    break;
                }

                ExfListFromDirent(Vcb, &Scan->Dirent, &List);
            }

            if (!ExfListMatches(Vcb, Ccb, &List.Name)) {
                continue;
            }

            EntryLength = Base + List.Name.Length;
            Room = Length - NextOffset;

            if (NextOffset + Base > Length ||
                (Returned != 0 && EntryLength > Room)) {

                /* Leave this entry for the next call */
                Ccb->QueryState = SavedState;
                Ccb->QueryOffset = SavedOffset;

                if (Returned == 0) {
                    Status = STATUS_BUFFER_OVERFLOW;
                }

                break;
            }

            if (Returned != 0) {
                *(PULONG)(Buffer + LastOffset) = NextOffset - LastOffset;
            }

            Copied = ExfFillListEntry(Class, Buffer + NextOffset, Room, &List);

            Returned++;
            LastOffset = NextOffset;
            Information = NextOffset + Base + Copied;

            if (Copied < List.Name.Length) {
                Status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            if (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY) {
                break;
            }

            NextOffset = (Information + 7) & ~7UL;

            if (NextOffset >= Length) {
                break;
            }
        }

        if (Returned == 0 && NT_SUCCESS(Status)) {
            Status = InitialQuery ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        }

        Irp->IoStatus.Information = Information;

    } __finally {

        ExfFreeScan(Scan);
        ExfRelease(&Dcb->Resource);
        ExfRelease(&Vcb->Resource);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* IRP_MN_NOTIFY_CHANGE_DIRECTORY                                      */
/* ------------------------------------------------------------------ */

static NTSTATUS
ExfNotifyChangeDirectory (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_FCB Dcb,
    PEXF_CCB Ccb
    )
{
    PEXF_VCB Vcb = Dcb->Vcb;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    NTSTATUS Status;

#ifdef EXF_NT31
    /* The NT 3.1 FsRtl notify package predates the one used here */
    UNREFERENCED_PARAMETER(Vcb);
    UNREFERENCED_PARAMETER(IrpSp);
    UNREFERENCED_PARAMETER(Ccb);
    Status = STATUS_INVALID_DEVICE_REQUEST;
#else
    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = ExfVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = ExfBuildFullName(Dcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync,
                                       &Vcb->DirNotifyList,
                                       Ccb,
                                       (PSTRING)&Dcb->FullName,
                                       (BOOLEAN)((IrpSp->Flags & SL_WATCH_TREE) != 0),
                                       FALSE,
                                       ExfXSp(IrpSp)->Parameters.NotifyDirectory.CompletionFilter,
                                       Ctx->Irp,
                                       NULL,
                                       NULL);

        Ctx->Flags |= EXF_CTX_NO_COMPLETE;
        Status = STATUS_PENDING;

    } __finally {

        ExfRelease(&Vcb->Resource);
    }

#endif
    return Status;
}

NTSTATUS
ExfCommonDirectoryControl (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_FCB Dcb = (PEXF_FCB)FileObject->FsContext;
    PEXF_CCB Ccb = (PEXF_CCB)FileObject->FsContext2;

    if (Dcb == NULL || Ccb == NULL || !ExfIsDcb(Dcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_DIRECTORY:
        return ExfQueryDirectory(Ctx, Dcb, Ccb);

    case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        return ExfNotifyChangeDirectory(Ctx, Dcb, Ccb);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
