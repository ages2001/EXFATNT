/*
 * EXFCHK - exFAT volume check and repair
 *
 * The checking and repairing of an exFAT volume, free of any operating
 * system and C library call: the front ends (exfatchk.c for Windows,
 * exfachk.c at boot time, test\exfchkl.c for the tests) supply the I/O,
 * memory and output routines.
 */

#ifndef EXFCHK_H
#define EXFCHK_H

#include "../fmt/exfmt.h"

/* Results of ExcCheck */
#define EXC_CLEAN       0       /* no problems found */
#define EXC_FIXED       1       /* problems found and all of them fixed */
#define EXC_ERRORS      2       /* problems left: not fixing, or not fixable */
#define EXC_FAILED      3       /* not an exFAT volume, or the disk failed */

/* What ExcQuickState says of a boot sector */
#define EXC_NOT_EXFAT   (-1)
#define EXC_STATE_CLEAN 0
#define EXC_STATE_DIRTY 1

typedef struct _EXC_HOST {
    void *Context;
    /* Offset and Length are whole sectors; nonzero on success */
    int (*Read)(void *Context, EXU64 Offset, void *Buffer, EXU32 Length);
    int (*Write)(void *Context, EXU64 Offset, const void *Buffer, EXU32 Length);
    /* Zeroed memory, or NULL */
    void *(*Alloc)(void *Context, EXU32 Size);
    void (*Free)(void *Context, void *Memory);
    /* One line of output, without the line end */
    void (*Print)(void *Context, const EXU16 *Text, EXU32 Length);
} EXC_HOST;

typedef struct _EXC_PARAMS {
    EXU64 VolumeSectors;        /* size of the volume (partition), 0 if unknown */
    EXU32 SectorSize;           /* of the device, 0 to take the boot sector's */
    int Fix;                    /* repair what is found */
    int Verbose;                /* name every file and directory checked */
} EXC_PARAMS;

typedef struct _EXC_RESULT {
    EXU32 Problems;             /* problems found */
    EXU32 Fixed;                /* of them fixed */
    int WasDirty;               /* VolumeDirty was set */
    EXU32 Files;
    EXU32 Directories;
    EXU32 ClusterSize;
    EXU32 ClusterCount;
    EXU32 UsedClusters;
    EXU64 FileBytes;
} EXC_RESULT;

int ExcQuickState(const EXU8 *BootSector);
int ExcCheck(const EXC_PARAMS *Params, const EXC_HOST *Host, EXC_RESULT *Result);

#endif
