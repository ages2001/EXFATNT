/*
 * EXFATNT - IRP_MJ_DEVICE_CONTROL and IRP_MJ_LOCK_CONTROL
 */

#include "exfat.h"

/* Device requests on any of our handles go to the disk */
NTSTATUS
ExfCommonDeviceControl (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PEXF_CCB Ccb = (PEXF_CCB)FileObject->FsContext2;
    PEXF_VCB Vcb = Ctx->Vcb;

    if (FileObject->FsContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* After a removal the target device may be gone */
    if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) &&
        (Ccb == NULL || !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME))) {

        return EXF_STATUS_DISMOUNTED;
    }

    ExfSkipStack(Ctx->Irp);
    Ctx->Flags |= EXF_CTX_NO_COMPLETE;

    return IoCallDriver(Vcb->TargetDeviceObject, Ctx->Irp);
}

NTSTATUS
ExfCommonLockControl (
    PEXF_IRP_CONTEXT Ctx
    )
{
    PEXF_FCB Fcb = (PEXF_FCB)Ctx->IrpSp->FileObject->FsContext;
    NTSTATUS Status;

    if (Fcb == NULL || !ExfIsFcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = ExfVerifyVcb(Ctx, Fcb->Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);

    __try {

        /* Completes the IRP itself */
        Status = FsRtlProcessFileLock(&Fcb->FileLock, Ctx->Irp, NULL);
        Ctx->Flags |= EXF_CTX_NO_COMPLETE;

        Fcb->Header.IsFastIoPossible = ExfIsFastIoPossible(Fcb);

    } __finally {

        ExfRelease(&Fcb->Resource);
    }

    return Status;
}
