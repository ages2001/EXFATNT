/*
 * EXFMT - exFAT format tool
 *
 * The layout and the writing of a new exFAT volume, free of any operating
 * system call: the front ends (exfmt.c for Windows, test\exfmtl.c for the
 * tests) supply the geometry and a routine that writes to the volume.
 */

#ifndef EXFMT_H
#define EXFMT_H

#ifdef _MSC_VER
typedef unsigned __int64 EXU64;
#else
typedef unsigned long long EXU64;
#endif
typedef unsigned long EXU32;
typedef unsigned short EXU16;
typedef unsigned char EXU8;

#define EXFMT_UPCASE_WORDS      2918
#define EXFMT_UPCASE_BYTES      (EXFMT_UPCASE_WORDS * 2)
#define EXFMT_UPCASE_CHECKSUM   0xE619D30DUL

#define EXFMT_MAX_LABEL         11
#define EXFMT_MAX_CLUSTERS      0xFFFFFFF5UL
#define EXFMT_MAX_CLUSTER_SIZE  (32UL * 1024 * 1024)
#define EXFMT_MIN_VOLUME        (1024UL * 1024)

/* Results */
#define EXFMT_OK                0
#define EXFMT_BAD_SECTOR_SIZE   1
#define EXFMT_BAD_CLUSTER_SIZE  2
#define EXFMT_TOO_SMALL         3
#define EXFMT_TOO_MANY_CLUSTERS 4
#define EXFMT_BAD_LABEL         5
#define EXFMT_WRITE_FAILED      6
#define EXFMT_NO_MEMORY         7
#define EXFMT_BAD_TABLE         8

/* What the front end knows about the volume */
typedef struct _EXFMT_PARAMS {
    EXU64 VolumeSectors;            /* sectors in the volume (partition) */
    EXU64 PartitionOffset;          /* sectors before it on the disk, for alignment */
    EXU32 SectorSize;               /* 512 to 4096 */
    EXU32 ClusterSize;              /* bytes; 0 picks the default for the size */
    EXU32 Serial;                   /* volume serial number */
    EXU16 Label[EXFMT_MAX_LABEL];   /* UTF-16 */
    EXU32 LabelLength;              /* characters, 0 for none */
    int Full;                       /* zero the whole cluster heap too */
} EXFMT_PARAMS;

/* The volume as it will be laid out; everything in sectors unless noted */
typedef struct _EXFMT_LAYOUT {
    EXU64 VolumeSectors;
    EXU64 PartitionOffset;
    EXU32 SectorSize;
    EXU32 ClusterSize;              /* bytes */
    EXU32 SectorShift;
    EXU32 ClusterShift;             /* sectors per cluster, as a shift */
    EXU32 FatOffset;
    EXU32 FatLength;
    EXU32 HeapOffset;
    EXU32 ClusterCount;
    EXU32 BitmapCluster;
    EXU32 BitmapClusters;
    EXU64 BitmapBytes;
    EXU32 UpcaseCluster;
    EXU32 UpcaseClusters;
    EXU32 RootCluster;
    EXU32 UsedClusters;
    EXU32 PercentInUse;
} EXFMT_LAYOUT;

/*
 * Writes Length bytes (a whole number of sectors) at byte Offset of the
 * volume; nonzero on success.
 */
typedef int (*EXFMT_WRITE)(void *Context, EXU64 Offset, const void *Buffer, EXU32 Length);

/* Called now and then while writing: Done out of Total */
typedef void (*EXFMT_PROGRESS)(void *Context, EXU64 Done, EXU64 Total);

extern const EXU16 ExfmtUpcase[EXFMT_UPCASE_WORDS];

EXU32 ExfmtDefaultClusterSize(EXU64 VolumeBytes, EXU32 SectorSize);
int ExfmtCheckLabel(const EXU16 *Label, EXU32 Length);
int ExfmtLayout(const EXFMT_PARAMS *Params, EXFMT_LAYOUT *Layout);
int ExfmtFormat(const EXFMT_PARAMS *Params, const EXFMT_LAYOUT *Layout,
                EXFMT_WRITE Write, EXFMT_PROGRESS Progress, void *Context);
const char *ExfmtMessage(int Result);

#endif
