/*
 * EXFATNT - exfinst: installs or removes the EXFATNT file system driver.
 *
 *   exfinst             Install: copy exfatnt.sys (from the folder exfinst.exe
 *                       is in) to System32\drivers and create the exfatnt
 *                       service (system start, group "File system").
 *   exfinst /READONLY   Mount exFAT volumes read-only.
 *   exfinst /U          Uninstall: disable the service and remove the driver.
 *
 * Everything is done with a file copy and the registry API, so it works on
 * Windows NT 3.1 too (whose registry cannot be edited offline, and whose
 * REGEDIT cannot import .reg files). A restart applies the change.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SVC_EXFATNT             "SYSTEM\\CurrentControlSet\\Services\\exfatnt"
#define SERVICE_SYSTEM_START    1
#define SERVICE_DISABLED        4
#define SERVICE_FILE_SYSTEM     2

static LONG SetDword(HKEY Key, const char *Name, DWORD Value)
{
    return RegSetValueExA(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

static LONG SetSz(HKEY Key, const char *Name, const char *Value)
{
    return RegSetValueExA(Key, Name, 0, REG_SZ, (const BYTE *)Value, (DWORD)(strlen(Value) + 1));
}

static int SystemPath(const char *Leaf, char *Out)
{
    char Dir[MAX_PATH];
    UINT N = GetWindowsDirectoryA(Dir, sizeof(Dir));
    if (N == 0 || N >= sizeof(Dir) - 40) {
        Out[0] = 0;
        return 0;
    }
    sprintf(Out, "%s\\System32\\drivers\\%s", Dir, Leaf);
    return 1;
}

/* exfatnt.sys next to this program, else in the current folder */
static void SourcePath(char *Out)
{
    char *Slash;
    DWORD N = GetModuleFileNameA(NULL, Out, MAX_PATH);
    if (N == 0 || N >= MAX_PATH - 16 || (Slash = strrchr(Out, '\\')) == NULL) {
        strcpy(Out, "exfatnt.sys");
        return;
    }
    strcpy(Slash + 1, "exfatnt.sys");
    if (GetFileAttributesA(Out) == 0xFFFFFFFF) strcpy(Out, "exfatnt.sys");
}

static int Install(int ReadOnly)
{
    char Sys[MAX_PATH], Src[MAX_PATH], Ms[MAX_PATH];
    HKEY Key;
    DWORD Disp;
    LONG R;

    if (!SystemPath("exfatnt.sys", Sys)) {
        printf("  cannot find the system directory.\n");
        return 1;
    }
    /* Windows XP SP2 and later have Microsoft's exFAT driver (KB955704) */
    if (SystemPath("exfat.sys", Ms) && GetFileAttributesA(Ms) != 0xFFFFFFFF) {
        printf("  this system has Microsoft's exFAT driver (exfat.sys); EXFATNT is not needed.\n");
        return 1;
    }
    SourcePath(Src);
    if (GetFileAttributesA(Src) == 0xFFFFFFFF) {
        printf("  error: exfatnt.sys not found; put it next to exfinst.exe.\n");
        return 1;
    }
    if (!CopyFileA(Src, Sys, FALSE)) {
        printf("  error: could not copy %s to %s (error %lu).\n", Src, Sys, GetLastError());
        return 1;
    }
    printf("  installed %s\n", Sys);

    R = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SVC_EXFATNT, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_ALL_ACCESS, NULL, &Key, &Disp);
    if (R != ERROR_SUCCESS) {
        printf("  error: cannot create the exfatnt service key (error %ld).\n", R);
        return 1;
    }
    SetDword(Key, "Type", SERVICE_FILE_SYSTEM);
    SetDword(Key, "Start", SERVICE_SYSTEM_START);
    SetDword(Key, "ErrorControl", 1);
    SetSz(Key, "Group", "File system");
    SetSz(Key, "ImagePath", "System32\\DRIVERS\\exfatnt.sys");
    SetDword(Key, "EnableWriteSupport", ReadOnly ? 0 : 1);
    RegFlushKey(Key);
    RegCloseKey(Key);

    printf("  exfatnt service: system start, %s\n", ReadOnly ? "read-only" : "read/write");
    printf("Done. Restart to load EXFATNT.\n");
    return 0;
}

static int Uninstall(void)
{
    char Sys[MAX_PATH];
    HKEY Key;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, SVC_EXFATNT, 0, KEY_ALL_ACCESS, &Key) == ERROR_SUCCESS) {
        SetDword(Key, "Start", SERVICE_DISABLED);
        RegFlushKey(Key);
        RegCloseKey(Key);
        printf("  disabled the exfatnt service\n");
    }
    /* the running driver keeps its file until the restart */
    if (SystemPath("exfatnt.sys", Sys) && GetFileAttributesA(Sys) != 0xFFFFFFFF && !DeleteFileA(Sys)) {
        printf("  %s is in use; delete it after the restart.\n", Sys);
    }
    printf("Done. Restart to apply.\n");
    return 0;
}

int main(int argc, char **argv)
{
    int I, Remove = 0, ReadOnly = 0;

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if ((A[0] == '/' || A[0] == '-') && !_stricmp(A + 1, "U")) Remove = 1;
        else if ((A[0] == '/' || A[0] == '-') && !_stricmp(A + 1, "READONLY")) ReadOnly = 1;
        else {
            printf("usage: exfinst [/READONLY]\n"
                   "       exfinst /U\n");
            return 2;
        }
    }
    printf("EXFATNT installer\n");
    return Remove ? Uninstall() : Install(ReadOnly);
}
