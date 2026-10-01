/*
 * EXFMT - exFAT format tool for Windows NT 3.51, NT 4.0 and Windows 2000
 *
 * exfmt drive: [/V:label] [/A:size] [/F] [/X] [/Y]
 *
 * Takes the volume over the way format.com does (lock), writes a new exFAT
 * volume through the volume handle, sets the partition type to 0x07 and
 * dismounts what was there. The next access mounts it with exfatnt.sys.
 */

#include <windows.h>
#include <winioctl.h>
#ifdef EXF_OWN_CRT
#include "exfcrt.h"     /* NT build: no C library, runs on NT 3.1 too */
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#endif
#include "exfmt.h"

#define ALIGNED_SIZE (1024UL * 1024 + 65536)

static HANDLE Volume = INVALID_HANDLE_VALUE;
static EXU8 *Aligned;
static DWORD LastPercent = 101;

static void Usage(void)
{
    printf("Formats a disk for use with exFAT (exfatnt.sys).\n\n"
           "EXFMT drive: [/V:label] [/A:size] [/F] [/X] [/Y]\n\n");
    printf("  /V:label  Volume label, up to 11 characters.\n"
           "  /A:size   Cluster size: 512, 1024, 2048, 4096, 8192, 16K, 32K, 64K,\n"
           "            128K, 256K, 512K, 1M, 2M, 4M, 8M, 16M or 32M. The default\n"
           "            is 4K up to 256 MB, 32K up to 32 GB and 128K above.\n"
           "  /F        Full format: zero the whole volume, not just the metadata.\n"
           "  /X        Force the volume to dismount first if it is in use.\n"
           "  /Y        Do not ask for confirmation.\n");
}

static void Fail(const char *What)
{
    DWORD Error = GetLastError();
    char Text[256];

    Text[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error,
                   0, Text, sizeof(Text), NULL);
    printf("%s failed (error %lu): %s\n", What, (unsigned long)Error, Text);
}

static int WriteVolume(void *Context, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    LONG High = (LONG)(Offset >> 32);
    DWORD Written = 0;
    EXU32 Done = 0;

    if (SetFilePointer(Volume, (LONG)(Offset & 0xFFFFFFFFUL), &High, FILE_BEGIN) == 0xFFFFFFFF &&
        GetLastError() != NO_ERROR) {
        Fail("Seeking on the volume");
        return 0;
    }

    /* The volume handle wants sector-aligned buffers */
    while (Done < Length) {
        EXU32 n = Length - Done;
        if (n > 1024UL * 1024) {
            n = 1024UL * 1024;
        }
        memcpy(Aligned, (const EXU8 *)Buffer + Done, n);
        if (!WriteFile(Volume, Aligned, n, &Written, NULL) || Written != n) {
            Fail("Writing to the volume");
            return 0;
        }
        Done += n;
    }

    return 1;
}

static void Progress(void *Context, EXU64 Done, EXU64 Total)
{
    DWORD Percent = (DWORD)(Total ? (Done * 100) / Total : 100);

    if (Percent != LastPercent) {
        printf("\r%lu percent completed.", (unsigned long)Percent);
        LastPercent = Percent;
    }
}

static int ParseSize(const char *Text, EXU32 *Size)
{
    char *End;
    unsigned long Value = strtoul(Text, &End, 10);

    if (*End == 'k' || *End == 'K') {
        Value *= 1024;
        End++;
    } else if (*End == 'm' || *End == 'M') {
        Value *= 1024 * 1024;
        End++;
    }

    if (*End != 0 || Value == 0) {
        return 0;
    }

    *Size = (EXU32)Value;
    return 1;
}

static int Confirm(const char *Question)
{
    char Line[16];

    printf("%s", Question);
    fflush(stdout);

    if (fgets(Line, sizeof(Line), stdin) == NULL) {
        return 0;
    }

    return Line[0] == 'y' || Line[0] == 'Y';
}

int main(int argc, char **argv)
{
    EXFMT_PARAMS Params;
    EXFMT_LAYOUT Layout;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    SET_PARTITION_INFORMATION SetType;
    SYSTEMTIME Now;
    char Drive = 0;
    char Path[16];
    char WinDir[MAX_PATH];
    char Name[32];
    WCHAR LabelW[64];
    const char *Label = NULL;
    int Force = 0, Quiet = 0, HavePartition, Result, i, Locked = 0, Status = 1;
    DWORD Bytes, Serial, MaxName, Flags;
    EXU64 VolumeBytes;

    memset(&Params, 0, sizeof(Params));

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') {
            if (Drive != 0 || !isalpha((unsigned char)a[0]) || a[1] != ':' || a[2] != 0) {
                Usage();
                return 1;
            }
            Drive = (char)toupper((unsigned char)a[0]);
        } else if ((a[1] == 'v' || a[1] == 'V') && a[2] == ':') {
            Label = a + 3;
        } else if ((a[1] == 'a' || a[1] == 'A') && a[2] == ':') {
            if (!ParseSize(a + 3, &Params.ClusterSize)) {
                printf("Invalid cluster size: %s\n", a + 3);
                return 1;
            }
        } else if ((a[1] == 'f' || a[1] == 'F') && a[2] == 0) {
            Params.Full = 1;
        } else if ((a[1] == 'x' || a[1] == 'X') && a[2] == 0) {
            Force = 1;
        } else if ((a[1] == 'y' || a[1] == 'Y') && a[2] == 0) {
            Quiet = 1;
        } else {
            Usage();
            return 1;
        }
    }

    if (Drive == 0) {
        Usage();
        return 1;
    }

    if (Label != NULL) {
        int n = MultiByteToWideChar(CP_ACP, 0, Label, -1, LabelW, sizeof(LabelW) / sizeof(LabelW[0]));
        if (n <= 0 || n - 1 > EXFMT_MAX_LABEL) {
            printf("%s\n", ExfmtMessage(EXFMT_BAD_LABEL));
            return 1;
        }
        Params.LabelLength = n - 1;
        memcpy(Params.Label, LabelW, Params.LabelLength * sizeof(WCHAR));
        if (ExfmtCheckLabel(Params.Label, Params.LabelLength) != EXFMT_OK) {
            printf("%s\n", ExfmtMessage(EXFMT_BAD_LABEL));
            return 1;
        }
    }

    if (GetWindowsDirectoryA(WinDir, sizeof(WinDir)) && toupper((unsigned char)WinDir[0]) == Drive) {
        printf("%c: holds Windows and cannot be formatted.\n", Drive);
        return 1;
    }

    sprintf(Path, "%c:\\", Drive);
    Flags = GetDriveTypeA(Path);
    if (Flags != DRIVE_FIXED && Flags != DRIVE_REMOVABLE) {
        printf("%c: is not a local disk.\n", Drive);
        return 1;
    }

    Aligned = (EXU8 *)VirtualAlloc(NULL, ALIGNED_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (Aligned == NULL) {
        printf("%s\n", ExfmtMessage(EXFMT_NO_MEMORY));
        return 1;
    }

    sprintf(Path, "\\\\.\\%c:", Drive);
    Volume = CreateFileA(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (Volume == INVALID_HANDLE_VALUE) {
        Fail("Opening the volume");
        return 1;
    }

    /* Geometry and size */
    if (!DeviceIoControl(Volume, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Geometry, sizeof(Geometry),
                         &Bytes, NULL)) {
        Fail("Reading the disk geometry");
        goto Done;
    }

    HavePartition = DeviceIoControl(Volume, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Partition,
                                    sizeof(Partition), &Bytes, NULL);

    Params.SectorSize = Geometry.BytesPerSector;

    if (HavePartition) {
        VolumeBytes = (EXU64)Partition.PartitionLength.QuadPart;
        Params.PartitionOffset = (EXU64)Partition.StartingOffset.QuadPart / Geometry.BytesPerSector;
    } else {
        /* A disk without a partition table */
        VolumeBytes = (EXU64)Geometry.Cylinders.QuadPart * Geometry.TracksPerCylinder *
                      Geometry.SectorsPerTrack * Geometry.BytesPerSector;
    }

    Params.VolumeSectors = VolumeBytes / Geometry.BytesPerSector;

    /* The serial number the way DOS made it, from the date and time */
    GetLocalTime(&Now);
    Serial = (((DWORD)Now.wMonth << 8 | Now.wDay) + ((DWORD)Now.wSecond << 8 | Now.wMilliseconds / 10)) << 16;
    Serial += ((DWORD)Now.wHour << 8 | Now.wMinute) + Now.wYear;
    Params.Serial = Serial;

    Result = ExfmtLayout(&Params, &Layout);
    if (Result != EXFMT_OK) {
        printf("%s\n", ExfmtMessage(Result));
        goto Done;
    }

    printf("%c: %lu MB, %lu-byte sectors, %lu-byte clusters, %lu clusters.\n", Drive,
           (unsigned long)(VolumeBytes >> 20), (unsigned long)Params.SectorSize,
           (unsigned long)Layout.ClusterSize, (unsigned long)Layout.ClusterCount);

    if (!Quiet && !Confirm("WARNING, ALL DATA ON THIS DRIVE WILL BE LOST!\nProceed with Format (Y/N)? ")) {
        Status = 1;
        goto Done;
    }

    /*
     * Take the volume over: with the lock, the file system mounted there
     * has flushed everything and lets this handle write to the volume.
     * /X dismounts it first, which ends every other open handle.
     */
    if (!DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL)) {
        if (!Force) {
            printf("The volume is in use. Close the programs using it, or use /X.\n");
            goto Done;
        }
        if (!DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL)) {
            Fail("Dismounting the volume");
            goto Done;
        }
        CloseHandle(Volume);
        /* NT 3.1 fails the first open after a dismount while it mounts again */
        Volume = CreateFileA(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (Volume == INVALID_HANDLE_VALUE) {
            Volume = CreateFileA(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
        }
        if (Volume == INVALID_HANDLE_VALUE) {
            Fail("Opening the volume");
            return 1;
        }
        if (!DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL)) {
            Fail("Locking the volume");
            goto Done;
        }
    }
    Locked = 1;

    Result = ExfmtFormat(&Params, &Layout, WriteVolume, Progress, NULL);
    printf("\n");
    if (Result != EXFMT_OK) {
        printf("Format failed: %s\n", ExfmtMessage(Result));
        goto Done;
    }

    /* 0x07 is the type of installable file systems */
    if (HavePartition && Partition.PartitionType != 0x07) {
        SetType.PartitionType = 0x07;
        if (!DeviceIoControl(Volume, IOCTL_DISK_SET_PARTITION_INFO, &SetType, sizeof(SetType),
                             NULL, 0, &Bytes, NULL)) {
            Fail("Setting the partition type to 0x07");
        }
    }

    FlushFileBuffers(Volume);

    /* The old file system lets go; the next access mounts the new one */
    if (!DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL)) {
        Fail("Dismounting the volume");
    }

    Status = 0;

Done:
    if (Locked) {
        DeviceIoControl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    }
    CloseHandle(Volume);

    if (Status == 0) {
        sprintf(Path, "%c:\\", Drive);
        if (GetVolumeInformationA(Path, NULL, 0, &Serial, &MaxName, &Flags, Name, sizeof(Name))) {
            printf("Format complete. %c: is now %s, serial number %04lX-%04lX.\n", Drive, Name,
                   (unsigned long)(Serial >> 16), (unsigned long)(Serial & 0xFFFF));
            if (strcmp(Name, "exFAT") != 0) {
                printf("exfatnt.sys does not seem to be running: the volume is not mounted as exFAT.\n");
            }
        } else {
            printf("Format complete. The volume does not mount yet: is exfatnt.sys installed and started?\n");
        }
    }

    return Status;
}
