/*
 * EXFACHK - checks exFAT volumes at boot time, like autochk
 *
 * A native application, run by the session manager before Windows starts
 * (BootExecute: "autocheck exfachk *"). Only ntdll.dll is there then, so
 * this file declares the few native calls it needs itself.
 *
 * Every partition of every disk is looked at through the whole-disk
 * device (Partition0), so no file system is mounted on partitions that
 * are not exFAT. An exFAT volume whose VolumeDirty flag is set is opened,
 * locked, checked and repaired, then dismounted; a clean one is left
 * alone.
 *
 *   exfachk [*] [/p] [/v]      /p: check clean volumes too, /v: name every file
 */

#include <stddef.h>
#include "exfchk.h"

typedef long NTSTATUS;
typedef void *HANDLE;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef unsigned char UCHAR;

#define NTAPI __stdcall
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING *ObjectName;
    ULONG Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS Status;
    ULONG Information;
} IO_STATUS_BLOCK;

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; long HighPart; } u;
    EXU64 QuadPart;
} LARGE_INTEGER;

typedef struct _DISK_GEOMETRY {
    LARGE_INTEGER Cylinders;
    ULONG MediaType;
    ULONG TracksPerCylinder;
    ULONG SectorsPerTrack;
    ULONG BytesPerSector;
} DISK_GEOMETRY;

typedef struct _PARTITION_INFORMATION {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    ULONG HiddenSectors;
    ULONG PartitionNumber;
    UCHAR PartitionType;
    UCHAR BootIndicator;
    UCHAR RecognizedPartition;
    UCHAR RewritePartition;
} PARTITION_INFORMATION;

typedef struct _DRIVE_LAYOUT_INFORMATION {
    ULONG PartitionCount;
    ULONG Signature;
    PARTITION_INFORMATION PartitionEntry[1];
} DRIVE_LAYOUT_INFORMATION;

/* The parts of the PEB and the process parameters used here (x86) */
typedef struct _PROCESS_PARAMETERS {
    ULONG MaximumLength;
    ULONG Length;
    ULONG Flags;
    ULONG DebugFlags;
    HANDLE ConsoleHandle;
    ULONG ConsoleFlags;
    HANDLE StandardInput;
    HANDLE StandardOutput;
    HANDLE StandardError;
    UNICODE_STRING CurrentDirectoryPath;
    HANDLE CurrentDirectoryHandle;
    UNICODE_STRING DllPath;
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
} PROCESS_PARAMETERS;

typedef struct _PEB {
    UCHAR Flags[4];
    HANDLE Mutant;
    void *ImageBaseAddress;
    void *Ldr;
    PROCESS_PARAMETERS *ProcessParameters;
    void *SubSystemData;
    void *ProcessHeap;
} PEB;

#define OBJ_CASE_INSENSITIVE            0x40
#define SYNCHRONIZE                     0x00100000UL
#define FILE_READ_DATA                  0x0001
#define FILE_WRITE_DATA                 0x0002
#define FILE_SHARE_READ                 0x0001
#define FILE_SHARE_WRITE                0x0002
#define FILE_SYNCHRONOUS_IO_NONALERT    0x0020
#define MEM_COMMIT                      0x1000
#define MEM_RELEASE                     0x8000
#define PAGE_READWRITE                  0x04
#define HEAP_ZERO_MEMORY                0x08

#define IOCTL_DISK_GET_DRIVE_GEOMETRY   0x00070000UL
#define IOCTL_DISK_GET_DRIVE_LAYOUT     0x0007400CUL
#define FSCTL_LOCK_VOLUME               0x00090018UL
#define FSCTL_UNLOCK_VOLUME             0x0009001CUL
#define FSCTL_DISMOUNT_VOLUME           0x00090020UL

#define CURRENT_PROCESS ((HANDLE)-1)

NTSTATUS NTAPI NtOpenFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, ULONG, ULONG);
NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, void *, ULONG, LARGE_INTEGER *, ULONG *);
NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, const void *, ULONG, LARGE_INTEGER *, ULONG *);
NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, ULONG, void *, ULONG, void *, ULONG);
NTSTATUS NTAPI NtFsControlFile(HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, ULONG, void *, ULONG, void *, ULONG);
NTSTATUS NTAPI NtClose(HANDLE);
NTSTATUS NTAPI NtDisplayString(UNICODE_STRING *);
NTSTATUS NTAPI NtTerminateProcess(HANDLE, NTSTATUS);
NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE, void **, ULONG, ULONG *, ULONG, ULONG);
NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, void **, ULONG *, ULONG);
void *NTAPI RtlAllocateHeap(void *, ULONG, ULONG);
int NTAPI RtlFreeHeap(void *, ULONG, void *);
PROCESS_PARAMETERS *NTAPI RtlNormalizeProcessParams(PROCESS_PARAMETERS *);

#define BOUNCE_SIZE (256UL * 1024)

static void *Heap;
static HANDLE Volume;
static EXU8 *Bounce;
static int Verbose;

/* ------------------------------------------------------------------ */
/* Output on the boot screen                                           */
/* ------------------------------------------------------------------ */

static void Show(const EXU16 *Text, EXU32 Length)
{
    WCHAR Line[700];
    UNICODE_STRING s;
    EXU32 i;

    if (Length > 690) {
        Length = 690;
    }
    for (i = 0; i < Length; i++) {
        Line[i] = Text[i];
    }
    Line[Length++] = '\n';
    s.Buffer = Line;
    s.Length = (USHORT)(Length * sizeof(WCHAR));
    s.MaximumLength = s.Length;
    NtDisplayString(&s);
}

/* An ASCII line with at most two numbers (%u) */
static void ShowText(const char *Text, EXU32 A, EXU32 B)
{
    EXU16 Line[300];
    EXU32 n = 0, v, Args = 0;
    char Digits[12];
    int d;

    for (; *Text && n < 280; Text++) {
        if (Text[0] == '%' && Text[1] == 'u') {
            v = Args++ == 0 ? A : B;
            d = 0;
            do {
                Digits[d++] = (char)('0' + v % 10);
                v /= 10;
            } while (v != 0);
            while (d > 0) {
                Line[n++] = (EXU8)Digits[--d];
            }
            Text++;
        } else {
            Line[n++] = (EXU8)*Text;
        }
    }
    Show(Line, n);
}

/* ------------------------------------------------------------------ */
/* Host routines for the checker                                       */
/* ------------------------------------------------------------------ */

static void CopyBytes(void *To, const void *From, EXU32 n)
{
    EXU8 *d = (EXU8 *)To;
    const EXU8 *s = (const EXU8 *)From;
    while (n--) {
        *d++ = *s++;
    }
}

static int DiskIo(HANDLE Handle, int Write, EXU64 Offset, void *Buffer, EXU32 Length)
{
    IO_STATUS_BLOCK Io;
    LARGE_INTEGER At;
    EXU32 Done = 0, n;
    NTSTATUS Status;

    while (Done < Length) {
        n = Length - Done > BOUNCE_SIZE ? BOUNCE_SIZE : Length - Done;
        At.QuadPart = Offset + Done;
        if (Write) {
            CopyBytes(Bounce, (EXU8 *)Buffer + Done, n);
            Status = NtWriteFile(Handle, NULL, NULL, NULL, &Io, Bounce, n, &At, NULL);
        } else {
            Status = NtReadFile(Handle, NULL, NULL, NULL, &Io, Bounce, n, &At, NULL);
        }
        if (!NT_SUCCESS(Status) || Io.Information != n) {
            return 0;
        }
        if (!Write) {
            CopyBytes((EXU8 *)Buffer + Done, Bounce, n);
        }
        Done += n;
    }
    return 1;
}

static int HostRead(void *c, EXU64 Offset, void *Buffer, EXU32 Length)
{
    return DiskIo(Volume, 0, Offset, Buffer, Length);
}

static int HostWrite(void *c, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    return DiskIo(Volume, 1, Offset, (void *)Buffer, Length);
}

static void *HostAlloc(void *c, EXU32 Size) { return RtlAllocateHeap(Heap, HEAP_ZERO_MEMORY, Size); }
static void HostFree(void *c, void *p) { RtlFreeHeap(Heap, 0, p); }
static void HostPrint(void *c, const EXU16 *Text, EXU32 Length) { Show(Text, Length); }

/* ------------------------------------------------------------------ */

static void MakeName(WCHAR *Out, EXU32 Disk, EXU32 Part)
{
    const char *Pre = "\\Device\\Harddisk", *Mid = "\\Partition";
    EXU32 n = 0, v, i;
    char Digits[12];
    int d;

    for (i = 0; Pre[i]; i++) Out[n++] = (EXU8)Pre[i];
    for (i = 0; i < 2; i++) {
        v = i == 0 ? Disk : Part;
        d = 0;
        do {
            Digits[d++] = (char)('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (d > 0) Out[n++] = (EXU8)Digits[--d];
        if (i == 0) {
            for (v = 0; Mid[v]; v++) Out[n++] = (EXU8)Mid[v];
        }
    }
    Out[n] = 0;
}

static NTSTATUS Open(EXU32 Disk, EXU32 Part, int Write, HANDLE *Handle)
{
    WCHAR Name[64];
    UNICODE_STRING s;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Io;
    EXU32 n;

    MakeName(Name, Disk, Part);
    for (n = 0; Name[n]; n++) ;
    s.Buffer = Name;
    s.Length = (USHORT)(n * sizeof(WCHAR));
    s.MaximumLength = s.Length;
    Oa.Length = sizeof(Oa);
    Oa.RootDirectory = NULL;
    Oa.ObjectName = &s;
    Oa.Attributes = OBJ_CASE_INSENSITIVE;
    Oa.SecurityDescriptor = NULL;
    Oa.SecurityQualityOfService = NULL;
    return NtOpenFile(Handle, SYNCHRONIZE | FILE_READ_DATA | (Write ? FILE_WRITE_DATA : 0), &Oa, &Io,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS Fsctl(HANDLE Handle, ULONG Code)
{
    IO_STATUS_BLOCK Io;
    return NtFsControlFile(Handle, NULL, NULL, NULL, &Io, Code, NULL, 0, NULL, 0);
}

/* Locks, checks and repairs, dismounts one volume */
static void CheckVolume(EXU32 Disk, EXU32 Part, EXU32 SectorSize, EXU64 Sectors)
{
    EXC_PARAMS Params;
    EXC_HOST Host;
    EXC_RESULT Result;
    NTSTATUS Status;
    int r;

    ShowText("Checking the exFAT volume on disk %u, partition %u.", Disk, Part);

    Status = Open(Disk, Part, 1, &Volume);
    if (!NT_SUCCESS(Status)) {
        ShowText("It cannot be opened; not checked.", 0, 0);
        return;
    }

    if (!NT_SUCCESS(Fsctl(Volume, FSCTL_LOCK_VOLUME))) {
        ShowText("It is in use and cannot be locked; not checked.", 0, 0);
        NtClose(Volume);
        return;
    }

    Params.VolumeSectors = Sectors;
    Params.SectorSize = SectorSize;
    Params.Fix = 1;
    Params.Verbose = Verbose;
    Host.Context = NULL;
    Host.Read = HostRead;
    Host.Write = HostWrite;
    Host.Alloc = HostAlloc;
    Host.Free = HostFree;
    Host.Print = HostPrint;

    r = ExcCheck(&Params, &Host, &Result);

    /* The file system lets go of what it read before the repair */
    Fsctl(Volume, FSCTL_DISMOUNT_VOLUME);
    Fsctl(Volume, FSCTL_UNLOCK_VOLUME);
    NtClose(Volume);

    switch (r) {
    case EXC_CLEAN: ShowText("No problems were found.", 0, 0); break;
    case EXC_FIXED: ShowText("%u problems were found and fixed.", Result.Problems, 0); break;
    case EXC_ERRORS: ShowText("%u problems were found, %u could not be fixed.", Result.Problems,
                              Result.Problems - Result.Fixed); break;
    default: ShowText("The volume could not be checked.", 0, 0); break;
    }
}

/* Every exFAT partition of one disk; FALSE if there is no such disk */
static int CheckDisk(EXU32 Disk, int Always)
{
    HANDLE Whole;
    IO_STATUS_BLOCK Io;
    DISK_GEOMETRY Geometry;
    DRIVE_LAYOUT_INFORMATION *Layout;
    EXU32 i, Size = 16384, Sector;
    NTSTATUS Status;
    EXU8 *Boot;

    if (!NT_SUCCESS(Open(Disk, 0, 0, &Whole))) {
        return 0;
    }

    Status = NtDeviceIoControlFile(Whole, NULL, NULL, NULL, &Io, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL, 0, &Geometry, sizeof(Geometry));
    Layout = (DRIVE_LAYOUT_INFORMATION *)RtlAllocateHeap(Heap, HEAP_ZERO_MEMORY, Size);
    Boot = (EXU8 *)RtlAllocateHeap(Heap, HEAP_ZERO_MEMORY, 4096);

    if (NT_SUCCESS(Status) && Layout != NULL && Boot != NULL &&
        Geometry.BytesPerSector >= 512 && Geometry.BytesPerSector <= 4096 &&
        NT_SUCCESS(NtDeviceIoControlFile(Whole, NULL, NULL, NULL, &Io, IOCTL_DISK_GET_DRIVE_LAYOUT,
                                         NULL, 0, Layout, Size))) {

        Sector = Geometry.BytesPerSector;

        for (i = 0; i < Layout->PartitionCount && (i + 1) * sizeof(PARTITION_INFORMATION) + 8 <= Size; i++) {

            PARTITION_INFORMATION *p = &Layout->PartitionEntry[i];
            UCHAR Type = p->PartitionType;
            int State;

            if (p->PartitionNumber == 0 || p->PartitionLength.QuadPart == 0 ||
                Type == 0x05 || Type == 0x0F || Type == 0x85) {
                continue;
            }

            if (!DiskIo(Whole, 0, p->StartingOffset.QuadPart, Boot, Sector)) {
                continue;
            }

            State = ExcQuickState(Boot);
            if (State == EXC_NOT_EXFAT || (State == EXC_STATE_CLEAN && !Always)) {
                continue;
            }

            CheckVolume(Disk, p->PartitionNumber, Sector, p->PartitionLength.QuadPart / Sector);
        }
    }

    if (Layout != NULL) RtlFreeHeap(Heap, 0, Layout);
    if (Boot != NULL) RtlFreeHeap(Heap, 0, Boot);
    NtClose(Whole);
    return 1;
}

void NTAPI NtProcessStartup(PEB *Peb)
{
    PROCESS_PARAMETERS *Params;
    UNICODE_STRING *Line;
    EXU32 i, n, Disk, Missing = 0;
    int Always = 0;
    ULONG Size = BOUNCE_SIZE;
    void *Memory = NULL;

    Heap = Peb->ProcessHeap;
    Params = RtlNormalizeProcessParams(Peb->ProcessParameters);

    /* Options anywhere on the command line */
    Line = &Params->CommandLine;
    n = Line->Length / sizeof(WCHAR);
    for (i = 0; i + 1 < n; i++) {
        if (Line->Buffer[i] == '/' || Line->Buffer[i] == '-') {
            WCHAR c = Line->Buffer[i + 1];
            if (c == 'p' || c == 'P') Always = 1;
            if (c == 'v' || c == 'V') Verbose = 1;
        }
    }

    if (!NT_SUCCESS(NtAllocateVirtualMemory(CURRENT_PROCESS, &Memory, 0, &Size, MEM_COMMIT, PAGE_READWRITE))) {
        NtTerminateProcess(CURRENT_PROCESS, 3);
        return;
    }
    Bounce = (EXU8 *)Memory;

    /* Disks are numbered from 0; allow a few gaps */
    for (Disk = 0; Disk < 64 && Missing < 8; Disk++) {
        if (CheckDisk(Disk, Always)) {
            Missing = 0;
        } else {
            Missing++;
        }
    }

    Size = 0;
    NtFreeVirtualMemory(CURRENT_PROCESS, &Memory, &Size, MEM_RELEASE);
    NtTerminateProcess(CURRENT_PROCESS, 0);
}
