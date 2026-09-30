/*
 * EXFATNT - cluster chains, run lists and the allocation bitmap
 *
 * Every stream (file, directory, bitmap) is described by a run list built
 * once from its FAT chain, or directly from its first cluster and length
 * when the NoFatChain flag says it is contiguous. Allocation keeps a
 * stream contiguous (and NoFatChain) as long as it can, and writes a FAT
 * chain for it once it cannot.
 */

#include "exfat.h"

#define EXF_FAT_ENTRY(Cluster)  ((LONGLONG)(Cluster) * 4)

/* ------------------------------------------------------------------ */
/* Run lists                                                           */
/* ------------------------------------------------------------------ */

/* Appends Count clusters at Lcn to the end of the list */
static NTSTATUS
ExfAppendRun (
    PEXF_RUN_LIST RunList,
    ULONG Lcn,
    ULONG Count
    )
{
    PEXF_RUN Last;
    PEXF_RUN NewRuns;
    ULONG NewMax;

    if (RunList->RunCount != 0) {

        Last = &RunList->Runs[RunList->RunCount - 1];

        if (Last->Lcn + Last->Count == Lcn) {
            Last->Count += Count;
            RunList->Clusters += Count;
            return STATUS_SUCCESS;
        }
    }

    if (RunList->RunCount == RunList->RunMax) {

        NewMax = RunList->RunMax ? RunList->RunMax * 2 : 4;

        NewRuns = (PEXF_RUN)ExAllocatePoolWithTag(PagedPool, NewMax * sizeof(EXF_RUN), EXF_TAG_RUNS);
        if (NewRuns == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (RunList->Runs != NULL) {
            RtlCopyMemory(NewRuns, RunList->Runs, RunList->RunCount * sizeof(EXF_RUN));
            ExFreePool(RunList->Runs);
        }

        RunList->Runs = NewRuns;
        RunList->RunMax = NewMax;
    }

    RunList->Runs[RunList->RunCount].Vcn = RunList->Clusters;
    RunList->Runs[RunList->RunCount].Lcn = Lcn;
    RunList->Runs[RunList->RunCount].Count = Count;
    RunList->RunCount++;
    RunList->Clusters += Count;

    return STATUS_SUCCESS;
}

/* Makes room for Extra more runs, so that appending them cannot fail */
static NTSTATUS
ExfReserveRuns (
    PEXF_RUN_LIST RunList,
    ULONG Extra
    )
{
    PEXF_RUN NewRuns;
    ULONG NewMax;

    if (RunList->RunCount + Extra <= RunList->RunMax) {
        return STATUS_SUCCESS;
    }

    NewMax = RunList->RunCount + Extra + 4;

    NewRuns = (PEXF_RUN)ExAllocatePoolWithTag(PagedPool, NewMax * sizeof(EXF_RUN), EXF_TAG_RUNS);
    if (NewRuns == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (RunList->Runs != NULL) {
        RtlCopyMemory(NewRuns, RunList->Runs, RunList->RunCount * sizeof(EXF_RUN));
        ExFreePool(RunList->Runs);
    }

    RunList->Runs = NewRuns;
    RunList->RunMax = NewMax;

    return STATUS_SUCCESS;
}

VOID
ExfFreeRunList (
    PEXF_RUN_LIST RunList
    )
{
    if (RunList->Runs != NULL) {
        ExFreePool(RunList->Runs);
    }

    RtlZeroMemory(RunList, sizeof(EXF_RUN_LIST));
}

static ULONG
ExfLastCluster (
    PEXF_RUN_LIST RunList
    )
{
    PEXF_RUN Last = &RunList->Runs[RunList->RunCount - 1];

    return Last->Lcn + Last->Count - 1;
}

static BOOLEAN
ExfIsValidCluster (
    PEXF_VCB Vcb,
    ULONG Cluster
    )
{
    return (BOOLEAN)(Cluster >= EXFAT_FIRST_CLUSTER && Cluster <= Vcb->ClusterCount + 1);
}

/*
 * Clusters is the expected length; 0 follows the chain to its end
 * (root directory).
 */
NTSTATUS
ExfBuildRunList (
    PEXF_VCB Vcb,
    ULONG FirstCluster,
    BOOLEAN NoFatChain,
    ULONG Clusters,
    PEXF_RUN_LIST RunList
    )
{
    EXF_MAP Map;
    ULONG Cluster;
    ULONG Next;
    ULONG Count = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlZeroMemory(RunList, sizeof(EXF_RUN_LIST));
    RtlZeroMemory(&Map, sizeof(Map));

    if (FirstCluster == 0) {
        return (Clusters == 0) ? STATUS_SUCCESS : STATUS_FILE_CORRUPT_ERROR;
    }

    if (!ExfIsValidCluster(Vcb, FirstCluster)) {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (NoFatChain) {

        if (Clusters == 0 || Clusters > Vcb->ClusterCount + 2 - FirstCluster) {
            return STATUS_FILE_CORRUPT_ERROR;
        }

        return ExfAppendRun(RunList, FirstCluster, Clusters);
    }

    __try {

        Cluster = FirstCluster;

        for (;;) {

            if (!ExfIsValidCluster(Vcb, Cluster)) {
                Status = STATUS_FILE_CORRUPT_ERROR;
                __leave;
            }

            Status = ExfAppendRun(RunList, Cluster, 1);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Count++;

            if (Clusters != 0 && Count == Clusters) {
                break;
            }

            if (Count > Vcb->ClusterCount) {
                Status = STATUS_FILE_CORRUPT_ERROR;
                __leave;
            }

            Next = *(ULONG UNALIGNED *)ExfMapStream(Vcb, Vcb->FatFcb, &Map,
                                                    EXF_FAT_ENTRY(Cluster), 4);

            if (Next == EXFAT_CLUSTER_END) {

                if (Clusters != 0) {
                    Status = STATUS_FILE_CORRUPT_ERROR;
                    __leave;
                }

                break;
            }

            Cluster = Next;
        }

    } __finally {

        ExfUnmap(&Map);

        if (AbnormalTermination() || !NT_SUCCESS(Status)) {
            ExfFreeRunList(RunList);
        }
    }

    if (!NT_SUCCESS(Status)) {
        EXF_DBG((EXF_PFX "Bad cluster chain at %lu\n", FirstCluster));
    }

    return Status;
}

BOOLEAN
ExfLookupVbo (
    PEXF_VCB Vcb,
    PEXF_RUN_LIST RunList,
    LONGLONG Vbo,
    PLONGLONG Lbo,
    PULONG Contiguous
    )
{
    PEXF_RUN Run;
    ULONG Vcn;
    ULONG Low;
    ULONG High;
    ULONG Middle;
    ULONG Within;
    ULONG ClusterOffset;
    ULONGLONG Remaining;

    if (Vbo < 0 || (ULONGLONG)Vbo >= ((ULONGLONG)RunList->Clusters << Vcb->ClusterShift)) {
        return FALSE;
    }

    Vcn = (ULONG)((ULONGLONG)Vbo >> Vcb->ClusterShift);

    Low = 0;
    High = RunList->RunCount;

    while (Low < High) {

        Middle = (Low + High) / 2;
        Run = &RunList->Runs[Middle];

        if (Vcn < Run->Vcn) {
            High = Middle;
        } else if (Vcn >= Run->Vcn + Run->Count) {
            Low = Middle + 1;
        } else {

            Within = Vcn - Run->Vcn;
            ClusterOffset = (ULONG)Vbo & (Vcb->ClusterSize - 1);

            *Lbo = ExfClusterToLbo(Vcb, Run->Lcn + Within) + ClusterOffset;

            Remaining = ((ULONGLONG)(Run->Count - Within) << Vcb->ClusterShift) - ClusterOffset;
            *Contiguous = (Remaining > 0x40000000) ? 0x40000000 : (ULONG)Remaining;

            return TRUE;
        }
    }

    return FALSE;
}

/*
 * The allocation bitmap and up-case table should have FAT chains, but
 * some formatters leave them contiguous without one.
 */
NTSTATUS
ExfBuildMetadataRunList (
    PEXF_VCB Vcb,
    ULONG FirstCluster,
    ULONGLONG Length,
    PEXF_RUN_LIST RunList
    )
{
    ULONGLONG Clusters;
    NTSTATUS Status;

    Clusters = (Length + Vcb->ClusterSize - 1) >> Vcb->ClusterShift;

    if (Clusters == 0 || Clusters > Vcb->ClusterCount) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    Status = ExfBuildRunList(Vcb, FirstCluster, FALSE, (ULONG)Clusters, RunList);

    if (Status == STATUS_FILE_CORRUPT_ERROR) {
        Status = ExfBuildRunList(Vcb, FirstCluster, TRUE, (ULONG)Clusters, RunList);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Allocation bitmap                                                   */
/* ------------------------------------------------------------------ */

static ULONG
ExfBitsSet (
    ULONG Byte
    )
{
    Byte = Byte - ((Byte >> 1) & 0x55);
    Byte = (Byte & 0x33) + ((Byte >> 2) & 0x33);
    return (Byte + (Byte >> 4)) & 0x0F;
}

NTSTATUS
ExfCountFreeClusters (
    PEXF_VCB Vcb
    )
{
    EXF_MAP Map;
    ULONG Bytes;
    ULONG Offset = 0;
    ULONG Chunk;
    ULONG Used = 0;
    ULONG i;
    PUCHAR Bits;
    UCHAR Last;

    Bytes = (Vcb->ClusterCount + 7) / 8;

    if (Vcb->BitmapLength < Bytes) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        while (Offset < Bytes) {

            Chunk = EXF_MAP_UNIT - (Offset & (EXF_MAP_UNIT - 1));
            if (Chunk > Bytes - Offset) {
                Chunk = Bytes - Offset;
            }

            Bits = ExfMapStream(Vcb, Vcb->BitmapFcb, &Map, Offset, Chunk);

            for (i = 0; i < Chunk; i++) {

                if (Offset + i == Bytes - 1 && (Vcb->ClusterCount & 7) != 0) {
                    Last = (UCHAR)(Bits[i] & ((1 << (Vcb->ClusterCount & 7)) - 1));
                    Used += ExfBitsSet(Last);
                } else {
                    Used += ExfBitsSet(Bits[i]);
                }
            }

            Offset += Chunk;
        }

    } __finally {

        ExfUnmap(&Map);
    }

    Vcb->FreeClusters = (Used > Vcb->ClusterCount) ? 0 : Vcb->ClusterCount - Used;

    return STATUS_SUCCESS;
}

/*
 * Finds the first free cluster at or after From (wrapping around) and
 * returns the free run there, at most Wanted clusters long.
 */
static BOOLEAN
ExfFindFreeRun (
    PEXF_VCB Vcb,
    ULONG From,
    ULONG Wanted,
    PULONG First,
    PULONG Count
    )
{
    EXF_MAP Map;
    ULONG Total = Vcb->ClusterCount;
    ULONG Index;
    ULONG Scanned = 0;
    ULONG Found = 0;
    ULONG Start = 0;
    ULONG Chunk;
    ULONG i;
    PUCHAR Bits;
    UCHAR Byte;

    RtlZeroMemory(&Map, sizeof(Map));

    if (!ExfIsValidCluster(Vcb, From)) {
        From = EXFAT_FIRST_CLUSTER;
    }

    Index = From - EXFAT_FIRST_CLUSTER;

    __try {

        while (Scanned < Total) {

            if (Index >= Total) {

                /* Wrap: a run never continues from the end to the start */
                if (Found != 0) {
                    break;
                }

                Index = 0;
            }

            /* Whole bytes that are full can be skipped while searching */
            if (Found == 0 && (Index & 7) == 0 && Index + 8 <= Total) {

                Chunk = EXF_MAP_UNIT - ((Index >> 3) & (EXF_MAP_UNIT - 1));
                if (Chunk > (Total - Index) >> 3) {
                    Chunk = (Total - Index) >> 3;
                }

                Bits = ExfMapStream(Vcb, Vcb->BitmapFcb, &Map, Index >> 3, Chunk);

                for (i = 0; i < Chunk && Bits[i] == 0xFF; i++) {
                    ;
                }

                if (i != 0) {
                    Index += i * 8;
                    Scanned += i * 8;
                    continue;
                }
            }

            Byte = *ExfMapStream(Vcb, Vcb->BitmapFcb, &Map, Index >> 3, 1);

            if (Byte & (1 << (Index & 7))) {

                if (Found != 0) {
                    break;
                }

            } else {

                if (Found == 0) {
                    Start = Index;
                }

                Found++;

                if (Found == Wanted) {
                    break;
                }
            }

            Index++;
            Scanned++;
        }

    } __finally {

        ExfUnmap(&Map);
    }

    if (Found == 0) {
        return FALSE;
    }

    *First = Start + EXFAT_FIRST_CLUSTER;
    *Count = Found;

    return TRUE;
}

static VOID
ExfMarkBitmap (
    PEXF_VCB Vcb,
    ULONG First,
    ULONG Count,
    BOOLEAN Used
    )
{
    EXF_MAP Map;
    ULONG Index = First - EXFAT_FIRST_CLUSTER;
    ULONG End = Index + Count;
    PUCHAR Byte;
    UCHAR Mask;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        while (Index < End) {

            Byte = ExfPinStream(Vcb, Vcb->BitmapFcb, &Map, Index >> 3, 1);

            if ((Index & 7) == 0 && End - Index >= 8) {

                *Byte = Used ? 0xFF : 0x00;
                Index += 8;

            } else {

                Mask = (UCHAR)(1 << (Index & 7));

                if (Used) {
                    *Byte |= Mask;
                } else {
                    *Byte &= (UCHAR)~Mask;
                }

                Index++;
            }

            ExfSetDirty(&Map);
        }

    } __finally {

        ExfUnmap(&Map);
    }
}

/* ------------------------------------------------------------------ */
/* FAT                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Chains the runs together in the FAT; the last cluster points to Next
 * (EXFAT_CLUSTER_END for the end of the stream).
 */
static VOID
ExfWriteChain (
    PEXF_VCB Vcb,
    PEXF_RUN Runs,
    ULONG RunCount,
    ULONG Next
    )
{
    EXF_MAP Map;
    ULONG r;
    ULONG c;
    ULONG Value;
    ULONG UNALIGNED *Entry;

    RtlZeroMemory(&Map, sizeof(Map));

    __try {

        for (r = 0; r < RunCount; r++) {

            for (c = 0; c < Runs[r].Count; c++) {

                if (c + 1 < Runs[r].Count) {
                    Value = Runs[r].Lcn + c + 1;
                } else if (r + 1 < RunCount) {
                    Value = Runs[r + 1].Lcn;
                } else {
                    Value = Next;
                }

                Entry = (ULONG UNALIGNED *)ExfPinStream(Vcb, Vcb->FatFcb, &Map,
                                                        EXF_FAT_ENTRY(Runs[r].Lcn + c), 4);
                *Entry = Value;
                ExfSetDirty(&Map);
            }
        }

    } __finally {

        ExfUnmap(&Map);
    }
}

static VOID
ExfWriteFatEntry (
    PEXF_VCB Vcb,
    ULONG Cluster,
    ULONG Value
    )
{
    EXF_RUN Run;

    Run.Vcn = 0;
    Run.Lcn = Cluster;
    Run.Count = 1;

    ExfWriteChain(Vcb, &Run, 1, Value);
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Adds Clusters clusters to the end of a stream. StreamFlags is NULL for
 * the root directory, which always has a FAT chain. Called with the
 * stream's paging resource exclusive when others can read its run list.
 */
NTSTATUS
ExfAllocateClusters (
    PEXF_VCB Vcb,
    PEXF_RUN_LIST RunList,
    PUCHAR StreamFlags,
    ULONG Clusters
    )
{
    EXF_RUN_LIST New;
    ULONG Hint;
    ULONG Wanted = Clusters;
    ULONG First;
    ULONG Count;
    ULONG LastLcn = 0;
    ULONG i;
    BOOLEAN WasEmpty = (BOOLEAN)(RunList->RunCount == 0);
    BOOLEAN NoFatChain;
    BOOLEAN Marked = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Clusters == 0) {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&New, sizeof(New));

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    __try {

        if (Clusters > Vcb->FreeClusters ||
            (ULONGLONG)RunList->Clusters + Clusters > Vcb->ClusterCount) {

            Status = STATUS_DISK_FULL;
            __leave;
        }

        if (!WasEmpty) {
            LastLcn = ExfLastCluster(RunList);
            Hint = LastLcn + 1;
        } else {
            Hint = Vcb->AllocHint;
        }

        while (Wanted != 0) {

            if (!ExfFindFreeRun(Vcb, Hint, Wanted, &First, &Count)) {
                Status = STATUS_DISK_FULL;
                __leave;
            }

            Status = ExfAppendRun(&New, First, Count);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Marked = TRUE;
            ExfMarkBitmap(Vcb, First, Count, TRUE);

            Wanted -= Count;
            Hint = First + Count;
        }

        Status = ExfReserveRuns(RunList, New.RunCount);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* Nothing below can fail short of a disk error */
        Marked = FALSE;

        Vcb->FreeClusters -= Clusters;
        Vcb->AllocHint = ExfIsValidCluster(Vcb, Hint) ? Hint : EXFAT_FIRST_CLUSTER;

        NoFatChain = (BOOLEAN)(StreamFlags != NULL && (*StreamFlags & EXFAT_STREAM_NO_FAT_CHAIN));

        if (WasEmpty) {

            if (StreamFlags != NULL && New.RunCount == 1) {
                *StreamFlags |= EXFAT_STREAM_NO_FAT_CHAIN;
            } else {
                ExfWriteChain(Vcb, New.Runs, New.RunCount, EXFAT_CLUSTER_END);
                if (StreamFlags != NULL) {
                    *StreamFlags &= ~EXFAT_STREAM_NO_FAT_CHAIN;
                }
            }

        } else if (NoFatChain) {

            if (New.RunCount != 1 || New.Runs[0].Lcn != LastLcn + 1) {

                /* No longer contiguous: give the old part its chain too */
                ExfWriteChain(Vcb, RunList->Runs, RunList->RunCount, New.Runs[0].Lcn);
                ExfWriteChain(Vcb, New.Runs, New.RunCount, EXFAT_CLUSTER_END);
                *StreamFlags &= ~EXFAT_STREAM_NO_FAT_CHAIN;
            }

        } else {

            ExfWriteChain(Vcb, New.Runs, New.RunCount, EXFAT_CLUSTER_END);
            ExfWriteFatEntry(Vcb, LastLcn, New.Runs[0].Lcn);
        }

        for (i = 0; i < New.RunCount; i++) {
            (VOID)ExfAppendRun(RunList, New.Runs[i].Lcn, New.Runs[i].Count);
        }

    } __finally {

        if (Marked) {

            /* Give back what this call took and did not attach */
            for (i = 0; i < New.RunCount; i++) {
                ExfMarkBitmap(Vcb, New.Runs[i].Lcn, New.Runs[i].Count, FALSE);
            }
        }

        ExfFreeRunList(&New);
        ExfRelease(&Vcb->AllocResource);
    }

    return Status;
}

/*
 * Keeps the first Keep clusters of a stream and frees the rest. The
 * caller has dropped every cached view of the freed part.
 */
VOID
ExfFreeClusters (
    PEXF_VCB Vcb,
    PEXF_RUN_LIST RunList,
    PUCHAR StreamFlags,
    ULONG Keep
    )
{
    PEXF_RUN Run;
    ULONG From;
    ULONG Freed;
    BOOLEAN NoFatChain;

    if (Keep >= RunList->Clusters) {
        return;
    }

    NoFatChain = (BOOLEAN)(StreamFlags != NULL && (*StreamFlags & EXFAT_STREAM_NO_FAT_CHAIN));

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->AllocResource, TRUE);

    __try {

        while (RunList->RunCount != 0) {

            Run = &RunList->Runs[RunList->RunCount - 1];

            if (Run->Vcn + Run->Count <= Keep) {
                break;
            }

            From = (Run->Vcn > Keep) ? Run->Vcn : Keep;
            Freed = Run->Vcn + Run->Count - From;

            ExfMarkBitmap(Vcb, Run->Lcn + (From - Run->Vcn), Freed, FALSE);

            Vcb->FreeClusters += Freed;
            RunList->Clusters -= Freed;
            Run->Count -= Freed;

            if (Run->Count == 0) {
                RunList->RunCount--;
            }
        }

        if (RunList->RunCount != 0 && !NoFatChain) {
            ExfWriteFatEntry(Vcb, ExfLastCluster(RunList), EXFAT_CLUSTER_END);
        }

        if (RunList->RunCount == 0 && StreamFlags != NULL) {
            *StreamFlags &= ~EXFAT_STREAM_NO_FAT_CHAIN;
        }

    } __finally {

        ExfRelease(&Vcb->AllocResource);
    }
}

/*
 * Sets the allocation of a file or directory to Bytes rounded up to
 * clusters. Called with the FCB exclusive; shrinking callers have already
 * cut FileSize and the cache.
 */
NTSTATUS
ExfSetAllocation (
    PEXF_VCB Vcb,
    PEXF_FCB Fcb,
    ULONGLONG Bytes
    )
{
    ULONGLONG Clusters;
    ULONG Have = Fcb->RunList.Clusters;
    NTSTATUS Status = STATUS_SUCCESS;

    Clusters = (Bytes + Vcb->ClusterSize - 1) >> Vcb->ClusterShift;

    if (Clusters > Vcb->ClusterCount) {
        return STATUS_DISK_FULL;
    }

    if ((ULONG)Clusters == Have) {
        return STATUS_SUCCESS;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);

    __try {

        if ((ULONG)Clusters > Have) {

            Status = ExfAllocateClusters(Vcb, &Fcb->RunList,
                                         (Fcb->FcbState & FCB_STATE_ROOT) ? NULL : &Fcb->StreamFlags,
                                         (ULONG)Clusters - Have);
        } else {

            ExfFreeClusters(Vcb, &Fcb->RunList,
                            (Fcb->FcbState & FCB_STATE_ROOT) ? NULL : &Fcb->StreamFlags,
                            (ULONG)Clusters);
        }

        Fcb->FirstCluster = (Fcb->RunList.RunCount != 0) ? Fcb->RunList.Runs[0].Lcn : 0;
        Fcb->Header.AllocationSize.QuadPart = (LONGLONG)Fcb->RunList.Clusters << Vcb->ClusterShift;
        Fcb->FcbState |= FCB_STATE_DIRENT_DIRTY;

    } __finally {

        ExfRelease(&Fcb->PagingIoResource);
    }

    return Status;
}
