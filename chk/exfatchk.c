/*
 * EXFATCHK - checks and repairs exFAT volumes on Windows NT 3.51, NT 4.0
 * and Windows 2000
 *
 * exfatchk drive: [/F] [/X] [/V]
 * exfatchk /INSTALL | /UNINSTALL
 *
 * Without /F the volume is only read. With /F it is locked (with /X
 * dismounted first if need be) and repaired through the volume handle;
 * unlocking then makes the file system mount it afresh. A volume that
 * cannot be locked can be marked dirty instead, and exfachk.exe repairs
 * it at the next restart. /INSTALL adds exfachk.exe to the programs run
 * at boot (BootExecute), /UNINSTALL removes it.
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
#include "exfchk.h"

#define BOUNCE_SIZE (256UL * 1024)

#ifndef FSCTL_MARK_VOLUME_DIRTY
#define FSCTL_MARK_VOLUME_DIRTY CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static HANDLE Volume = INVALID_HANDLE_VALUE;
static EXU8 *Bounce;

static void Usage(void)
{
    printf("Checks an exFAT volume and repairs it (exfatnt.sys).\n\n"
           "EXFATCHK drive: [/F] [/X] [/V]\n"
           "EXFATCHK /INSTALL | /UNINSTALL\n\n");
    printf("  /F         Fix the problems found. The volume is locked meanwhile.\n"
           "  /X         Dismount the volume first if it is in use (implies /F).\n"
           "  /V         Name every file and directory as it is checked.\n"
           "  /INSTALL   Check exFAT volumes marked dirty at every restart\n"
           "             (copies exfachk.exe to System32 and adds it to BootExecute).\n"
           "  /UNINSTALL Stop checking at restart.\n");
}

/*
 * FSCTL_MARK_VOLUME_DIRTY. NT 3.1's DeviceIoControl passes on only the
 * file system controls it knows (lock, unlock, dismount): there the request
 * goes to NtFsControlFile directly.
 */
typedef LONG (WINAPI *EXF_NT_FS_CONTROL)(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG);
typedef ULONG (WINAPI *EXF_STATUS_TO_ERROR)(LONG);

static BOOL MarkDirty(HANDLE Handle)
{
    HMODULE Ntdll;
    EXF_NT_FS_CONTROL FsControl;
    EXF_STATUS_TO_ERROR ToError;
    PVOID Iosb[2];                  /* IO_STATUS_BLOCK: two pointer-sized fields */
    DWORD Bytes;
    LONG Status;

    if (DeviceIoControl(Handle, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0, &Bytes, NULL)) {
        return TRUE;
    }
    if (GetLastError() != ERROR_INVALID_FUNCTION) {
        return FALSE;
    }

    Ntdll = GetModuleHandleA("ntdll.dll");
    FsControl = Ntdll ? (EXF_NT_FS_CONTROL)GetProcAddress(Ntdll, "NtFsControlFile") : NULL;
    ToError = Ntdll ? (EXF_STATUS_TO_ERROR)GetProcAddress(Ntdll, "RtlNtStatusToDosError") : NULL;
    if (FsControl == NULL) {
        return FALSE;
    }

    Status = FsControl(Handle, NULL, NULL, NULL, Iosb, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0);
    if (Status < 0) {
        SetLastError(ToError ? ToError(Status) : ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    return TRUE;
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

/* ------------------------------------------------------------------ */
/* Host routines for the checker                                       */
/* ------------------------------------------------------------------ */

static int Seek(EXU64 Offset)
{
    LONG High = (LONG)(Offset >> 32);
    if (SetFilePointer(Volume, (LONG)(Offset & 0xFFFFFFFFUL), &High, FILE_BEGIN) == 0xFFFFFFFF &&
        GetLastError() != NO_ERROR) {
        return 0;
    }
    return 1;
}

static int HostRead(void *Context, EXU64 Offset, void *Buffer, EXU32 Length)
{
    DWORD Done;
    EXU32 At = 0, n;

    while (At < Length) {
        n = Length - At > BOUNCE_SIZE ? BOUNCE_SIZE : Length - At;
        if (!Seek(Offset + At) || !ReadFile(Volume, Bounce, n, &Done, NULL) || Done != n) {
            return 0;
        }
        memcpy((EXU8 *)Buffer + At, Bounce, n);
        At += n;
    }
    return 1;
}

static int HostWrite(void *Context, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    DWORD Done;
    EXU32 At = 0, n;

    while (At < Length) {
        n = Length - At > BOUNCE_SIZE ? BOUNCE_SIZE : Length - At;
        memcpy(Bounce, (const EXU8 *)Buffer + At, n);
        if (!Seek(Offset + At) || !WriteFile(Volume, Bounce, n, &Done, NULL) || Done != n) {
            return 0;
        }
        At += n;
    }
    return 1;
}

static void *HostAlloc(void *Context, EXU32 Size) { return calloc(1, Size); }
static void HostFree(void *Context, void *Memory) { free(Memory); }

static void HostPrint(void *Context, const EXU16 *Text, EXU32 Length)
{
    char Line[1400];
    int n = WideCharToMultiByte(CP_OEMCP, 0, (LPCWSTR)Text, (int)Length, Line, sizeof(Line) - 1, NULL, NULL);
    Line[n > 0 ? n : 0] = 0;
    printf("%s\n", Line);
}

/* ------------------------------------------------------------------ */
/* Boot-time checking: BootExecute                                     */
/* ------------------------------------------------------------------ */

#define SESSION_KEY "SYSTEM\\CurrentControlSet\\Control\\Session Manager"
#define BOOT_ENTRY  L"autocheck exfachk *"

/* A 32-bit program on 64-bit Windows (IsWow64Process: XP SP2, x64 and later) */
static int UnderWow64(void)
{
    typedef BOOL (WINAPI *ISWOW64)(HANDLE, BOOL *);
    ISWOW64 IsWow64 = (ISWOW64)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    BOOL Wow = FALSE;

    return IsWow64 != NULL && IsWow64(GetCurrentProcess(), &Wow) && Wow;
}

static int Install(int Add)
{
    HKEY Key;
    WCHAR *Old, *New, *p, *q;
    DWORD Type, Size = 0;
    char Here[MAX_PATH], To[MAX_PATH], *Slash;
    int Found = 0;
    LONG Error;

    if (Add && UnderWow64()) {
        /* System32 would be redirected and a 32-bit native program cannot run */
        printf("On 64-bit Windows use the x64 build of exfatchk.exe and exfachk.exe.\n");
        return 1;
    }

    if (Add) {
        /* exfachk.exe comes from next to this program */
        GetModuleFileNameA(NULL, Here, sizeof(Here));
        Slash = strrchr(Here, '\\');
        strcpy(Slash ? Slash + 1 : Here, "exfachk.exe");
        GetSystemDirectoryA(To, sizeof(To));
        strcat(To, "\\exfachk.exe");
        if (_stricmp(Here, To) != 0 && !CopyFileA(Here, To, FALSE)) {
            Fail("Copying exfachk.exe to the system directory");
            return 1;
        }
    }

    Error = RegOpenKeyExA(HKEY_LOCAL_MACHINE, SESSION_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        Fail("Opening the Session Manager key");
        return 1;
    }

    RegQueryValueExW(Key, L"BootExecute", NULL, &Type, NULL, &Size);
    Old = (WCHAR *)calloc(1, Size + 4 * sizeof(WCHAR));
    New = (WCHAR *)calloc(1, Size + sizeof(BOOT_ENTRY) + 4 * sizeof(WCHAR));
    if (Old == NULL || New == NULL) {
        RegCloseKey(Key);
        return 1;
    }
    if (Size != 0 && RegQueryValueExW(Key, L"BootExecute", NULL, &Type, (BYTE *)Old, &Size) != ERROR_SUCCESS) {
        Old[0] = 0;
    }

    /* Copy every string but ours; add ours at the end */
    q = New;
    for (p = Old; *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, BOOT_ENTRY) == 0) {
            Found = 1;
            continue;
        }
        wcscpy(q, p);
        q += wcslen(q) + 1;
    }
    if (Add) {
        wcscpy(q, BOOT_ENTRY);
        q += wcslen(q) + 1;
    }
    *q++ = 0;

    Error = RegSetValueExW(Key, L"BootExecute", 0, REG_MULTI_SZ, (BYTE *)New, (DWORD)((q - New) * sizeof(WCHAR)));
    RegCloseKey(Key);
    free(Old);
    free(New);

    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        Fail("Changing BootExecute");
        return 1;
    }

    if (Add) {
        printf("exFAT volumes marked dirty are now checked and repaired at every restart.\n");
    } else {
        printf(Found ? "exFAT volumes are no longer checked at restart.\n"
                     : "exfachk was not in BootExecute.\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static HANDLE OpenVolume(const char *Path, int Write)
{
    return CreateFileA(Path, GENERIC_READ | (Write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

int main(int argc, char **argv)
{
    EXC_PARAMS Params;
    EXC_HOST Host;
    EXC_RESULT Result;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    char Drive = 0, Path[16], WinDir[MAX_PATH], Line[16];
    int Fix = 0, Force = 0, Verbose = 0, Locked = 0, Status, i;
    DWORD Bytes;
    EXU64 Free, Size;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') {
            if (Drive != 0 || !isalpha((unsigned char)a[0]) || a[1] != ':' || a[2] != 0) {
                Usage();
                return 3;
            }
            Drive = (char)toupper((unsigned char)a[0]);
        } else if (_stricmp(a + 1, "F") == 0) {
            Fix = 1;
        } else if (_stricmp(a + 1, "X") == 0) {
            Fix = Force = 1;
        } else if (_stricmp(a + 1, "V") == 0) {
            Verbose = 1;
        } else if (_stricmp(a + 1, "INSTALL") == 0) {
            return Install(1);
        } else if (_stricmp(a + 1, "UNINSTALL") == 0) {
            return Install(0);
        } else {
            Usage();
            return 3;
        }
    }

    if (Drive == 0) {
        Usage();
        return 3;
    }

    if (Fix && GetWindowsDirectoryA(WinDir, sizeof(WinDir)) && toupper((unsigned char)WinDir[0]) == Drive) {
        printf("%c: holds Windows and cannot be locked for repair.\n", Drive);
        return 3;
    }

    Bounce = (EXU8 *)VirtualAlloc(NULL, BOUNCE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (Bounce == NULL) {
        printf("Not enough memory.\n");
        return 3;
    }

    sprintf(Path, "\\\\.\\%c:", Drive);
    Volume = OpenVolume(Path, Fix);
    if (Volume == INVALID_HANDLE_VALUE) {
        Fail("Opening the volume");
        return 3;
    }

    memset(&Params, 0, sizeof(Params));

    if (DeviceIoControl(Volume, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Geometry, sizeof(Geometry), &Bytes, NULL)) {
        Params.SectorSize = Geometry.BytesPerSector;
        if (DeviceIoControl(Volume, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Partition, sizeof(Partition), &Bytes, NULL)) {
            Params.VolumeSectors = (EXU64)Partition.PartitionLength.QuadPart / Geometry.BytesPerSector;
        }
    }

    if (Fix) {
        if (!DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL)) {

            if (Force) {
                if (!DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL)) {
                    Fail("Dismounting the volume");
                    CloseHandle(Volume);
                    return 3;
                }
                CloseHandle(Volume);
                /* NT 3.1 fails the first open after a dismount while it mounts again */
                Volume = OpenVolume(Path, 1);
                if (Volume == INVALID_HANDLE_VALUE) {
                    Volume = OpenVolume(Path, 1);
                }
                if (Volume == INVALID_HANDLE_VALUE) {
                    Fail("Opening the volume");
                    return 3;
                }
                Locked = DeviceIoControl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
            }

            if (!Locked) {
                printf("%c: is in use and cannot be locked.\n"
                       "Check and repair it at the next restart (Y/N)? ", Drive);
                fflush(stdout);
                if (fgets(Line, sizeof(Line), stdin) != NULL && (Line[0] == 'y' || Line[0] == 'Y')) {
                    if (MarkDirty(Volume)) {
                        printf("%c: will be checked at the next restart", Drive);
                        printf(" (if exfachk is installed: EXFINST or EXFATCHK /INSTALL).\n");
                        CloseHandle(Volume);
                        return 2;
                    }
                    Fail("Marking the volume dirty");
                }
                CloseHandle(Volume);
                return 3;
            }
        } else {
            Locked = 1;
        }
    }

    memset(&Host, 0, sizeof(Host));
    Host.Read = HostRead;
    Host.Write = HostWrite;
    Host.Alloc = HostAlloc;
    Host.Free = HostFree;
    Host.Print = HostPrint;
    Params.Fix = Fix;
    Params.Verbose = Verbose;

    printf("Checking %c:%s\n", Drive, Fix ? "" : " (read only: nothing is changed)");
    Status = ExcCheck(&Params, &Host, &Result);

    if (Fix) {
        FlushFileBuffers(Volume);
    }
    if (Locked) {
        /* After the repair the file system mounts the volume again */
        DeviceIoControl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
        DeviceIoControl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL);
    }
    CloseHandle(Volume);

    if (Status != EXC_FAILED) {
        Size = (EXU64)Result.ClusterCount * Result.ClusterSize;
        Free = (EXU64)(Result.ClusterCount - Result.UsedClusters) * Result.ClusterSize;
        printf("\n%10lu KB total disk space.\n", (unsigned long)(Size >> 10));
        printf("%10lu KB in %lu files.\n", (unsigned long)(Result.FileBytes >> 10), (unsigned long)Result.Files);
        printf("%10lu directories.\n", (unsigned long)Result.Directories);
        printf("%10lu KB available.\n", (unsigned long)(Free >> 10));
        printf("%10lu bytes in each allocation unit.\n\n", (unsigned long)Result.ClusterSize);
    }

    switch (Status) {
    case EXC_CLEAN:
        printf("No problems were found.\n");
        break;
    case EXC_FIXED:
        printf("%lu problems were found and fixed.\n", (unsigned long)Result.Problems);
        break;
    case EXC_ERRORS:
        if (Fix) {
            printf("%lu problems were found, %lu of them could not be fixed.\n",
                   (unsigned long)Result.Problems, (unsigned long)(Result.Problems - Result.Fixed));
        } else {
            printf("%lu problems were found. Run EXFATCHK %c: /F to fix them.\n",
                   (unsigned long)Result.Problems, Drive);
            printf("(On a volume in use, files being written can show as problems.)\n");
        }
        break;
    default:
        printf("The volume could not be checked.\n");
        break;
    }

    return Status;
}
