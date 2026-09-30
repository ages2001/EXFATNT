/*
 * EXFATNT - exFAT on-disk format (Microsoft exFAT specification 1.00)
 *
 * Needs only the basic NT types (UCHAR, USHORT, ULONG, ULONGLONG, WCHAR,
 * BOOLEAN), so the helpers in exfsup.c can also be built outside the
 * kernel.
 */

#ifndef _EXFDISK_H_
#define _EXFDISK_H_

/* ------------------------------------------------------------------ */
/* Boot region                                                         */
/* ------------------------------------------------------------------ */

#define EXFAT_BOOT_REGION_SECTORS   12      /* main and backup each */
#define EXFAT_BOOT_CHECKSUM_SECTOR  11
#define EXFAT_MIN_SECTOR_SHIFT      9
#define EXFAT_MAX_SECTOR_SHIFT      12
#define EXFAT_MAX_CLUSTER_SHIFT     25      /* 32 MB clusters */
#define EXFAT_MIN_FAT_OFFSET        24
#define EXFAT_MAX_CLUSTER_COUNT     0xFFFFFFF5UL

#define EXFAT_VOLUME_ACTIVE_FAT     0x0001
#define EXFAT_VOLUME_DIRTY          0x0002
#define EXFAT_VOLUME_MEDIA_FAILURE  0x0004
#define EXFAT_VOLUME_CLEAR_TO_ZERO  0x0008

#pragma pack(1)

typedef struct _EXFAT_BOOT_SECTOR {
    UCHAR       JumpBoot[3];
    UCHAR       FileSystemName[8];              /* "EXFAT   " */
    UCHAR       MustBeZero[53];
    ULONGLONG   PartitionOffset;
    ULONGLONG   VolumeLength;                   /* sectors */
    ULONG       FatOffset;                      /* sectors */
    ULONG       FatLength;                      /* sectors */
    ULONG       ClusterHeapOffset;              /* sectors */
    ULONG       ClusterCount;
    ULONG       FirstClusterOfRootDirectory;
    ULONG       VolumeSerialNumber;
    USHORT      FileSystemRevision;
    USHORT      VolumeFlags;
    UCHAR       BytesPerSectorShift;
    UCHAR       SectorsPerClusterShift;
    UCHAR       NumberOfFats;
    UCHAR       DriveSelect;
    UCHAR       PercentInUse;
    UCHAR       Reserved[7];
    UCHAR       BootCode[390];
    USHORT      BootSignature;                  /* 0xAA55 */
} EXFAT_BOOT_SECTOR, *PEXFAT_BOOT_SECTOR;

/* ------------------------------------------------------------------ */
/* FAT                                                                 */
/* ------------------------------------------------------------------ */

#define EXFAT_FIRST_CLUSTER         2
#define EXFAT_CLUSTER_BAD           0xFFFFFFF7UL
#define EXFAT_CLUSTER_END           0xFFFFFFFFUL

/* ------------------------------------------------------------------ */
/* Directory entries                                                   */
/* ------------------------------------------------------------------ */

#define EXFAT_DIRENT_SIZE           32
#define EXFAT_DIRENT_SHIFT          5

#define EXFAT_TYPE_IN_USE           0x80
#define EXFAT_TYPE_SECONDARY        0x40
#define EXFAT_TYPE_BENIGN           0x20

#define EXFAT_ENTRY_EOD             0x00
#define EXFAT_ENTRY_BITMAP          0x81
#define EXFAT_ENTRY_UPCASE          0x82
#define EXFAT_ENTRY_LABEL           0x83
#define EXFAT_ENTRY_FILE            0x85
#define EXFAT_ENTRY_GUID            0xA0
#define EXFAT_ENTRY_STREAM          0xC0
#define EXFAT_ENTRY_NAME            0xC1

#define EXFAT_ATTR_READONLY         0x0001
#define EXFAT_ATTR_HIDDEN           0x0002
#define EXFAT_ATTR_SYSTEM           0x0004
#define EXFAT_ATTR_DIRECTORY        0x0010
#define EXFAT_ATTR_ARCHIVE          0x0020
#define EXFAT_ATTR_VALID            0x0037

#define EXFAT_STREAM_ALLOC_POSSIBLE 0x01
#define EXFAT_STREAM_NO_FAT_CHAIN   0x02

#define EXFAT_MAX_NAME              255
#define EXFAT_NAME_PER_ENTRY        15
#define EXFAT_MIN_SECONDARY         2
#define EXFAT_MAX_SECONDARY         18
#define EXFAT_MAX_SET_ENTRIES       (EXFAT_MAX_SECONDARY + 1)
#define EXFAT_MAX_LABEL             11
#define EXFAT_MAX_DIR_SIZE          0x10000000UL   /* 256 MB */

#define EXFAT_UTC_OFFSET_VALID      0x80

typedef struct _EXFAT_FILE_ENTRY {
    UCHAR       EntryType;                      /* 0x85 */
    UCHAR       SecondaryCount;
    USHORT      SetChecksum;
    USHORT      FileAttributes;
    USHORT      Reserved1;
    ULONG       CreateTimestamp;
    ULONG       LastModifiedTimestamp;
    ULONG       LastAccessedTimestamp;
    UCHAR       Create10msIncrement;
    UCHAR       LastModified10msIncrement;
    UCHAR       CreateUtcOffset;
    UCHAR       LastModifiedUtcOffset;
    UCHAR       LastAccessedUtcOffset;
    UCHAR       Reserved2[7];
} EXFAT_FILE_ENTRY, *PEXFAT_FILE_ENTRY;

typedef struct _EXFAT_STREAM_ENTRY {
    UCHAR       EntryType;                      /* 0xC0 */
    UCHAR       GeneralSecondaryFlags;
    UCHAR       Reserved1;
    UCHAR       NameLength;
    USHORT      NameHash;
    USHORT      Reserved2;
    ULONGLONG   ValidDataLength;
    ULONG       Reserved3;
    ULONG       FirstCluster;
    ULONGLONG   DataLength;
} EXFAT_STREAM_ENTRY, *PEXFAT_STREAM_ENTRY;

typedef struct _EXFAT_NAME_ENTRY {
    UCHAR       EntryType;                      /* 0xC1 */
    UCHAR       GeneralSecondaryFlags;
    WCHAR       FileName[EXFAT_NAME_PER_ENTRY];
} EXFAT_NAME_ENTRY, *PEXFAT_NAME_ENTRY;

typedef struct _EXFAT_BITMAP_ENTRY {
    UCHAR       EntryType;                      /* 0x81 */
    UCHAR       BitmapFlags;                    /* bit 0: second bitmap */
    UCHAR       Reserved[18];
    ULONG       FirstCluster;
    ULONGLONG   DataLength;
} EXFAT_BITMAP_ENTRY, *PEXFAT_BITMAP_ENTRY;

typedef struct _EXFAT_UPCASE_ENTRY {
    UCHAR       EntryType;                      /* 0x82 */
    UCHAR       Reserved1[3];
    ULONG       TableChecksum;
    UCHAR       Reserved2[12];
    ULONG       FirstCluster;
    ULONGLONG   DataLength;
} EXFAT_UPCASE_ENTRY, *PEXFAT_UPCASE_ENTRY;

typedef struct _EXFAT_LABEL_ENTRY {
    UCHAR       EntryType;                      /* 0x83 */
    UCHAR       CharacterCount;
    WCHAR       VolumeLabel[EXFAT_MAX_LABEL];
    UCHAR       Reserved[8];
} EXFAT_LABEL_ENTRY, *PEXFAT_LABEL_ENTRY;

#pragma pack()

/* ------------------------------------------------------------------ */
/* Up-case table                                                       */
/* ------------------------------------------------------------------ */

#define EXFAT_UPCASE_CHARS          0x10000UL
#define EXFAT_UPCASE_MAX_BYTES      (EXFAT_UPCASE_CHARS * 2 * 2)

/* ------------------------------------------------------------------ */
/* Decoded directory entry set (exfsup.c)                              */
/* ------------------------------------------------------------------ */

typedef struct _EXF_DIRENT {
    ULONG       Offset;             /* File entry byte offset in its directory */
    ULONG       EntryCount;         /* SecondaryCount + 1 */
    USHORT      Attributes;
    UCHAR       StreamFlags;
    UCHAR       NameLength;         /* characters */
    USHORT      NameHash;
    USHORT      SetChecksum;
    ULONG       FirstCluster;
    ULONGLONG   DataLength;
    ULONGLONG   ValidDataLength;
    ULONG       CreateTimestamp;
    ULONG       ModifyTimestamp;
    ULONG       AccessTimestamp;
    UCHAR       Create10ms;
    UCHAR       Modify10ms;
    UCHAR       CreateUtcOffset;
    UCHAR       ModifyUtcOffset;
    UCHAR       AccessUtcOffset;
    WCHAR       Name[EXFAT_MAX_NAME];
} EXF_DIRENT, *PEXF_DIRENT;

typedef struct _EXF_TIME_PARTS {
    USHORT      Year;
    USHORT      Month;
    USHORT      Day;
    USHORT      Hour;
    USHORT      Minute;
    USHORT      Second;
    USHORT      Milliseconds;
} EXF_TIME_PARTS, *PEXF_TIME_PARTS;

/* ExfCheckBootSector results */
#define EXF_BOOT_OK                 0
#define EXF_BOOT_NOT_EXFAT          1
#define EXF_BOOT_BAD_GEOMETRY       2
#define EXF_BOOT_BAD_LAYOUT         3
#define EXF_BOOT_BAD_REVISION       4

/* ExfParseEntrySet results */
#define EXF_SET_OK                  0
#define EXF_SET_NEED_MORE           1
#define EXF_SET_BAD                 2

ULONG
ExfBootChecksum (
    const UCHAR *Region,
    ULONG BytesPerSector
    );

USHORT
ExfSetChecksum (
    const UCHAR *Set,
    ULONG EntryCount
    );

ULONG
ExfTableChecksum (
    const UCHAR *Data,
    ULONG Length
    );

USHORT
ExfNameHash (
    const USHORT *Upcase,
    const WCHAR *Name,
    ULONG Length
    );

VOID
ExfExpandUpcase (
    const USHORT *Source,
    ULONG SourceCount,
    USHORT *Table
    );

ULONG
ExfCheckBootSector (
    const EXFAT_BOOT_SECTOR *Boot,
    ULONG DeviceSectorSize,
    ULONGLONG DeviceSectors
    );

ULONG
ExfParseEntrySet (
    const UCHAR *Set,
    ULONG Available,
    PEXF_DIRENT Dirent
    );

BOOLEAN
ExfNamesEqual (
    const USHORT *Upcase,
    const WCHAR *Name1,
    ULONG Length1,
    const WCHAR *Name2,
    ULONG Length2
    );

BOOLEAN
ExfIsLegalNameChar (
    WCHAR Char
    );

BOOLEAN
ExfDecodeTimestamp (
    ULONG Timestamp,
    UCHAR Increment10ms,
    PEXF_TIME_PARTS Parts
    );

BOOLEAN
ExfDecodeUtcOffset (
    UCHAR UtcOffset,
    LONG *Minutes
    );

BOOLEAN
ExfEncodeTimestamp (
    const EXF_TIME_PARTS *Parts,
    ULONG *Timestamp,
    UCHAR *Increment10ms
    );

UCHAR
ExfEncodeUtcOffset (
    LONG Minutes
    );

ULONG
ExfSetEntryCount (
    ULONG NameLength
    );

ULONG
ExfBuildEntrySet (
    const USHORT *Upcase,
    const WCHAR *Name,
    ULONG NameLength,
    const EXF_DIRENT *Info,
    UCHAR *Set
    );

VOID
ExfStoreEntrySet (
    UCHAR *Set,
    ULONG EntryCount,
    const EXF_DIRENT *Info
    );

#endif /* _EXFDISK_H_ */
