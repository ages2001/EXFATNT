/*
 * EXFMT - layout and writing of a new exFAT volume
 *
 * The volume follows the exFAT specification and what Windows writes:
 * boot region and its backup, one FAT aligned to the cluster size (and to
 * 1 MB on volumes of 32 MB and more, counted from the start of the disk),
 * then the cluster heap: allocation bitmap, up-case table and one cluster
 * of root directory, each FAT-chained. Only the metadata is written unless
 * a full format is asked for.
 */

#include <stdlib.h>
#include <string.h>
#include "exfmt.h"

#define CHUNK (1024UL * 1024)

/* The regions WriteRegion fills */
#define REGION_ZERO     0
#define REGION_FAT      1
#define REGION_BITMAP   2
#define REGION_UPCASE   3
#define REGION_ROOT     4

typedef struct _WRITER {
    const EXFMT_PARAMS *Params;
    const EXFMT_LAYOUT *Layout;
    EXFMT_WRITE Write;
    EXFMT_PROGRESS Progress;
    void *Context;
    EXU8 *Buffer;
    EXU64 Done;
    EXU64 Total;
} WRITER;

static void Put16(EXU8 *p, EXU32 v) { p[0] = (EXU8)v; p[1] = (EXU8)(v >> 8); }
static void Put32(EXU8 *p, EXU32 v) { Put16(p, v & 0xFFFF); Put16(p + 2, v >> 16); }
static void Put64(EXU8 *p, EXU64 v) { Put32(p, (EXU32)(v & 0xFFFFFFFFUL)); Put32(p + 4, (EXU32)(v >> 32)); }

static EXU32 Shift(EXU32 Value)
{
    EXU32 s = 0;
    while ((1UL << s) < Value) {
        s++;
    }
    return s;
}

static int PowerOfTwo(EXU32 Value)
{
    return Value != 0 && (Value & (Value - 1)) == 0;
}

/* Rounds Sector up so that PartitionOffset + Sector is a multiple of Unit */
static EXU64 AlignUp(EXU64 Sector, EXU64 PartitionOffset, EXU64 Unit)
{
    EXU64 Absolute = PartitionOffset + Sector;
    return (Absolute + Unit - 1) / Unit * Unit - PartitionOffset;
}

EXU32 ExfmtDefaultClusterSize(EXU64 VolumeBytes, EXU32 SectorSize)
{
    EXU32 Size;

    if (VolumeBytes < (EXU64)256 * 1024 * 1024) {
        Size = 4096;
    } else if (VolumeBytes < (EXU64)32 * 1024 * 1024 * 1024) {
        Size = 32768;
    } else {
        Size = 131072;
    }

    return Size < SectorSize ? SectorSize : Size;
}

/* The characters a label may not hold are those a file name may not */
int ExfmtCheckLabel(const EXU16 *Label, EXU32 Length)
{
    EXU32 i;

    if (Length > EXFMT_MAX_LABEL) {
        return EXFMT_BAD_LABEL;
    }

    for (i = 0; i < Length; i++) {
        EXU16 c = Label[i];
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|') {
            return EXFMT_BAD_LABEL;
        }
    }

    return EXFMT_OK;
}

int ExfmtLayout(const EXFMT_PARAMS *Params, EXFMT_LAYOUT *Layout)
{
    EXU64 Bytes = Params->VolumeSectors * Params->SectorSize;
    EXU64 Unit, FatOffset, FatLength, HeapOffset, Count;
    EXU32 SectorsPerCluster;
    int Result;

    memset(Layout, 0, sizeof(*Layout));

    if (!PowerOfTwo(Params->SectorSize) || Params->SectorSize < 512 || Params->SectorSize > 4096) {
        return EXFMT_BAD_SECTOR_SIZE;
    }

    Result = ExfmtCheckLabel(Params->Label, Params->LabelLength);
    if (Result != EXFMT_OK) {
        return Result;
    }

    if (Bytes < EXFMT_MIN_VOLUME) {
        return EXFMT_TOO_SMALL;
    }

    Layout->ClusterSize = Params->ClusterSize ? Params->ClusterSize :
                          ExfmtDefaultClusterSize(Bytes, Params->SectorSize);

    if (!PowerOfTwo(Layout->ClusterSize) || Layout->ClusterSize < Params->SectorSize ||
        Layout->ClusterSize > EXFMT_MAX_CLUSTER_SIZE) {
        return EXFMT_BAD_CLUSTER_SIZE;
    }

    Layout->VolumeSectors = Params->VolumeSectors;
    Layout->PartitionOffset = Params->PartitionOffset;
    Layout->SectorSize = Params->SectorSize;
    Layout->SectorShift = Shift(Params->SectorSize);
    Layout->ClusterShift = Shift(Layout->ClusterSize) - Layout->SectorShift;
    SectorsPerCluster = 1UL << Layout->ClusterShift;

    /* Alignment unit, in sectors */
    Unit = Layout->ClusterSize;
    if (Bytes >= (EXU64)32 * 1024 * 1024 && Unit < 1024 * 1024) {
        Unit = 1024 * 1024;
    }
    Unit /= Params->SectorSize;

    /* The FAT follows the two boot regions */
    FatOffset = AlignUp(24, Params->PartitionOffset, Unit);

    /* Sized for every cluster the rest could hold, so never too small */
    if (FatOffset >= Params->VolumeSectors) {
        return EXFMT_TOO_SMALL;
    }
    Count = (Params->VolumeSectors - FatOffset) / SectorsPerCluster;
    if (Count > EXFMT_MAX_CLUSTERS) {
        Count = EXFMT_MAX_CLUSTERS;
    }
    FatLength = ((Count + 2) * 4 + Params->SectorSize - 1) / Params->SectorSize;

    HeapOffset = AlignUp(FatOffset + FatLength, Params->PartitionOffset, Unit);
    if (HeapOffset >= Params->VolumeSectors || HeapOffset > 0xFFFFFFFFUL) {
        return EXFMT_TOO_SMALL;
    }

    Count = (Params->VolumeSectors - HeapOffset) / SectorsPerCluster;
    if (Count > EXFMT_MAX_CLUSTERS) {
        return EXFMT_TOO_MANY_CLUSTERS;
    }

    Layout->FatOffset = (EXU32)FatOffset;
    Layout->FatLength = (EXU32)FatLength;
    Layout->HeapOffset = (EXU32)HeapOffset;
    Layout->ClusterCount = (EXU32)Count;

    /* Bitmap, up-case table and root directory, one after the other */
    Layout->BitmapBytes = (Count + 7) / 8;
    Layout->BitmapCluster = 2;
    Layout->BitmapClusters = (EXU32)((Layout->BitmapBytes + Layout->ClusterSize - 1) / Layout->ClusterSize);
    Layout->UpcaseCluster = Layout->BitmapCluster + Layout->BitmapClusters;
    Layout->UpcaseClusters = (EXFMT_UPCASE_BYTES + Layout->ClusterSize - 1) / Layout->ClusterSize;
    Layout->RootCluster = Layout->UpcaseCluster + Layout->UpcaseClusters;
    Layout->UsedClusters = Layout->BitmapClusters + Layout->UpcaseClusters + 1;

    if (Layout->UsedClusters >= Count) {
        return EXFMT_TOO_SMALL;
    }

    Layout->PercentInUse = (EXU32)((EXU64)Layout->UsedClusters * 100 / Count);

    return EXFMT_OK;
}

/* The checksum of the up-case table as it is stored (little-endian) */
static EXU32 UpcaseChecksum(void)
{
    EXU32 Checksum = 0;
    EXU32 i;

    for (i = 0; i < EXFMT_UPCASE_BYTES; i++) {
        EXU32 Byte = (i & 1) ? (ExfmtUpcase[i / 2] >> 8) : (ExfmtUpcase[i / 2] & 0xFF);
        Checksum = ((Checksum & 1) ? 0x80000000UL : 0) + (Checksum >> 1) + Byte;
    }

    return Checksum;
}

static void BuildBootRegion(const EXFMT_PARAMS *Params, const EXFMT_LAYOUT *Layout, EXU8 *Region)
{
    EXU32 Size = Layout->SectorSize;
    EXU8 *Boot = Region;
    EXU32 Checksum = 0;
    EXU32 i;

    memset(Region, 0, Size * 12);

    Boot[0] = 0xEB;
    Boot[1] = 0x76;
    Boot[2] = 0x90;
    memcpy(Boot + 3, "EXFAT   ", 8);
    Put64(Boot + 64, Layout->PartitionOffset);
    Put64(Boot + 72, Layout->VolumeSectors);
    Put32(Boot + 80, Layout->FatOffset);
    Put32(Boot + 84, Layout->FatLength);
    Put32(Boot + 88, Layout->HeapOffset);
    Put32(Boot + 92, Layout->ClusterCount);
    Put32(Boot + 96, Layout->RootCluster);
    Put32(Boot + 100, Params->Serial);
    Put16(Boot + 104, 0x0100);
    Put16(Boot + 106, 0);
    Boot[108] = (EXU8)Layout->SectorShift;
    Boot[109] = (EXU8)Layout->ClusterShift;
    Boot[110] = 1;
    Boot[111] = 0x80;
    Boot[112] = (EXU8)Layout->PercentInUse;

    /* Not bootable: stop the processor */
    memset(Boot + 120, 0xF4, 390);
    Boot[120] = 0xFA;
    Boot[121] = 0xF4;
    Boot[122] = 0xEB;
    Boot[123] = 0xFD;
    Boot[510] = 0x55;
    Boot[511] = 0xAA;

    /* Extended boot sectors carry only their signature */
    for (i = 1; i <= 8; i++) {
        Put32(Region + i * Size + Size - 4, 0xAA550000UL);
    }

    for (i = 0; i < Size * 11; i++) {
        if (i == 106 || i == 107 || i == 112) {
            continue;
        }
        Checksum = ((Checksum & 1) ? 0x80000000UL : 0) + (Checksum >> 1) + Region[i];
    }

    for (i = 0; i < Size; i += 4) {
        Put32(Region + 11 * Size + i, Checksum);
    }
}

/* The FAT entry of a cluster: the three system objects are chained, the rest is free */
static EXU32 FatEntry(const EXFMT_LAYOUT *Layout, EXU64 Cluster)
{
    if (Cluster == 0) {
        return 0xFFFFFFF8UL;
    }
    if (Cluster == 1) {
        return 0xFFFFFFFFUL;
    }
    if (Cluster >= 2 + (EXU64)Layout->UsedClusters) {
        return 0;
    }

    if (Cluster == Layout->UpcaseCluster - 1 || Cluster == Layout->RootCluster - 1 ||
        Cluster == Layout->RootCluster) {
        return 0xFFFFFFFFUL;
    }

    return (EXU32)(Cluster + 1);
}

/* Fills one chunk of a region; Offset is in bytes from the start of the region */
static void Fill(WRITER *W, int Region, EXU64 Offset, EXU8 *Buffer, EXU32 Length)
{
    const EXFMT_LAYOUT *L = W->Layout;
    const EXFMT_PARAMS *P = W->Params;
    EXU32 i;

    memset(Buffer, 0, Length);

    switch (Region) {

    case REGION_FAT:
        if (Offset < (EXU64)(2 + L->UsedClusters) * 4) {
            for (i = 0; i < Length; i += 4) {
                EXU64 Cluster = (Offset + i) / 4;
                if (Cluster >= 2 + (EXU64)L->UsedClusters) {
                    break;
                }
                Put32(Buffer + i, FatEntry(L, Cluster));
            }
        }
        break;

    case REGION_BITMAP:
        /* Bit n is cluster n + 2; the used clusters are the first ones */
        for (i = 0; i < Length && (Offset + i) * 8 < L->UsedClusters; i++) {
            EXU64 Bit = (Offset + i) * 8;
            EXU64 Left = L->UsedClusters - Bit;
            Buffer[i] = (EXU8)(Left >= 8 ? 0xFF : (1U << Left) - 1);
        }
        break;

    case REGION_UPCASE:
        for (i = 0; i < Length && Offset + i < EXFMT_UPCASE_BYTES; i += 2) {
            Put16(Buffer + i, ExfmtUpcase[(Offset + i) / 2]);
        }
        break;

    case REGION_ROOT:
        if (Offset == 0) {
            EXU8 *e = Buffer;

            /* Volume label; an empty one says there is none */
            e[0] = 0x83;
            e[1] = (EXU8)P->LabelLength;
            for (i = 0; i < P->LabelLength; i++) {
                Put16(e + 2 + i * 2, P->Label[i]);
            }

            e += 32;
            e[0] = 0x81;
            Put32(e + 20, L->BitmapCluster);
            Put64(e + 24, L->BitmapBytes);

            e += 32;
            e[0] = 0x82;
            Put32(e + 4, EXFMT_UPCASE_CHECKSUM);
            Put32(e + 20, L->UpcaseCluster);
            Put64(e + 24, EXFMT_UPCASE_BYTES);
        }
        break;
    }
}

static int WriteRegion(WRITER *W, int Region, EXU64 Sector, EXU64 Bytes)
{
    EXU64 Offset = 0;
    EXU64 Base = Sector * W->Layout->SectorSize;

    while (Offset < Bytes) {

        EXU32 Length = (EXU32)(Bytes - Offset > CHUNK ? CHUNK : Bytes - Offset);

        Fill(W, Region, Offset, W->Buffer, Length);

        if (!W->Write(W->Context, Base + Offset, W->Buffer, Length)) {
            return EXFMT_WRITE_FAILED;
        }

        Offset += Length;
        W->Done += Length;

        if (W->Progress != NULL) {
            W->Progress(W->Context, W->Done, W->Total);
        }
    }

    return EXFMT_OK;
}

static EXU64 ClusterSector(const EXFMT_LAYOUT *Layout, EXU32 Cluster)
{
    return Layout->HeapOffset + ((EXU64)(Cluster - 2) << Layout->ClusterShift);
}

int ExfmtFormat(const EXFMT_PARAMS *Params, const EXFMT_LAYOUT *Layout,
                EXFMT_WRITE Write, EXFMT_PROGRESS Progress, void *Context)
{
    WRITER W;
    EXU32 Size = Layout->SectorSize;
    EXU64 Heap = (EXU64)Layout->ClusterCount * Layout->ClusterSize;
    EXU64 Tail;
    EXU8 *Boot;
    int Result;

    if (UpcaseChecksum() != EXFMT_UPCASE_CHECKSUM) {
        return EXFMT_BAD_TABLE;
    }

    memset(&W, 0, sizeof(W));
    W.Params = Params;
    W.Layout = Layout;
    W.Write = Write;
    W.Progress = Progress;
    W.Context = Context;
    W.Buffer = (EXU8 *)malloc(CHUNK);
    Boot = (EXU8 *)malloc(12 * 4096);

    if (W.Buffer == NULL || Boot == NULL) {
        free(W.Buffer);
        free(Boot);
        return EXFMT_NO_MEMORY;
    }

    Tail = Layout->VolumeSectors * Size - ((EXU64)Layout->HeapOffset * Size + Heap);

    W.Total = 2 * (EXU64)Size + (EXU64)Layout->FatLength * Size + 24 * (EXU64)Size + Tail;
    if (Params->Full) {
        W.Total += Heap;
    }
    W.Total += ((EXU64)Layout->BitmapClusters + Layout->UpcaseClusters + 1) * Layout->ClusterSize;

    /* Whatever was there stops being recognized first */
    memset(Boot, 0, Size);
    if (!Write(Context, 0, Boot, Size) || !Write(Context, 12 * (EXU64)Size, Boot, Size)) {
        Result = EXFMT_WRITE_FAILED;
        goto Done;
    }
    W.Done += 2 * (EXU64)Size;

    Result = WriteRegion(&W, REGION_FAT, Layout->FatOffset, (EXU64)Layout->FatLength * Size);
    if (Result != EXFMT_OK) {
        goto Done;
    }

    if (Params->Full) {
        Result = WriteRegion(&W, REGION_ZERO, Layout->HeapOffset, Heap);
        if (Result != EXFMT_OK) {
            goto Done;
        }
    }

    /* Sectors past the last cluster */
    if (Tail != 0) {
        Result = WriteRegion(&W, REGION_ZERO, Layout->VolumeSectors - Tail / Size, Tail);
        if (Result != EXFMT_OK) {
            goto Done;
        }
    }

    Result = WriteRegion(&W, REGION_BITMAP, ClusterSector(Layout, Layout->BitmapCluster),
                         (EXU64)Layout->BitmapClusters * Layout->ClusterSize);
    if (Result != EXFMT_OK) {
        goto Done;
    }

    Result = WriteRegion(&W, REGION_UPCASE, ClusterSector(Layout, Layout->UpcaseCluster),
                         (EXU64)Layout->UpcaseClusters * Layout->ClusterSize);
    if (Result != EXFMT_OK) {
        goto Done;
    }

    Result = WriteRegion(&W, REGION_ROOT, ClusterSector(Layout, Layout->RootCluster),
                         Layout->ClusterSize);
    if (Result != EXFMT_OK) {
        goto Done;
    }

    /* Backup boot region, then the main one: from here on it is exFAT */
    BuildBootRegion(Params, Layout, Boot);
    if (!Write(Context, 12 * (EXU64)Size, Boot, 12 * Size) || !Write(Context, 0, Boot, 12 * Size)) {
        Result = EXFMT_WRITE_FAILED;
        goto Done;
    }
    W.Done += 24 * (EXU64)Size;

    if (Progress != NULL) {
        Progress(Context, W.Done, W.Total);
    }

Done:
    free(W.Buffer);
    free(Boot);
    return Result;
}

const char *ExfmtMessage(int Result)
{
    switch (Result) {
    case EXFMT_OK:                  return "success";
    case EXFMT_BAD_SECTOR_SIZE:     return "the sector size is not supported (512 to 4096 bytes)";
    case EXFMT_BAD_CLUSTER_SIZE:    return "the cluster size must be a power of two from the sector size to 32 MB";
    case EXFMT_TOO_SMALL:           return "the volume is too small for exFAT";
    case EXFMT_TOO_MANY_CLUSTERS:   return "too many clusters: choose a larger cluster size";
    case EXFMT_BAD_LABEL:           return "the label is too long (11 characters) or holds characters not allowed";
    case EXFMT_WRITE_FAILED:        return "writing to the volume failed";
    case EXFMT_NO_MEMORY:           return "not enough memory";
    case EXFMT_BAD_TABLE:           return "the built-in up-case table is damaged";
    }
    return "unknown error";
}
