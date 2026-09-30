/*
 * EXFATNT - on-disk format helpers
 *
 * Checksums, up-case table expansion, boot sector validation, directory
 * entry set decoding and timestamp decoding. Nothing here calls the
 * kernel.
 */

#include "exfat.h"

ULONG
ExfBootChecksum (
    const UCHAR *Region,
    ULONG BytesPerSector
    )
{
    ULONG Checksum = 0;
    ULONG Count = BytesPerSector * EXFAT_BOOT_CHECKSUM_SECTOR;
    ULONG i;

    for (i = 0; i < Count; i++) {

        /* VolumeFlags and PercentInUse are not covered */
        if (i == 106 || i == 107 || i == 112) {
            continue;
        }

        Checksum = ((Checksum & 1) ? 0x80000000UL : 0) + (Checksum >> 1) + Region[i];
    }

    return Checksum;
}

USHORT
ExfSetChecksum (
    const UCHAR *Set,
    ULONG EntryCount
    )
{
    USHORT Checksum = 0;
    ULONG Count = EntryCount * EXFAT_DIRENT_SIZE;
    ULONG i;

    for (i = 0; i < Count; i++) {

        /* SetChecksum field of the File entry */
        if (i == 2 || i == 3) {
            continue;
        }

        Checksum = (USHORT)(((Checksum & 1) ? 0x8000 : 0) + (Checksum >> 1) + Set[i]);
    }

    return Checksum;
}

ULONG
ExfTableChecksum (
    const UCHAR *Data,
    ULONG Length
    )
{
    ULONG Checksum = 0;
    ULONG i;

    for (i = 0; i < Length; i++) {
        Checksum = ((Checksum & 1) ? 0x80000000UL : 0) + (Checksum >> 1) + Data[i];
    }

    return Checksum;
}

USHORT
ExfNameHash (
    const USHORT *Upcase,
    const WCHAR *Name,
    ULONG Length
    )
{
    USHORT Hash = 0;
    USHORT Char;
    ULONG i;

    for (i = 0; i < Length; i++) {

        Char = Upcase[(USHORT)Name[i]];

        Hash = (USHORT)(((Hash & 1) ? 0x8000 : 0) + (Hash >> 1) + (Char & 0xFF));
        Hash = (USHORT)(((Hash & 1) ? 0x8000 : 0) + (Hash >> 1) + (Char >> 8));
    }

    return Hash;
}

/*
 * The stored table may be compressed: 0xFFFF followed by N means the next
 * N characters map to themselves. Characters past the end of the table
 * also map to themselves.
 */
VOID
ExfExpandUpcase (
    const USHORT *Source,
    ULONG SourceCount,
    USHORT *Table
    )
{
    ULONG Char = 0;
    ULONG i = 0;
    ULONG Run;

    while (i < SourceCount && Char < EXFAT_UPCASE_CHARS) {

        if (Source[i] == 0xFFFF && i + 1 < SourceCount) {

            Run = Source[i + 1];
            i += 2;

            while (Run != 0 && Char < EXFAT_UPCASE_CHARS) {
                Table[Char] = (USHORT)Char;
                Char++;
                Run--;
            }

        } else {

            Table[Char] = Source[i];
            Char++;
            i++;
        }
    }

    while (Char < EXFAT_UPCASE_CHARS) {
        Table[Char] = (USHORT)Char;
        Char++;
    }
}

ULONG
ExfCheckBootSector (
    const EXFAT_BOOT_SECTOR *Boot,
    ULONG DeviceSectorSize,
    ULONGLONG DeviceSectors
    )
{
    static const UCHAR Name[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    ULONG SectorShift;
    ULONGLONG FatEnd;
    ULONGLONG HeapEnd;
    ULONGLONG FatBytesNeeded;
    ULONG i;

    for (i = 0; i < sizeof(Name); i++) {
        if (Boot->FileSystemName[i] != Name[i]) {
            return EXF_BOOT_NOT_EXFAT;
        }
    }

    /* The BPB area must be zero, so FAT drivers never take the volume */
    for (i = 0; i < sizeof(Boot->MustBeZero); i++) {
        if (Boot->MustBeZero[i] != 0) {
            return EXF_BOOT_NOT_EXFAT;
        }
    }

    if (Boot->BootSignature != 0xAA55) {
        return EXF_BOOT_NOT_EXFAT;
    }

    if ((Boot->FileSystemRevision >> 8) != 1) {
        return EXF_BOOT_BAD_REVISION;
    }

    SectorShift = Boot->BytesPerSectorShift;

    if (SectorShift < EXFAT_MIN_SECTOR_SHIFT ||
        SectorShift > EXFAT_MAX_SECTOR_SHIFT ||
        Boot->SectorsPerClusterShift > EXFAT_MAX_CLUSTER_SHIFT - SectorShift) {

        return EXF_BOOT_BAD_GEOMETRY;
    }

    if (DeviceSectorSize != 0 && ((ULONG)1 << SectorShift) != DeviceSectorSize) {
        return EXF_BOOT_BAD_GEOMETRY;
    }

    if (Boot->NumberOfFats != 1 && Boot->NumberOfFats != 2) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    if (Boot->ClusterCount == 0 || Boot->ClusterCount > EXFAT_MAX_CLUSTER_COUNT) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    if (Boot->FatOffset < EXFAT_MIN_FAT_OFFSET || Boot->FatLength == 0) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    FatBytesNeeded = ((ULONGLONG)Boot->ClusterCount + EXFAT_FIRST_CLUSTER) * 4;

    if (((ULONGLONG)Boot->FatLength << SectorShift) < FatBytesNeeded) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    FatEnd = (ULONGLONG)Boot->FatOffset +
             (ULONGLONG)Boot->FatLength * Boot->NumberOfFats;

    if ((ULONGLONG)Boot->ClusterHeapOffset < FatEnd) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    HeapEnd = (ULONGLONG)Boot->ClusterHeapOffset +
              ((ULONGLONG)Boot->ClusterCount << Boot->SectorsPerClusterShift);

    if (HeapEnd > Boot->VolumeLength || (Boot->VolumeLength >> (62 - SectorShift)) != 0) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    if (DeviceSectors != 0 && Boot->VolumeLength > DeviceSectors) {
        return EXF_BOOT_BAD_LAYOUT;
    }

    if (Boot->FirstClusterOfRootDirectory < EXFAT_FIRST_CLUSTER ||
        Boot->FirstClusterOfRootDirectory >
            (ULONGLONG)Boot->ClusterCount + EXFAT_FIRST_CLUSTER - 1) {

        return EXF_BOOT_BAD_LAYOUT;
    }

    return EXF_BOOT_OK;
}

/*
 * Set points at a File entry followed by Available - 1 more entries.
 * EXF_SET_NEED_MORE: the set is longer than Available (Dirent->EntryCount
 * holds the required count).
 */
ULONG
ExfParseEntrySet (
    const UCHAR *Set,
    ULONG Available,
    PEXF_DIRENT Dirent
    )
{
    const EXFAT_FILE_ENTRY *File = (const EXFAT_FILE_ENTRY *)Set;
    const EXFAT_STREAM_ENTRY *Stream;
    const EXFAT_NAME_ENTRY *NameEntry;
    ULONG Count;
    ULONG NameEntries;
    ULONG Copied;
    ULONG Chunk;
    ULONG i;
    ULONG j;
    UCHAR Type;

    if (Available == 0 || File->EntryType != EXFAT_ENTRY_FILE) {
        return EXF_SET_BAD;
    }

    if (File->SecondaryCount < EXFAT_MIN_SECONDARY ||
        File->SecondaryCount > EXFAT_MAX_SECONDARY) {

        return EXF_SET_BAD;
    }

    Count = (ULONG)File->SecondaryCount + 1;
    Dirent->EntryCount = Count;

    if (Available < Count) {
        return EXF_SET_NEED_MORE;
    }

    Stream = (const EXFAT_STREAM_ENTRY *)(Set + EXFAT_DIRENT_SIZE);

    if (Stream->EntryType != EXFAT_ENTRY_STREAM || Stream->NameLength == 0) {
        return EXF_SET_BAD;
    }

    NameEntries = ((ULONG)Stream->NameLength + EXFAT_NAME_PER_ENTRY - 1) / EXFAT_NAME_PER_ENTRY;

    if (NameEntries > Count - 2) {
        return EXF_SET_BAD;
    }

    Copied = 0;

    for (i = 0; i < NameEntries; i++) {

        NameEntry = (const EXFAT_NAME_ENTRY *)(Set + (i + 2) * EXFAT_DIRENT_SIZE);

        if (NameEntry->EntryType != EXFAT_ENTRY_NAME) {
            return EXF_SET_BAD;
        }

        Chunk = Stream->NameLength - Copied;
        if (Chunk > EXFAT_NAME_PER_ENTRY) {
            Chunk = EXFAT_NAME_PER_ENTRY;
        }

        for (j = 0; j < Chunk; j++) {
            Dirent->Name[Copied + j] = NameEntry->FileName[j];
        }

        Copied += Chunk;
    }

    /* Anything after the names must be an in-use benign secondary entry */
    for (i = NameEntries + 2; i < Count; i++) {

        Type = Set[i * EXFAT_DIRENT_SIZE];

        if ((Type & (EXFAT_TYPE_IN_USE | EXFAT_TYPE_SECONDARY | EXFAT_TYPE_BENIGN)) !=
            (EXFAT_TYPE_IN_USE | EXFAT_TYPE_SECONDARY | EXFAT_TYPE_BENIGN)) {

            return EXF_SET_BAD;
        }
    }

    if (ExfSetChecksum(Set, Count) != File->SetChecksum) {
        return EXF_SET_BAD;
    }

    if (Stream->ValidDataLength > Stream->DataLength) {
        return EXF_SET_BAD;
    }

    if (Stream->FirstCluster == 0 && Stream->DataLength != 0) {
        return EXF_SET_BAD;
    }

    Dirent->Attributes      = File->FileAttributes;
    Dirent->SetChecksum     = File->SetChecksum;
    Dirent->CreateTimestamp = File->CreateTimestamp;
    Dirent->ModifyTimestamp = File->LastModifiedTimestamp;
    Dirent->AccessTimestamp = File->LastAccessedTimestamp;
    Dirent->Create10ms      = File->Create10msIncrement;
    Dirent->Modify10ms      = File->LastModified10msIncrement;
    Dirent->CreateUtcOffset = File->CreateUtcOffset;
    Dirent->ModifyUtcOffset = File->LastModifiedUtcOffset;
    Dirent->AccessUtcOffset = File->LastAccessedUtcOffset;

    Dirent->StreamFlags     = Stream->GeneralSecondaryFlags;
    Dirent->NameLength      = Stream->NameLength;
    Dirent->NameHash        = Stream->NameHash;
    Dirent->FirstCluster    = Stream->FirstCluster;
    Dirent->DataLength      = Stream->DataLength;
    Dirent->ValidDataLength = Stream->ValidDataLength;

    return EXF_SET_OK;
}

BOOLEAN
ExfNamesEqual (
    const USHORT *Upcase,
    const WCHAR *Name1,
    ULONG Length1,
    const WCHAR *Name2,
    ULONG Length2
    )
{
    ULONG i;

    if (Length1 != Length2) {
        return FALSE;
    }

    for (i = 0; i < Length1; i++) {
        if (Upcase[(USHORT)Name1[i]] != Upcase[(USHORT)Name2[i]]) {
            return FALSE;
        }
    }

    return TRUE;
}

BOOLEAN
ExfIsLegalNameChar (
    WCHAR Char
    )
{
    if (Char < 0x20) {
        return FALSE;
    }

    switch (Char) {
    case '"':
    case '*':
    case '/':
    case ':':
    case '<':
    case '>':
    case '?':
    case '\\':
    case '|':
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
ExfDecodeTimestamp (
    ULONG Timestamp,
    UCHAR Increment10ms,
    PEXF_TIME_PARTS Parts
    )
{
    ULONG Extra;

    Parts->Year   = (USHORT)(1980 + ((Timestamp >> 25) & 0x7F));
    Parts->Month  = (USHORT)((Timestamp >> 21) & 0x0F);
    Parts->Day    = (USHORT)((Timestamp >> 16) & 0x1F);
    Parts->Hour   = (USHORT)((Timestamp >> 11) & 0x1F);
    Parts->Minute = (USHORT)((Timestamp >> 5) & 0x3F);
    Parts->Second = (USHORT)((Timestamp & 0x1F) * 2);
    Parts->Milliseconds = 0;

    if (Parts->Month < 1 || Parts->Month > 12 || Parts->Day < 1 ||
        Parts->Hour > 23 || Parts->Minute > 59 || Parts->Second > 58) {

        return FALSE;
    }

    /* 0..199 adds up to 1.99 seconds */
    if (Increment10ms < 200) {
        Extra = (ULONG)Increment10ms * 10;
        Parts->Second = (USHORT)(Parts->Second + Extra / 1000);
        Parts->Milliseconds = (USHORT)(Extra % 1000);
    }

    return TRUE;
}

BOOLEAN
ExfDecodeUtcOffset (
    UCHAR UtcOffset,
    LONG *Minutes
    )
{
    LONG Value;

    if (!(UtcOffset & EXFAT_UTC_OFFSET_VALID)) {
        return FALSE;
    }

    /* 7-bit two's complement, 15 minute units */
    Value = UtcOffset & 0x7F;
    if (Value & 0x40) {
        Value -= 0x80;
    }

    *Minutes = Value * 15;
    return TRUE;
}

/*
 * Years outside 1980..2107 are clamped to the nearest representable
 * moment. Returns FALSE for a date that does not exist.
 */
BOOLEAN
ExfEncodeTimestamp (
    const EXF_TIME_PARTS *Parts,
    ULONG *Timestamp,
    UCHAR *Increment10ms
    )
{
    if (Parts->Month < 1 || Parts->Month > 12 || Parts->Day < 1 || Parts->Day > 31 ||
        Parts->Hour > 23 || Parts->Minute > 59 || Parts->Second > 59 ||
        Parts->Milliseconds > 999) {

        return FALSE;
    }

    if (Parts->Year < 1980) {
        *Timestamp = (1UL << 21) | (1UL << 16);         /* 1980-01-01 00:00:00 */
        *Increment10ms = 0;
        return TRUE;
    }

    if (Parts->Year > 2107) {
        *Timestamp = (127UL << 25) | (12UL << 21) | (31UL << 16) |
                     (23UL << 11) | (59UL << 5) | 29;
        *Increment10ms = 199;
        return TRUE;
    }

    *Timestamp = ((ULONG)(Parts->Year - 1980) << 25) |
                 ((ULONG)Parts->Month << 21) |
                 ((ULONG)Parts->Day << 16) |
                 ((ULONG)Parts->Hour << 11) |
                 ((ULONG)Parts->Minute << 5) |
                 ((ULONG)Parts->Second / 2);

    *Increment10ms = (UCHAR)((Parts->Second & 1) * 100 + Parts->Milliseconds / 10);

    return TRUE;
}

UCHAR
ExfEncodeUtcOffset (
    LONG Minutes
    )
{
    LONG Units = Minutes / 15;

    if (Units < -64 || Units > 63) {
        return 0;
    }

    return (UCHAR)(EXFAT_UTC_OFFSET_VALID | (Units & 0x7F));
}

/* File + Stream + Name entries for a name of NameLength characters */
ULONG
ExfSetEntryCount (
    ULONG NameLength
    )
{
    return 2 + (NameLength + EXFAT_NAME_PER_ENTRY - 1) / EXFAT_NAME_PER_ENTRY;
}

/*
 * Writes the attribute, time and stream fields of Info into an existing
 * entry set and recomputes its checksum. The name entries are left alone.
 */
VOID
ExfStoreEntrySet (
    UCHAR *Set,
    ULONG EntryCount,
    const EXF_DIRENT *Info
    )
{
    EXFAT_FILE_ENTRY *File = (EXFAT_FILE_ENTRY *)Set;
    EXFAT_STREAM_ENTRY *Stream = (EXFAT_STREAM_ENTRY *)(Set + EXFAT_DIRENT_SIZE);

    File->FileAttributes            = Info->Attributes;
    File->CreateTimestamp           = Info->CreateTimestamp;
    File->LastModifiedTimestamp     = Info->ModifyTimestamp;
    File->LastAccessedTimestamp     = Info->AccessTimestamp;
    File->Create10msIncrement       = Info->Create10ms;
    File->LastModified10msIncrement = Info->Modify10ms;
    File->CreateUtcOffset           = Info->CreateUtcOffset;
    File->LastModifiedUtcOffset     = Info->ModifyUtcOffset;
    File->LastAccessedUtcOffset     = Info->AccessUtcOffset;

    Stream->GeneralSecondaryFlags   = Info->StreamFlags;
    Stream->FirstCluster            = Info->FirstCluster;
    Stream->DataLength              = Info->DataLength;
    Stream->ValidDataLength         = Info->ValidDataLength;

    File->SetChecksum = ExfSetChecksum(Set, EntryCount);
}

/*
 * Builds a complete entry set for Name into Set (room for
 * EXFAT_MAX_SET_ENTRIES entries) and returns its entry count.
 */
ULONG
ExfBuildEntrySet (
    const USHORT *Upcase,
    const WCHAR *Name,
    ULONG NameLength,
    const EXF_DIRENT *Info,
    UCHAR *Set
    )
{
    EXFAT_FILE_ENTRY *File = (EXFAT_FILE_ENTRY *)Set;
    EXFAT_STREAM_ENTRY *Stream = (EXFAT_STREAM_ENTRY *)(Set + EXFAT_DIRENT_SIZE);
    EXFAT_NAME_ENTRY *NameEntry;
    ULONG Count = ExfSetEntryCount(NameLength);
    ULONG Done = 0;
    ULONG Chunk;
    ULONG i;
    ULONG j;

    for (i = 0; i < Count * EXFAT_DIRENT_SIZE; i++) {
        Set[i] = 0;
    }

    File->EntryType = EXFAT_ENTRY_FILE;
    File->SecondaryCount = (UCHAR)(Count - 1);

    Stream->EntryType = EXFAT_ENTRY_STREAM;
    Stream->NameLength = (UCHAR)NameLength;
    Stream->NameHash = ExfNameHash(Upcase, Name, NameLength);

    for (i = 2; i < Count; i++) {

        NameEntry = (EXFAT_NAME_ENTRY *)(Set + i * EXFAT_DIRENT_SIZE);
        NameEntry->EntryType = EXFAT_ENTRY_NAME;

        Chunk = NameLength - Done;
        if (Chunk > EXFAT_NAME_PER_ENTRY) {
            Chunk = EXFAT_NAME_PER_ENTRY;
        }

        for (j = 0; j < Chunk; j++) {
            NameEntry->FileName[j] = Name[Done + j];
        }

        Done += Chunk;
    }

    ExfStoreEntrySet(Set, Count, Info);

    return Count;
}
