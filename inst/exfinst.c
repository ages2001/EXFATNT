/*
 * EXFINST - installs exfatnt.sys and the tools, on NT 3.1 to XP.
 *
 *   EXFINST /INSTALL [/READONLY | /READWRITE] [/NOCHECK]
 *   EXFINST /UNINSTALL
 *
 * Takes the files from its own folder: exfatnt.sys goes to
 * System32\drivers, exfmt.exe, exfatchk.exe, exfachk.exe and exfinst.exe
 * itself (when present) to System32. Creates the service key the driver needs and, with
 * exfachk.exe, adds the boot-time check to BootExecute. Nothing here
 * needs regedit, which cannot import .reg files on NT 3.1.
 */

#include <windows.h>
#ifdef EXF_OWN_CRT
#include "exfcrt.h"     /* NT build: no C library, runs on NT 3.1 too */
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#define SERVICE_KEY "SYSTEM\\CurrentControlSet\\Services\\exfatnt"
#define SESSION_KEY "SYSTEM\\CurrentControlSet\\Control\\Session Manager"
#define BOOT_ENTRY  L"autocheck exfachk *"

static void Usage(void)
{
    printf("Installs the exFAT file system driver (exfatnt.sys) and its tools.\n\n"
           "EXFINST /INSTALL [/READONLY | /READWRITE] [/NOCHECK]\n"
           "EXFINST /UNINSTALL\n\n");
    printf("  /INSTALL    Copies exfatnt.sys to System32\\drivers and exfmt.exe,\n"
           "              exfatchk.exe and exfachk.exe to System32 (those found next\n"
           "              to EXFINST), and registers the driver. Restart afterwards.\n"
           "  /READONLY   Mount exFAT volumes read-only (EnableWriteSupport = 0).\n"
           "  /READWRITE  Mount them read/write (EnableWriteSupport = 1, the default).\n"
           "  /NOCHECK    Do not check dirty exFAT volumes at restart.\n"
           "  /UNINSTALL  Disables the driver (Start = 4) and the check at restart.\n"
           "              The files stay where they are.\n");
}

static void Fail(const char *What, DWORD Error)
{
    char Text[256];

    Text[0] = 0;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error,
                   0, Text, sizeof(Text), NULL);
    printf("%s failed (error %lu): %s\n", What, (unsigned long)Error, Text);
}

static int Exists(const char *Path)
{
    return GetFileAttributesA(Path) != 0xFFFFFFFF;
}

/* Copies Name from the program's folder to To\Name; 1 copied, 0 absent, -1 failed */
static int CopyOne(const char *From, const char *To, const char *Name)
{
    char Source[MAX_PATH], Target[MAX_PATH];

    if (strlen(From) + strlen(Name) + 2 > MAX_PATH || strlen(To) + strlen(Name) + 2 > MAX_PATH) {
        return -1;
    }
    strcpy(Source, From); strcat(Source, "\\"); strcat(Source, Name);
    strcpy(Target, To); strcat(Target, "\\"); strcat(Target, Name);

    if (!Exists(Source)) {
        return 0;
    }
    if (_stricmp(Source, Target) == 0) {
        return 1;
    }
    if (!CopyFileA(Source, Target, FALSE)) {
        DWORD Error = GetLastError();
        char Old[MAX_PATH], *Dot;

        /*
         * A loaded driver (or a running program) cannot be overwritten, but
         * it can be renamed: the new one is used from the next restart.
         */
        if (Error == ERROR_SHARING_VIOLATION || Error == ERROR_ACCESS_DENIED) {
            strcpy(Old, Target);
            Dot = strrchr(Old, '.');
            if (Dot != NULL && strrchr(Old, '\\') < Dot) {
                strcpy(Dot, ".old");
                DeleteFileA(Old);
                if (MoveFileA(Target, Old)) {
                    if (CopyFileA(Source, Target, FALSE)) {
                        printf("  %s -> %s\n    (the one in use was renamed %s)\n", Name, To, strrchr(Old, '\\') + 1);
                        return 1;
                    }
                    Error = GetLastError();
                    MoveFileA(Old, Target);
                } else {
                    Error = GetLastError();
                }
            }
        }
        printf("Copying %s to %s: ", Name, To);
        Fail("CopyFile", Error);
        return -1;
    }
    printf("  %s -> %s\n", Name, To);
    return 1;
}

static LONG SetDword(HKEY Key, const char *Name, DWORD Value)
{
    return RegSetValueExA(Key, Name, 0, REG_DWORD, (BYTE *)&Value, sizeof(Value));
}

/* Adds or removes "autocheck exfachk *" in BootExecute */
static int BootExecute(int Add)
{
    HKEY Key;
    WCHAR *Old, *New, *p, *q;
    DWORD Type, Size = 0;
    LONG Error;

    Error = RegOpenKeyExA(HKEY_LOCAL_MACHINE, SESSION_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key);
    if (Error != ERROR_SUCCESS) {
        Fail("Opening the Session Manager key", (DWORD)Error);
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

    /* Every string but ours, then ours at the end */
    q = New;
    for (p = Old; *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, BOOT_ENTRY) == 0) {
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
        Fail("Changing BootExecute", (DWORD)Error);
        return 1;
    }
    return 0;
}

/* A 32-bit program on 64-bit Windows: System32 would be redirected */
static int UnderWow64(void)
{
    typedef BOOL (WINAPI *ISWOW64)(HANDLE, BOOL *);
    ISWOW64 IsWow64 = (ISWOW64)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    BOOL Wow = FALSE;

    return IsWow64 != NULL && IsWow64(GetCurrentProcess(), &Wow) && Wow;
}

static int Install(int Write, int Check)
{
    char Here[MAX_PATH], System[MAX_PATH], Drivers[MAX_PATH], *Slash;
    HKEY Key;
    DWORD Disposition, Value, Size, Type;
    LONG Error;
    int Checker = 0;

    if (UnderWow64()) {
        printf("On 64-bit Windows use the x64 build of exfinst.exe.\n");
        return 1;
    }

    GetModuleFileNameA(NULL, Here, sizeof(Here));
    Slash = strrchr(Here, '\\');
    if (Slash != NULL) {
        *Slash = 0;
    }
    GetSystemDirectoryA(System, sizeof(System) - 16);
    strcpy(Drivers, System);
    strcat(Drivers, "\\drivers");

    printf("Copying files:\n");
    switch (CopyOne(Here, Drivers, "exfatnt.sys")) {
    case 0:
        printf("exfatnt.sys is not in %s.\n", Here);
        return 1;
    case -1:
        return 1;
    }
    if (CopyOne(Here, System, "exfmt.exe") < 0 || CopyOne(Here, System, "exfatchk.exe") < 0 ||
        CopyOne(Here, System, "exfinst.exe") < 0) {
        return 1;
    }
    if (Check) {
        Checker = CopyOne(Here, System, "exfachk.exe");
        if (Checker < 0) {
            return 1;
        }
    }

    Error = RegCreateKeyExA(HKEY_LOCAL_MACHINE, SERVICE_KEY, 0, "", REG_OPTION_NON_VOLATILE,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &Key, &Disposition);
    if (Error != ERROR_SUCCESS) {
        Fail("Creating the service key", (DWORD)Error);
        return 1;
    }

    Error = SetDword(Key, "Type", 2);                   /* file system driver */
    if (Error == ERROR_SUCCESS) Error = SetDword(Key, "Start", 1);         /* system */
    if (Error == ERROR_SUCCESS) Error = SetDword(Key, "ErrorControl", 1);  /* normal */
    if (Error == ERROR_SUCCESS) {
        Error = RegSetValueExA(Key, "Group", 0, REG_SZ, (BYTE *)"File System", sizeof("File System"));
    }

    /* Keep an existing setting unless one was asked for */
    Size = sizeof(Value);
    if (Error == ERROR_SUCCESS &&
        (Write >= 0 || RegQueryValueExA(Key, "EnableWriteSupport", NULL, &Type, (BYTE *)&Value, &Size) != ERROR_SUCCESS)) {
        Error = SetDword(Key, "EnableWriteSupport", Write != 0 ? 1 : 0);
    }
    RegCloseKey(Key);

    if (Error != ERROR_SUCCESS) {
        Fail("Writing the service key", (DWORD)Error);
        return 1;
    }

    if (Checker > 0) {
        if (BootExecute(1) != 0) {
            return 1;
        }
    } else if (!Check) {
        (VOID)BootExecute(0);
    }

    printf("\nThe exFAT driver is installed%s.\n", Write == 0 ? " (read-only)" : "");
    if (Checker > 0) {
        printf("Dirty exFAT volumes are checked at every restart.\n");
    }
    printf("Restart Windows to load the driver.\n");
    return 0;
}

static int Uninstall(void)
{
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExA(HKEY_LOCAL_MACHINE, SERVICE_KEY, 0, KEY_SET_VALUE, &Key);
    if (Error == ERROR_FILE_NOT_FOUND) {
        printf("The exFAT driver is not installed.\n");
    } else if (Error != ERROR_SUCCESS) {
        Fail("Opening the service key", (DWORD)Error);
        return 1;
    } else {
        Error = SetDword(Key, "Start", 4);
        RegCloseKey(Key);
        if (Error != ERROR_SUCCESS) {
            Fail("Disabling the driver", (DWORD)Error);
            return 1;
        }
    }

    if (BootExecute(0) != 0) {
        return 1;
    }

    printf("The exFAT driver is disabled from the next restart.\n");
    return 0;
}

int main(int argc, char **argv)
{
    int Action = 0, Write = -1, Check = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "/INSTALL") == 0) Action = 1;
        else if (_stricmp(argv[i], "/UNINSTALL") == 0) Action = 2;
        else if (_stricmp(argv[i], "/READONLY") == 0) Write = 0;
        else if (_stricmp(argv[i], "/READWRITE") == 0) Write = 1;
        else if (_stricmp(argv[i], "/NOCHECK") == 0) Check = 0;
        else {
            Usage();
            return 1;
        }
    }

    switch (Action) {
    case 1:
        return Install(Write, Check);
    case 2:
        return Uninstall();
    default:
        Usage();
        return 1;
    }
}
