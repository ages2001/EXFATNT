/*
 * EXFATNT - driver entry and settings
 *
 * Settings live in the service key and are read at every mount, so a
 * change applies to volumes mounted afterwards:
 *
 *   EnableWriteSupport (REG_DWORD)  1 or missing: read/write, 0: read-only
 */

#include "exfat.h"

EXF_DATA ExfData;

#if defined(EXF_NT31) && !defined(FAKE_NTIFS_H)

EXF_NT31_KERNEL ExfNt31;

/* Follows an import thunk (jmp dword ptr [iat]) to the kernel's code */
static PUCHAR
ExfCodeOf (
    PVOID Routine
    )
{
    PUCHAR Code = (PUCHAR)Routine;

    if (Code[0] == 0xFF && Code[1] == 0x25) {
        Code = **(PUCHAR **)(Code + 2);
    }

    return Code;
}

static BOOLEAN
ExfThreadOffsetOk (
    ULONG Offset
    )
{
    return (BOOLEAN)(Offset >= 0x100 && Offset < 0x400 && (Offset & 3) == 0);
}

/* The kernel image around a routine it exports: the page with its headers */
static PUCHAR
ExfKernelBase (
    PUCHAR Inside
    )
{
    PUCHAR Page = (PUCHAR)((ULONG)Inside & ~(ULONG)(PAGE_SIZE - 1));
    ULONG Pages;
    ULONG Header;

    for (Pages = 0; Pages < 0x1000; Pages++, Page -= PAGE_SIZE) {
        if (Page[0] == 'M' && Page[1] == 'Z') {
            Header = *(ULONG UNALIGNED *)(Page + 0x3C);
            if (Header >= 0x40 && Header < PAGE_SIZE - 0xF8 &&
                *(ULONG UNALIGNED *)(Page + Header) == 0x00004550) {     /* "PE\0\0" */
                return Page;
            }
        }
    }

    return NULL;
}

/* GetProcAddress for ntoskrnl.exe: NT 3.1 has no routine for it */
static PVOID
ExfKernelExport (
    PUCHAR Base,
    PCSTR Name
    )
{
    ULONG Header = *(ULONG UNALIGNED *)(Base + 0x3C);
    ULONG ExportRva = *(ULONG UNALIGNED *)(Base + Header + 0x78);   /* DataDirectory[0] */
    PUCHAR Exports;
    PULONG Functions;
    PULONG Names;
    PUSHORT Ordinals;
    ULONG Count;
    ULONG i;
    ULONG k;

    if (ExportRva == 0) {
        return NULL;
    }

    Exports = Base + ExportRva;
    Count = *(PULONG)(Exports + 0x18);
    Functions = (PULONG)(Base + *(PULONG)(Exports + 0x1C));
    Names = (PULONG)(Base + *(PULONG)(Exports + 0x20));
    Ordinals = (PUSHORT)(Base + *(PULONG)(Exports + 0x24));

    for (i = 0; i < Count; i++) {
        PCSTR Export = (PCSTR)(Base + Names[i]);

        for (k = 0; Name[k] != 0 && Name[k] == Export[k]; k++) {
        }

        if (Name[k] == 0 && Export[k] == 0) {
            return Base + Functions[Ordinals[i]];
        }
    }

    return NULL;
}

/*
 * NT 3.1 has the thread fields but not the routines that reach them.
 * The offsets are read from two exported routines that use them:
 *
 *   IoSetHardErrorOrVerifyDevice:  mov ecx, [eax+50h]   (Irp->Tail.Overlay.Thread)
 *                                  ...
 *                                  mov [ecx+disp32], eax (DeviceToVerify)
 *   KeLeaveCriticalRegion:         mov eax, [reg+disp32]
 *                                  inc eax               (KernelApcDisable)
 *
 * TopLevelIrp is the field before DeviceToVerify, as NT 3.1's FASTFAT uses it.
 * The three routines the NT4 import library cannot link come from the
 * kernel's export table.
 */
NTSTATUS
ExfFindNt31Kernel (
    VOID
    )
{
    PUCHAR Code;
    PUCHAR Base;
    ULONG i;

    RtlZeroMemory(&ExfNt31, sizeof(ExfNt31));

    Code = ExfCodeOf((PVOID)IoSetHardErrorOrVerifyDevice);
    for (i = 0; i + 9 < 32; i++) {
        if (Code[i] == 0x8B && Code[i + 1] == 0x48 && Code[i + 2] == 0x50) {
            for (i += 3; i + 6 < 32; i++) {
                if (Code[i] == 0x89 && Code[i + 1] == 0x81) {
                    ExfNt31.DeviceToVerify = *(ULONG UNALIGNED *)(Code + i + 2);
                    break;
                }
            }
            break;
        }
    }

    Code = ExfCodeOf((PVOID)KeLeaveCriticalRegion);
    for (i = 0; i + 7 < 48; i++) {
        if (Code[i] == 0x8B && (Code[i + 1] & 0xF8) == 0x80 && (Code[i + 1] & 7) != 4 &&
            Code[i + 6] == 0x40) {
            ExfNt31.KernelApcDisable = *(ULONG UNALIGNED *)(Code + i + 2);
            break;
        }
    }

    if (!ExfThreadOffsetOk(ExfNt31.DeviceToVerify) ||
        !ExfThreadOffsetOk(ExfNt31.KernelApcDisable)) {

        return STATUS_NOT_SUPPORTED;
    }

    ExfNt31.TopLevelIrp = ExfNt31.DeviceToVerify - sizeof(PVOID);

    Base = ExfKernelBase(Code);
    if (Base == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    ExfNt31.AcquireResourceShared =
        (PEXF31_ACQUIRE_SHARED)ExfKernelExport(Base, "ExAcquireResourceShared");
    ExfNt31.FsRtlCopyRead = ExfKernelExport(Base, "FsRtlCopyRead");
    ExfNt31.FsRtlCopyWrite = ExfKernelExport(Base, "FsRtlCopyWrite");

    if (ExfNt31.AcquireResourceShared == NULL ||
        ExfNt31.FsRtlCopyRead == NULL ||
        ExfNt31.FsRtlCopyWrite == NULL) {

        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

#endif

/* Keeps a NUL-terminated copy of the service key path */
static VOID
ExfSaveRegistryPath (
    PUNICODE_STRING RegistryPath
    )
{
    USHORT Size;

    if (RegistryPath == NULL || RegistryPath->Length == 0) {
        return;
    }

    Size = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    ExfData.RegistryPath.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, Size, EXF_TAG_REGISTRY);

    if (ExfData.RegistryPath.Buffer != NULL) {
        RtlCopyMemory(ExfData.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        ExfData.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        ExfData.RegistryPath.Length = RegistryPath->Length;
        ExfData.RegistryPath.MaximumLength = Size;
    }
}

/* EnableWriteSupport: nonzero (the default) lets volumes be written */
BOOLEAN
ExfWriteSupportEnabled (
    VOID
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 1;
    ULONG Default = 1;

    if (ExfData.RegistryPath.Buffer == NULL) {
        return TRUE;
    }

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = L"EnableWriteSupport";
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, ExfData.RegistryPath.Buffer,
                                           Table, NULL, NULL))) {
        return TRUE;
    }

    return (BOOLEAN)(Value != 0);
}

NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

#if defined(EXF_NT31) && !defined(FAKE_NTIFS_H)
    /* Not NT 3.1, or a kernel this driver does not know */
    Status = ExfFindNt31Kernel();
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    EXF_DBG((EXF_PFX "NT 3.1 thread fields: ApcDisable %lX TopLevelIrp %lX DeviceToVerify %lX\n",
             ExfNt31.KernelApcDisable, ExfNt31.TopLevelIrp, ExfNt31.DeviceToVerify));
#endif

    RtlZeroMemory(&ExfData, sizeof(ExfData));

    ExfSaveRegistryPath(RegistryPath);

    RtlInitUnicodeString(&Name, EXF_FS_DEVICE_NAME);

    Status = IoCreateDevice(DriverObject,
                            0,
                            &Name,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status)) {
        EXF_DBG((EXF_PFX "IoCreateDevice failed %lX\n", Status));
        return Status;
    }

    ExfData.DriverObject = DriverObject;
    ExfData.FileSystemDeviceObject = DeviceObject;

    ExInitializeResourceLite(&ExfData.Resource);
    InitializeListHead(&ExfData.VcbList);

    for (i = 0; i <= EXF_MAX_MAJOR_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = ExfFsdDispatch;
    }

    ExfInitializeFastIo(&ExfData.FastIoDispatch);
    DriverObject->FastIoDispatch = &ExfData.FastIoDispatch;

    ExfInitializeCacheCallbacks();

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Volumes are left clean at shutdown */
    (VOID)IoRegisterShutdownNotification(DeviceObject);

    IoRegisterFileSystem(DeviceObject);

    EXF_DBG((EXF_PFX "Loaded\n"));

    return STATUS_SUCCESS;
}
