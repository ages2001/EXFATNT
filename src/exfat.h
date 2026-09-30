/*
 * EXFATNT - exFAT file system driver for Windows NT 3.51, 4.0, 2000 and XP
 *
 *   EXF_NT4   Windows NT 3.51 and 4.0 (NT4 DDK + free ntifs.h, see src\NT\build.bat)
 *   (none)    Windows 2000 and XP, x86 and amd64 (WDK 6001, src\2KXP)
 */

#ifndef _EXFAT_H_
#define _EXFAT_H_

#include <ntifs.h>
#include <ntdddisk.h>

#include "exfdisk.h"

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

/*
 * The NT4 DDK IO_STACK_LOCATION lacks the file system members
 * (QueryDirectory, NotifyDirectory, FileSystemControl, LockControl);
 * the free ntifs.h provides them through EXTENDED_IO_STACK_LOCATION.
 */
#ifdef EXF_NT4
#define ExfXSp(IrpSp)           ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))
#define ExfMdlAddress(Mdl)      MmGetSystemAddressForMdl(Mdl)
#define EXF_STATUS_DISMOUNTED   STATUS_FILE_INVALID
#else
#define ExfXSp(IrpSp)           (IrpSp)
#define ExfMdlAddress(Mdl)      MmGetSystemAddressForMdlSafe((Mdl), NormalPagePriority)
#define EXF_STATUS_DISMOUNTED   STATUS_VOLUME_DISMOUNTED
#endif

/* Same as the W2K IoSkipCurrentIrpStackLocation, which NT4 lacks */
#define ExfSkipStack(Irp) {                         \
    (Irp)->CurrentLocation++;                       \
    (Irp)->Tail.Overlay.CurrentStackLocation++;     \
}

#define ExfCopyStackToNext(Irp) {                                           \
    PIO_STACK_LOCATION _Sp = IoGetCurrentIrpStackLocation(Irp);            \
    PIO_STACK_LOCATION _Next = IoGetNextIrpStackLocation(Irp);             \
    RtlCopyMemory(_Next, _Sp, FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine)); \
    _Next->Control = 0;                                                     \
}

/* ExReleaseResourceLite is not exported by every NT4 build */
#define ExfRelease(Resource) \
    ExReleaseResourceForThreadLite((Resource), ExGetCurrentResourceThread())

#ifndef VPB_LOCKED
#define VPB_LOCKED                  0x00000002
#endif

/* File system IRP details some DDK headers leave out */
#ifndef IRP_MN_QUERY_DIRECTORY
#define IRP_MN_QUERY_DIRECTORY          0x01
#endif
#ifndef IRP_MN_NOTIFY_CHANGE_DIRECTORY
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#endif
#ifndef IRP_MN_USER_FS_REQUEST
#define IRP_MN_USER_FS_REQUEST          0x00
#endif
#ifndef IRP_MN_MOUNT_VOLUME
#define IRP_MN_MOUNT_VOLUME             0x01
#endif
#ifndef IRP_MN_VERIFY_VOLUME
#define IRP_MN_VERIFY_VOLUME            0x02
#endif
#ifndef SL_RESTART_SCAN
#define SL_RESTART_SCAN                 0x01
#endif
#ifndef SL_RETURN_SINGLE_ENTRY
#define SL_RETURN_SINGLE_ENTRY          0x02
#endif
#ifndef SL_INDEX_SPECIFIED
#define SL_INDEX_SPECIFIED              0x04
#endif
#ifndef SL_WATCH_TREE
#define SL_WATCH_TREE                   0x01
#endif
#ifndef SL_OPEN_PAGING_FILE
#define SL_OPEN_PAGING_FILE             0x02
#endif
#ifndef SL_OPEN_TARGET_DIRECTORY
#define SL_OPEN_TARGET_DIRECTORY        0x04
#endif
#ifndef SL_OVERRIDE_VERIFY_VOLUME
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#endif

#ifndef IO_REMOUNT
#define IO_REMOUNT                  0x00000001
#endif

#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME       0x00080000
#endif

#ifndef FSCTL_IS_VOLUME_DIRTY
#define FSCTL_IS_VOLUME_DIRTY       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY             0x00000001
#endif

#ifndef FILE_WRITE_TO_END_OF_FILE
#define FILE_WRITE_TO_END_OF_FILE   0xffffffff
#endif

#ifndef IOCTL_DISK_IS_WRITABLE
#define IOCTL_DISK_IS_WRITABLE      CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Status codes newer than some DDK headers */
#ifndef STATUS_USER_MAPPED_FILE
#define STATUS_USER_MAPPED_FILE     ((NTSTATUS)0xC0000243L)
#endif
#ifndef STATUS_CANNOT_MAKE
#define STATUS_CANNOT_MAKE          ((NTSTATUS)0xC000016BL)
#endif

/* Information classes newer than NT4, answered on both targets */
#define EXF_CLASS_ATTRIBUTE_TAG             35
#define EXF_CLASS_FS_FULL_SIZE              7

typedef struct _EXF_FILE_ATTRIBUTE_TAG_INFORMATION {
    ULONG       FileAttributes;
    ULONG       ReparseTag;
} EXF_FILE_ATTRIBUTE_TAG_INFORMATION, *PEXF_FILE_ATTRIBUTE_TAG_INFORMATION;

typedef struct _EXF_FILE_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG       SectorsPerAllocationUnit;
    ULONG       BytesPerSector;
} EXF_FILE_FS_FULL_SIZE_INFORMATION, *PEXF_FILE_FS_FULL_SIZE_INFORMATION;

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

/* EXF_DBG((EXF_PFX "Cluster %lu\n", Cluster)); -- VC++ 4.0 has no
   variadic macros. */
#define EXF_PFX                 "[EXFATNT] "

#if DBG || defined(EXF_DEBUG)
#define EXF_DBG(args)           DbgPrint args
#else
#define EXF_DBG(args)
#endif

/* ------------------------------------------------------------------ */
/* Limits and tags                                                     */
/* ------------------------------------------------------------------ */

#define EXF_FS_DEVICE_NAME      L"\\ExfatNt"
#define EXF_FS_NAME             L"exFAT"

#define EXF_TAG                 'tafE'
#define EXF_TAG_FCB             'FfxE'
#define EXF_TAG_CCB             'CfxE'
#define EXF_TAG_NAME            'NfxE'
#define EXF_TAG_RUNS            'RfxE'
#define EXF_TAG_UPCASE          'UfxE'
#define EXF_TAG_BUFFER          'BfxE'
#define EXF_TAG_WORK            'WfxE'
#define EXF_TAG_REGISTRY        'GfxE'

#define EXF_MAP_UNIT            0x1000      /* CcMapData / CcPinRead granule */
#define EXF_PAGE_SIZE           0x1000      /* x86 and amd64 */
#define EXF_ZERO_CHUNK          0x10000     /* zero buffer for disk writes */

/* ------------------------------------------------------------------ */
/* Node types                                                          */
/* ------------------------------------------------------------------ */

#define EXF_NTC_VCB             ((CSHORT)0x0E01)
#define EXF_NTC_FCB             ((CSHORT)0x0E02)    /* file */
#define EXF_NTC_DCB             ((CSHORT)0x0E03)    /* directory */
#define EXF_NTC_VFCB            ((CSHORT)0x0E04)    /* whole volume (DASD) */
#define EXF_NTC_CCB             ((CSHORT)0x0E05)
#define EXF_NTC_META            ((CSHORT)0x0E06)    /* FAT or allocation bitmap */

#define ExfNodeType(Ptr)        (*((CSHORT *)(Ptr)))

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

/*
 * Locking, in acquisition order:
 *
 *   Vcb->Resource        exclusive for anything that adds, removes or
 *                        rewrites entry sets or FCBs: create, cleanup,
 *                        close, every set information class, label,
 *                        dismount. Shared for reads, writes and listings.
 *   Fcb->Resource        the stream's sizes and contents; exclusive to
 *                        change them. A directory's own resource also
 *                        guards its entries while they are listed.
 *   Fcb->PagingIoResource exclusive while the run list changes or the
 *                        file shrinks; paging I/O takes it shared.
 *   Vcb->AllocResource   the allocation bitmap, the FAT, FreeClusters,
 *                        AllocHint and the volume dirty bit.
 *
 * Metadata is cached per object, never through one volume-wide stream: a
 * page of a volume stream could hold file data next to a directory
 * cluster, and writing that page back would put stale data on the disk.
 * So the FAT, the bitmap and every directory get their own stream file,
 * addressed by offset within the object.
 */

typedef struct _EXF_VCB EXF_VCB, *PEXF_VCB;

/* Cluster run: Count clusters of the stream starting at Vcn map to Lcn.. */
typedef struct _EXF_RUN {
    ULONG       Vcn;
    ULONG       Lcn;
    ULONG       Count;
} EXF_RUN, *PEXF_RUN;

typedef struct _EXF_RUN_LIST {
    PEXF_RUN    Runs;
    ULONG       RunCount;
    ULONG       RunMax;
    ULONG       Clusters;
} EXF_RUN_LIST, *PEXF_RUN_LIST;

/* Allocated from nonpaged pool: holds ERESOURCEs and the FsRtl header */
typedef struct _EXF_FCB {
    FSRTL_COMMON_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE   Resource;
    ERESOURCE   PagingIoResource;
    PEXF_VCB    Vcb;
    struct _EXF_FCB *ParentDcb;
    LIST_ENTRY  FcbLinks;
    ULONG       RefCount;           /* file objects + child FCBs */
    ULONG       UncleanCount;       /* handles not yet cleaned up */
    ULONG       NonCachedUncleanCount;
    ULONG       FcbState;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK   FileLock;
    ULONG       FileLockSpare[4];   /* in case an older FILE_LOCK is larger */
    PFILE_OBJECT StreamFile;        /* internal cached stream (directory, FAT, bitmap) */
    PKTHREAD    LazyWriteThread;
    LONGLONG    MetaLbo;            /* FAT: volume offset, mapped linearly */
    ULONG       DirOffset;          /* File entry offset in parent */
    ULONG       EntryCount;
    USHORT      Attributes;         /* exFAT attributes */
    USHORT      NameHash;
    UCHAR       StreamFlags;
    ULONG       FirstCluster;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER LastAccessTime;
    LONGLONG    IndexNumber;
    UNICODE_STRING Name;            /* last component, as on disk */
    UNICODE_STRING FullName;        /* built on demand */
    EXF_RUN_LIST RunList;
} EXF_FCB, *PEXF_FCB;

#define FCB_STATE_ROOT              0x0001
#define FCB_STATE_VISITED           0x0002  /* list walk marker */
#define FCB_STATE_DELETE_PENDING    0x0004
#define FCB_STATE_DELETED           0x0008  /* off the disk, kept for Cc and Mm */
#define FCB_STATE_DIRENT_DIRTY      0x0010  /* entry set lags behind the FCB */
#define FCB_STATE_TRUNCATE_ON_CLOSE 0x0020  /* allocation beyond the file size */
#define FCB_STATE_STREAM_OPEN       0x0040  /* internal stream not closed yet */
#define FCB_STATE_TEARDOWN          0x0080  /* freed when its stream closes */

typedef struct _EXF_CCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ULONG       Flags;
    ULONG       QueryState;         /* 0: ".", 1: "..", 2: entries */
    ULONG       QueryOffset;        /* next directory offset to scan */
    UNICODE_STRING Pattern;         /* upcased */
} EXF_CCB, *PEXF_CCB;

#define CCB_FLAG_PATTERN_SET        0x0001
#define CCB_FLAG_WILDCARD           0x0002
#define CCB_FLAG_MATCH_ALL          0x0004
#define CCB_FLAG_VOLUME_OPEN        0x0008
#define CCB_FLAG_DISMOUNTED_VOLUME  0x0010      /* this handle dismounted it */
#define CCB_FLAG_DELETE_ON_CLOSE    0x0020
#define CCB_FLAG_USER_SET_WRITE     0x0040      /* keep the caller's write time */
#define CCB_FLAG_USER_SET_ACCESS    0x0080
#define CCB_FLAG_WRITE_HANDLE       0x0100      /* counted in Vcb->WriteCount */

/* Lives in the volume device object's extension */
struct _EXF_VCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ERESOURCE   Resource;
    ERESOURCE   AllocResource;
    LIST_ENTRY  VcbLinks;
    PVPB        Vpb;
    PDEVICE_OBJECT TargetDeviceObject;
    PDEVICE_OBJECT VolumeDeviceObject;
    ULONG       VcbState;
    ULONG       OpenCount;          /* all our file objects, internal too */
    ULONG       UncleanCount;       /* user handles */
    ULONG       WriteCount;         /* user handles that may change the volume */
    PFILE_OBJECT LockFileObject;
    PKTHREAD    VerifyThread;       /* mount/verify in progress */
    PEXF_FCB    VolumeFcb;
    PEXF_FCB    RootDcb;
    PEXF_FCB    FatFcb;
    PEXF_FCB    BitmapFcb;
    LIST_ENTRY  FcbList;

    /* Geometry and layout */
    ULONG       SectorSize;
    ULONG       SectorShift;
    ULONG       ClusterSize;
    ULONG       ClusterShift;
    ULONG       SectorsPerClusterShift;
    LONGLONG    VolumeBytes;
    LONGLONG    PartitionBytes;
    ULONG       FatSector;          /* first sector of the active FAT */
    ULONG       FatLength;
    ULONG       ClusterHeapSector;
    ULONG       ClusterCount;
    ULONG       RootCluster;
    ULONG       SerialNumber;
    USHORT      Revision;
    USHORT      VolumeFlags;        /* as last written to the boot sector */
    UCHAR       NumberOfFats;
    BOOLEAN     DirtyMarked;        /* we set VolumeDirty on the disk */
    ULONG       BitmapCluster;
    ULONGLONG   BitmapLength;

    /* Allocation, under AllocResource */
    ULONG       FreeClusters;
    ULONG       AllocHint;

    PUSHORT     Upcase;             /* 64K entries */
    ULONG       LabelLength;        /* characters */
    WCHAR       Label[EXFAT_MAX_LABEL];

    /* Directory change notification */
    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY  DirNotifyList;
};

#define VCB_STATE_MOUNTED           0x0001
#define VCB_STATE_LOCKED            0x0002
#define VCB_STATE_DISMOUNTED        0x0004
#define VCB_STATE_IN_DISMOUNT       0x0008
#define VCB_STATE_DELETE_PENDING    0x0010
#define VCB_STATE_DASD_WRITTEN      0x0020
#define VCB_STATE_REMOVABLE         0x0040
#define VCB_STATE_PNP_LOCKED        0x0080
#define VCB_STATE_FREE_VPB          0x0100
#define VCB_STATE_REMOVED           0x0200
#define VCB_STATE_READ_ONLY         0x0400  /* write protected or TexFAT */
#define VCB_STATE_KEEP_DIRTY        0x0800  /* dirty at mount or by request */
#define VCB_STATE_FLUSH_ON_CLOSE    0x1000  /* removable or hot-plug media */
#define VCB_STATE_SHUTDOWN          0x2000

typedef struct _EXF_DATA {
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    ERESOURCE       Resource;       /* protects VcbList */
    LIST_ENTRY      VcbList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    CACHE_MANAGER_CALLBACKS MetaCacheCallbacks;
    UNICODE_STRING  RegistryPath;   /* the service key, NUL-terminated */
} EXF_DATA, *PEXF_DATA;

extern EXF_DATA ExfData;

/* Per-request context, lives on the dispatch routine's stack */
typedef struct _EXF_IRP_CONTEXT {
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PEXF_VCB        Vcb;            /* NULL for the file system device */
    ULONG           Flags;
    NTSTATUS        ExceptionStatus;
} EXF_IRP_CONTEXT, *PEXF_IRP_CONTEXT;

#define EXF_CTX_TOP_LEVEL           0x0001
#define EXF_CTX_NO_COMPLETE         0x0002  /* IRP completed, pended or passed down */

/*
 * Mapped piece of a metadata stream (FAT, bitmap or directory), by offset
 * within the stream. While mounting, before the streams exist, a buffer
 * read straight from the disk instead.
 */
typedef struct _EXF_MAP {
    PEXF_FCB    Fcb;
    PVOID       Bcb;
    PUCHAR      Data;
    PUCHAR      Direct;
    LONGLONG    Vbo;
    ULONG       Length;
    BOOLEAN     Pinned;
    BOOLEAN     Dirty;
} EXF_MAP, *PEXF_MAP;

/* Directory scan state */
typedef struct _EXF_SCAN {
    EXF_MAP     Map;
    EXF_DIRENT  Dirent;
    UCHAR       Set[EXFAT_MAX_SET_ENTRIES * EXFAT_DIRENT_SIZE];
} EXF_SCAN, *PEXF_SCAN;

/* A close that could not take the VCB, finished by a worker thread */
typedef struct _EXF_CLOSE_ITEM {
    WORK_QUEUE_ITEM Item;
    PEXF_VCB    Vcb;
    PEXF_FCB    Fcb;
    PEXF_CCB    Ccb;
} EXF_CLOSE_ITEM, *PEXF_CLOSE_ITEM;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define ExfIsFcb(Fcb)       ((Fcb)->Header.NodeTypeCode == EXF_NTC_FCB)
#define ExfIsDcb(Fcb)       ((Fcb)->Header.NodeTypeCode == EXF_NTC_DCB)
#define ExfIsVfcb(Fcb)      ((Fcb)->Header.NodeTypeCode == EXF_NTC_VFCB)
#define ExfIsMeta(Fcb)      ((Fcb)->Header.NodeTypeCode == EXF_NTC_META)

#define ExfClusterToLbo(Vcb, Cluster) \
    (((LONGLONG)(Vcb)->ClusterHeapSector << (Vcb)->SectorShift) + \
     ((LONGLONG)((Cluster) - EXFAT_FIRST_CLUSTER) << (Vcb)->ClusterShift))

#define ExfRoundUp(Value, Size) \
    (((Value) + ((Size) - 1)) & ~((LONGLONG)(Size) - 1))

#define ExfSetDirty(Map)    ((Map)->Dirty = TRUE)

/* Access bits that would change the volume */
#define EXF_WRITE_ACCESS    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | \
                             FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD)

/* Attribute bits a caller may set */
#define EXF_SETTABLE_ATTRIBUTES (EXFAT_ATTR_READONLY | EXFAT_ATTR_HIDDEN | \
                                 EXFAT_ATTR_SYSTEM | EXFAT_ATTR_ARCHIVE)

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

/* exfinit.c */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
BOOLEAN ExfWriteSupportEnabled(VOID);

/* exfdisp.c */
NTSTATUS NTAPI ExfFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS ExfVerifyVcb(PEXF_IRP_CONTEXT Ctx, PEXF_VCB Vcb);
NTSTATUS ExfVerifyWritable(PEXF_IRP_CONTEXT Ctx, PEXF_VCB Vcb);

/* exfcreat.c */
NTSTATUS ExfCommonCreate(PEXF_IRP_CONTEXT Ctx);

/* exfclose.c */
NTSTATUS ExfCommonCleanup(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfCommonClose(PEXF_IRP_CONTEXT Ctx);
BOOLEAN  ExfTryTeardown(PEXF_VCB Vcb, ULONG InFlightReferences);
VOID     ExfDeleteVcb(PEXF_VCB Vcb);
BOOLEAN  ExfDismountVcb(PEXF_VCB Vcb);
VOID     ExfUnlockVcb(PEXF_VCB Vcb);
VOID     ExfPurgeCachedFiles(PEXF_VCB Vcb);

/* exfread.c */
NTSTATUS ExfCommonRead(PEXF_IRP_CONTEXT Ctx);

/* exfwrite.c */
NTSTATUS ExfCommonWrite(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfZeroFileRange(PEXF_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End);

/* exfdir.c */
NTSTATUS ExfCommonDirectoryControl(PEXF_IRP_CONTEXT Ctx);
BOOLEAN  ExfNextFileEntry(PEXF_VCB Vcb, PEXF_FCB Dcb, PULONG Offset,
                          const USHORT *WantHash, PEXF_SCAN Scan);
NTSTATUS ExfLookupName(PEXF_VCB Vcb, PEXF_FCB Dcb, PUNICODE_STRING Name, PEXF_SCAN Scan);
PEXF_SCAN ExfAllocateScan(VOID);
VOID     ExfFreeScan(PEXF_SCAN Scan);

/* exfdirw.c */
NTSTATUS ExfCreateDirent(PEXF_VCB Vcb, PEXF_FCB Dcb, PUNICODE_STRING Name,
                         USHORT Attributes, PEXF_DIRENT Dirent);
NTSTATUS ExfUpdateDirent(PEXF_VCB Vcb, PEXF_FCB Fcb);
NTSTATUS ExfRemoveDirent(PEXF_VCB Vcb, PEXF_FCB Dcb, ULONG Offset, ULONG Count);
NTSTATUS ExfMoveDirent(PEXF_VCB Vcb, PEXF_FCB Fcb, PEXF_FCB TargetDcb, PUNICODE_STRING Name);
NTSTATUS ExfIsDirectoryEmpty(PEXF_VCB Vcb, PEXF_FCB Dcb, PBOOLEAN Empty);
NTSTATUS ExfWriteLabel(PEXF_VCB Vcb, const WCHAR *Label, ULONG Length);
NTSTATUS ExfDeleteFromDisk(PEXF_VCB Vcb, PEXF_FCB Fcb);
VOID     ExfNotifyChange(PEXF_VCB Vcb, PEXF_FCB Fcb, ULONG Filter, ULONG Action);

/* exfinfo.c */
NTSTATUS ExfCommonQueryInformation(PEXF_IRP_CONTEXT Ctx);
ULONG    ExfNtAttributes(PEXF_FCB Fcb);
ULONG    ExfDirentNtAttributes(USHORT Attributes);
VOID     ExfFillBasicInfo(PEXF_FCB Fcb, PFILE_BASIC_INFORMATION Info);
VOID     ExfFillStandardInfo(PEXF_FCB Fcb, PFILE_STANDARD_INFORMATION Info);
VOID     ExfFillNetworkOpenInfo(PEXF_FCB Fcb, PFILE_NETWORK_OPEN_INFORMATION Info);

/* exfsetin.c */
NTSTATUS ExfCommonSetInformation(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfSetFileSize(PEXF_IRP_CONTEXT Ctx, PEXF_FCB Fcb, PFILE_OBJECT FileObject,
                        LONGLONG NewSize);

/* exfvol.c */
NTSTATUS ExfCommonQueryVolumeInformation(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfCommonSetVolumeInformation(PEXF_IRP_CONTEXT Ctx);

/* exffsctl.c */
NTSTATUS ExfCommonFileSystemControl(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfCommonPnp(PEXF_IRP_CONTEXT Ctx);

/* exfmisc.c */
NTSTATUS ExfCommonDeviceControl(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfCommonLockControl(PEXF_IRP_CONTEXT Ctx);

/* exfflush.c */
NTSTATUS ExfCommonFlushBuffers(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfCommonShutdown(PEXF_IRP_CONTEXT Ctx);
NTSTATUS ExfFlushFile(PEXF_VCB Vcb, PEXF_FCB Fcb);
NTSTATUS ExfFlushMetadata(PEXF_VCB Vcb);
NTSTATUS ExfFlushVolume(PEXF_VCB Vcb, BOOLEAN MarkClean);
VOID     ExfMarkVolumeDirty(PEXF_VCB Vcb);
NTSTATUS ExfMarkVolumeClean(PEXF_VCB Vcb);

/* exffast.c */
VOID     ExfInitializeFastIo(PFAST_IO_DISPATCH FastIo);
VOID     ExfInitializeCacheCallbacks(VOID);
UCHAR    ExfIsFastIoPossible(PEXF_FCB Fcb);

/* exfalloc.c */
NTSTATUS ExfBuildRunList(PEXF_VCB Vcb, ULONG FirstCluster, BOOLEAN NoFatChain,
                         ULONG Clusters, PEXF_RUN_LIST RunList);
VOID     ExfFreeRunList(PEXF_RUN_LIST RunList);
BOOLEAN  ExfLookupVbo(PEXF_VCB Vcb, PEXF_RUN_LIST RunList, LONGLONG Vbo,
                      PLONGLONG Lbo, PULONG Contiguous);
NTSTATUS ExfBuildMetadataRunList(PEXF_VCB Vcb, ULONG FirstCluster, ULONGLONG Length,
                                 PEXF_RUN_LIST RunList);
NTSTATUS ExfCountFreeClusters(PEXF_VCB Vcb);
NTSTATUS ExfAllocateClusters(PEXF_VCB Vcb, PEXF_RUN_LIST RunList, PUCHAR StreamFlags,
                             ULONG Clusters);
VOID     ExfFreeClusters(PEXF_VCB Vcb, PEXF_RUN_LIST RunList, PUCHAR StreamFlags,
                         ULONG Keep);
NTSTATUS ExfSetAllocation(PEXF_VCB Vcb, PEXF_FCB Fcb, ULONGLONG Bytes);

/* exfio.c */
PUCHAR   ExfMapStream(PEXF_VCB Vcb, PEXF_FCB Fcb, PEXF_MAP Map, LONGLONG Vbo, ULONG Length);
PUCHAR   ExfPinStream(PEXF_VCB Vcb, PEXF_FCB Fcb, PEXF_MAP Map, LONGLONG Vbo, ULONG Length);
VOID     ExfUnmap(PEXF_MAP Map);
VOID     ExfZeroStream(PEXF_VCB Vcb, PEXF_FCB Fcb, LONGLONG Vbo, ULONG Length);
BOOLEAN  ExfStreamToLbo(PEXF_VCB Vcb, PEXF_FCB Fcb, LONGLONG Vbo, PLONGLONG Lbo,
                        PULONG Contiguous);
NTSTATUS ExfSyncIo(PDEVICE_OBJECT Device, UCHAR MajorFunction, LONGLONG Offset,
                   ULONG Length, PVOID Buffer, BOOLEAN OverrideVerify);
NTSTATUS ExfDeviceIoctl(PDEVICE_OBJECT Device, ULONG IoControlCode, PVOID InputBuffer,
                        ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                        BOOLEAN OverrideVerify, PULONG Information);
NTSTATUS ExfFlushDevice(PEXF_VCB Vcb);
VOID     ExfLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG Length);
PVOID    ExfMapUserBuffer(PIRP Irp);
NTSTATUS ExfNonCachedIo(PEXF_IRP_CONTEXT Ctx, PEXF_FCB Fcb, UCHAR MajorFunction,
                        LONGLONG StartingVbo, ULONG ByteCount, LONGLONG ValidData);
NTSTATUS ExfZeroDisk(PEXF_VCB Vcb, PEXF_FCB Fcb, LONGLONG Start, LONGLONG End);

#define ExfReadSectors(Device, Offset, Length, Buffer, Override) \
    ExfSyncIo((Device), IRP_MJ_READ, (Offset), (Length), (Buffer), (Override))
#define ExfWriteSectors(Device, Offset, Length, Buffer, Override) \
    ExfSyncIo((Device), IRP_MJ_WRITE, (Offset), (Length), (Buffer), (Override))

/* exfstruc.c */
PEXF_FCB ExfCreateVolumeFcb(PEXF_VCB Vcb);
PEXF_FCB ExfCreateRootDcb(PEXF_VCB Vcb);
PEXF_FCB ExfCreateMetaFcb(PEXF_VCB Vcb, LONGLONG Lbo, ULONG FirstCluster, ULONGLONG Length);
NTSTATUS ExfCreateFcb(PEXF_VCB Vcb, PEXF_FCB ParentDcb, PEXF_DIRENT Dirent, PEXF_FCB *Fcb);
VOID     ExfDeleteFcb(PEXF_FCB Fcb);
VOID     ExfDereferenceFcb(PEXF_VCB Vcb, PEXF_FCB Fcb);
PEXF_FCB ExfFindFcb(PEXF_VCB Vcb, PEXF_FCB ParentDcb, PUNICODE_STRING Name);
NTSTATUS ExfOpenStream(PEXF_VCB Vcb, PEXF_FCB Fcb);
VOID     ExfCloseStream(PEXF_VCB Vcb, PEXF_FCB Fcb, BOOLEAN Discard);
VOID     ExfStreamClosed(PEXF_VCB Vcb, PEXF_FCB Fcb);
PEXF_CCB ExfCreateCcb(VOID);
VOID     ExfDeleteCcb(PEXF_CCB Ccb);
NTSTATUS ExfBuildFullName(PEXF_FCB Fcb);
VOID     ExfForgetNames(PEXF_VCB Vcb, PEXF_FCB Fcb);
BOOLEAN  ExfIsAncestor(PEXF_FCB Ancestor, PEXF_FCB Fcb);
LARGE_INTEGER ExfConvertTime(ULONG Timestamp, UCHAR Increment10ms, UCHAR UtcOffset);
VOID     ExfEncodeTime(LARGE_INTEGER Time, PULONG Timestamp, PUCHAR Increment10ms,
                       PUCHAR UtcOffset);
VOID     ExfFcbToDirent(PEXF_FCB Fcb, PEXF_DIRENT Dirent);

#endif /* _EXFAT_H_ */
