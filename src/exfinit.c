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

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
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
