/*
 * EXFATNT - IRP dispatch, exception handling and volume verification
 *
 * Every request is handled synchronously in the caller's thread. Handlers
 * return a status and leave completion to ExfFsdDispatch unless they set
 * EXF_CTX_NO_COMPLETE.
 */

#include "exfat.h"

static LONG
ExfExceptionFilter (
    PEXF_IRP_CONTEXT Ctx,
    PEXCEPTION_POINTERS Pointers
    )
{
    NTSTATUS Code = Pointers->ExceptionRecord->ExceptionCode;

    if (Code == STATUS_IN_PAGE_ERROR) {

        if (Pointers->ExceptionRecord->NumberParameters >= 3) {
            Code = (NTSTATUS)Pointers->ExceptionRecord->ExceptionInformation[2];
        }

        Ctx->ExceptionStatus = FsRtlNormalizeNtstatus(Code, STATUS_UNEXPECTED_IO_ERROR);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    if (Code == STATUS_ACCESS_VIOLATION || Code == STATUS_DATATYPE_MISALIGNMENT) {

        /* Tolerated only while touching a caller's buffer */
        if (Ctx->Irp->RequestorMode != UserMode) {

            EXF_DBG((EXF_PFX "Exception %lX at %p\n",
                     Code, Pointers->ExceptionRecord->ExceptionAddress));

            return EXCEPTION_CONTINUE_SEARCH;
        }

        Ctx->ExceptionStatus = STATUS_INVALID_USER_BUFFER;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    if (!FsRtlIsNtstatusExpected(Code)) {

        EXF_DBG((EXF_PFX "Unexpected exception %lX at %p\n",
                 Code, Pointers->ExceptionRecord->ExceptionAddress));

        return EXCEPTION_CONTINUE_SEARCH;
    }

    Ctx->ExceptionStatus = Code;
    return EXCEPTION_EXECUTE_HANDLER;
}

static NTSTATUS
ExfCommonDispatch (
    PEXF_IRP_CONTEXT Ctx
    )
{
    UCHAR Major = Ctx->IrpSp->MajorFunction;

    /* The file system device itself */
    if (Ctx->Vcb == NULL) {

        switch (Major) {

        case IRP_MJ_CREATE:
            Ctx->IrpSp->FileObject->FsContext = NULL;
            Ctx->IrpSp->FileObject->FsContext2 = NULL;
            Ctx->Irp->IoStatus.Information = FILE_OPENED;
            return STATUS_SUCCESS;

        case IRP_MJ_CLEANUP:
        case IRP_MJ_CLOSE:
            return STATUS_SUCCESS;

        case IRP_MJ_FILE_SYSTEM_CONTROL:
            return ExfCommonFileSystemControl(Ctx);

        case IRP_MJ_SHUTDOWN:
            return ExfCommonShutdown(Ctx);

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    switch (Major) {

    case IRP_MJ_CREATE:
        return ExfCommonCreate(Ctx);

    case IRP_MJ_CLOSE:
        return ExfCommonClose(Ctx);

    case IRP_MJ_CLEANUP:
        return ExfCommonCleanup(Ctx);

    case IRP_MJ_READ:
        return ExfCommonRead(Ctx);

    case IRP_MJ_WRITE:
        return ExfCommonWrite(Ctx);

    case IRP_MJ_QUERY_INFORMATION:
        return ExfCommonQueryInformation(Ctx);

    case IRP_MJ_SET_INFORMATION:
        return ExfCommonSetInformation(Ctx);

    case IRP_MJ_QUERY_VOLUME_INFORMATION:
        return ExfCommonQueryVolumeInformation(Ctx);

    case IRP_MJ_SET_VOLUME_INFORMATION:
        return ExfCommonSetVolumeInformation(Ctx);

    case IRP_MJ_DIRECTORY_CONTROL:
        return ExfCommonDirectoryControl(Ctx);

    case IRP_MJ_FILE_SYSTEM_CONTROL:
        return ExfCommonFileSystemControl(Ctx);

    case IRP_MJ_DEVICE_CONTROL:
        return ExfCommonDeviceControl(Ctx);

    case IRP_MJ_LOCK_CONTROL:
        return ExfCommonLockControl(Ctx);

    case IRP_MJ_FLUSH_BUFFERS:
        return ExfCommonFlushBuffers(Ctx);

    case IRP_MJ_QUERY_EA:
    case IRP_MJ_SET_EA:
        return STATUS_EAS_NOT_SUPPORTED;

#ifndef EXF_NT4
    case IRP_MJ_PNP:
        return ExfCommonPnp(Ctx);
#endif

    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static NTSTATUS
ExfGuardedDispatch (
    PEXF_IRP_CONTEXT Ctx
    )
{
    NTSTATUS Status;

    __try {

        Status = ExfCommonDispatch(Ctx);

    } __except (ExfExceptionFilter(Ctx, GetExceptionInformation())) {

        Status = Ctx->ExceptionStatus;
    }

    return Status;
}

static BOOLEAN
ExfCanRetry (
    PEXF_IRP_CONTEXT Ctx
    )
{
    if ((Ctx->Flags & EXF_CTX_NO_COMPLETE) || (Ctx->Irp->Flags & IRP_PAGING_IO)) {
        return FALSE;
    }

    /* Every handler below checks the volume before changing anything */
    switch (Ctx->IrpSp->MajorFunction) {

    case IRP_MJ_CREATE:
    case IRP_MJ_READ:
    case IRP_MJ_WRITE:
    case IRP_MJ_QUERY_INFORMATION:
    case IRP_MJ_SET_INFORMATION:
    case IRP_MJ_QUERY_VOLUME_INFORMATION:
    case IRP_MJ_SET_VOLUME_INFORMATION:
    case IRP_MJ_DIRECTORY_CONTROL:
    case IRP_MJ_FLUSH_BUFFERS:
        return TRUE;

    case IRP_MJ_FILE_SYSTEM_CONTROL:
        return (BOOLEAN)(Ctx->IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST);
    }

    return FALSE;
}

/*
 * The device reported a media change. Let the I/O manager run a verify
 * (IRP_MN_VERIFY_VOLUME comes back to us) so the request can be retried.
 */
static NTSTATUS
ExfPerformVerify (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PDEVICE_OBJECT Device;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;

    Device = IoGetDeviceToVerify(PsGetCurrentThread());
    IoSetDeviceToVerify(PsGetCurrentThread(), NULL);

    if (Device == NULL) {

        if (Ctx->Vcb == NULL) {
            return STATUS_VERIFY_REQUIRED;
        }

        Device = Ctx->Vcb->Vpb->RealDevice;
    }

    Status = IoVerifyVolume(Device, FALSE);

    /* A different volume is in the drive: have the create reparsed */
    FileObject = Ctx->IrpSp->FileObject;

#ifndef EXF_NT31
    /* NT 3.1 has no IO_REMOUNT: the open fails and is retried by the caller */
    if (Ctx->IrpSp->MajorFunction == IRP_MJ_CREATE &&
        FileObject != NULL &&
        FileObject->RelatedFileObject == NULL &&
        (Status == STATUS_WRONG_VOLUME ||
         (Ctx->Vcb != NULL && (Ctx->Vcb->VcbState & VCB_STATE_DISMOUNTED)))) {

        Ctx->Irp->IoStatus.Information = IO_REMOUNT;
        return STATUS_REPARSE;
    }
#else
    UNREFERENCED_PARAMETER(FileObject);
#endif

    return Status;
}

NTSTATUS
NTAPI
ExfFsdDispatch (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    EXF_IRP_CONTEXT Ctx;
    NTSTATUS Status;
    NTSTATUS VerifyStatus;
    BOOLEAN TopLevel = FALSE;
    ULONG Attempts = 0;

    FsRtlEnterFileSystem();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp(Irp);
        TopLevel = TRUE;
    }

    RtlZeroMemory(&Ctx, sizeof(Ctx));
    Ctx.Irp = Irp;
    Ctx.IrpSp = IoGetCurrentIrpStackLocation(Irp);
    Ctx.DeviceObject = DeviceObject;

    if (DeviceObject != ExfData.FileSystemDeviceObject) {
        Ctx.Vcb = (PEXF_VCB)DeviceObject->DeviceExtension;
    }

    if (TopLevel) {
        Ctx.Flags |= EXF_CTX_TOP_LEVEL;
    }

    for (;;) {

        Status = ExfGuardedDispatch(&Ctx);

        if (Status != STATUS_VERIFY_REQUIRED || !TopLevel ||
            Attempts >= 2 || !ExfCanRetry(&Ctx)) {

            break;
        }

        Attempts++;

        VerifyStatus = ExfPerformVerify(&Ctx);

        if (VerifyStatus == STATUS_REPARSE || !NT_SUCCESS(VerifyStatus)) {
            Status = VerifyStatus;
            break;
        }

        Irp->IoStatus.Information = 0;
    }

    if (!(Ctx.Flags & EXF_CTX_NO_COMPLETE)) {

        if (Status == STATUS_VERIFY_REQUIRED && Ctx.Vcb != NULL &&
            IoGetDeviceToVerify(PsGetCurrentThread()) == NULL) {

            IoSetHardErrorOrVerifyDevice(Irp, Ctx.Vcb->Vpb->RealDevice);
        }

        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, (CCHAR)(NT_SUCCESS(Status) ? IO_DISK_INCREMENT : IO_NO_INCREMENT));
    }

    if (TopLevel) {
        IoSetTopLevelIrp(NULL);
    }

    FsRtlExitFileSystem();

    return Status;
}

/*
 * Fails requests on a dismounted volume and asks for a verify when the
 * device has flagged a media change.
 */
NTSTATUS
ExfVerifyVcb (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_VCB Vcb
    )
{
    PDEVICE_OBJECT RealDevice = Vcb->Vpb->RealDevice;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;

    if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {

        /*
         * Files were still open at the dismount, so the VPB still leads
         * here. A new open asks for a verify: it fails, the I/O manager
         * gives the device a fresh VPB and the open is reparsed onto a
         * new mount.
         */
        if (Ctx->IrpSp->MajorFunction == IRP_MJ_CREATE &&
            FileObject != NULL && FileObject->RelatedFileObject == NULL &&
            RealDevice->Vpb == Vcb->Vpb) {

            IoSetHardErrorOrVerifyDevice(Ctx->Irp, RealDevice);
            return STATUS_VERIFY_REQUIRED;
        }

        return EXF_STATUS_DISMOUNTED;
    }

    if ((RealDevice->Flags & DO_VERIFY_VOLUME) &&
        !(Ctx->IrpSp->Flags & SL_OVERRIDE_VERIFY_VOLUME) &&
        Vcb->VerifyThread != KeGetCurrentThread()) {

        IoSetHardErrorOrVerifyDevice(Ctx->Irp, RealDevice);
        return STATUS_VERIFY_REQUIRED;
    }

    return STATUS_SUCCESS;
}

/* ExfVerifyVcb, and the volume must accept changes */
NTSTATUS
ExfVerifyWritable (
    PEXF_IRP_CONTEXT Ctx,
    PEXF_VCB Vcb
    )
{
    NTSTATUS Status;

    Status = ExfVerifyVcb(Ctx, Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    return STATUS_SUCCESS;
}
