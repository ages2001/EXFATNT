/*
 * EXFATNT - Windows NT 3.1 support (EXF_NT31)
 *
 * Built with the same NT4 DDK headers and free ntifs.h as the NT 3.51/4.0
 * driver, then mapped onto what the NT 3.1 kernel exports:
 *
 *   - the original executive resources (ExAcquireResourceExclusive, ...),
 *     which are larger than the NT 3.5 "Lite" ones
 *   - no tagged pool, no fastcall Iof/Obf entry points
 *   - fast I/O routines without the trailing DeviceObject argument
 *   - no IoGet/SetTopLevelIrp, IoGet/SetDeviceToVerify or
 *     KeEnterCriticalRegion exports: the thread fields are used directly,
 *     at offsets read from the kernel when the driver loads
 *   - no MmCanFileBeTruncated, no FsRtlNotifyFull* (change notification
 *     is not offered), no IO_REMOUNT reparse
 *   - 22 major function slots in the driver object (up to IRP_MJ_SET_SECURITY)
 */

#ifndef _EXFNT31_H_
#define _EXFNT31_H_

/* Old-style ERESOURCE: 0x78 bytes on NT 3.1 (0x38 for the Lite ones) */
#define EXF_NT31_ERESOURCE_SIZE     0x78

typedef struct _EXF_ERESOURCE {
    ULONG Opaque[EXF_NT31_ERESOURCE_SIZE / sizeof(ULONG)];
} EXF_ERESOURCE, *PEXF_ERESOURCE;

/* The NT4 headers map the old names onto the Lite ones: undo that */
#undef ExInitializeResource
#undef ExDeleteResource
#undef ExAcquireResourceExclusive
#undef ExAcquireResourceShared
#undef ExReleaseResourceForThread
#undef ExReleaseResource
#undef ExIsResourceAcquiredExclusive
#undef ExIsResourceAcquiredShared
#undef ExInitializeResourceLite
#undef ExDeleteResourceLite
#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceForThreadLite
#undef ExReleaseResourceLite

#define ExInitializeResourceLite(R)         ExInitializeResource((PERESOURCE)(R))
#define ExDeleteResourceLite(R)             ExDeleteResource((PERESOURCE)(R))
#define ExAcquireResourceExclusiveLite(R,W) ExAcquireResourceExclusive((PERESOURCE)(R), (W))
#define ExReleaseResourceForThreadLite(R,T) ExReleaseResourceForThread((PERESOURCE)(R), (T))
#define ExReleaseResourceLite(R) \
    ExReleaseResourceForThread((PERESOURCE)(R), ExGetCurrentResourceThread())

/* Pool tags came with NT 3.5 */
#undef ExAllocatePool
#undef ExAllocatePoolWithTag
#define ExAllocatePoolWithTag(Type, Bytes, Tag)     ExAllocatePool((Type), (Bytes))

/* Change notification: not offered on NT 3.1 */
#undef FsRtlNotifyInitializeSync
#undef FsRtlNotifyUninitializeSync
#undef FsRtlNotifyCleanup
#undef FsRtlNotifyFullReportChange
#define FsRtlNotifyInitializeSync(PSync)            (*(PSync) = NULL)
#define FsRtlNotifyUninitializeSync(PSync)          ((VOID)0)
#define FsRtlNotifyCleanup(Sync, List, Context)     ((VOID)0)
#define FsRtlNotifyFullReportChange(a, b, c, d, e, f, g, h, i) ((VOID)0)

/* The image section check NT 3.1's own file systems use before truncating */
#define ExfCanFileBeTruncated(SectionPointers, NewSize) \
    MmFlushImageSection((SectionPointers), MmFlushForWrite)

/* Cleanup runs in the caller's process */
#define ExfRequestorProcess(Irp)    IoGetCurrentProcess()

/*
 * Thread fields. The fake kernel of the test run has these as functions;
 * on NT 3.1 they are found in the kernel at load time (see exfinit.c).
 */
#ifndef FAKE_NTIFS_H

#undef ObDereferenceObject
#undef IoCallDriver
#undef IoCompleteRequest

NTKERNELAPI VOID NTAPI ObDereferenceObject(PVOID Object);
NTKERNELAPI NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTKERNELAPI VOID NTAPI IoCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
NTKERNELAPI VOID NTAPI ExInitializeResource(PERESOURCE Resource);
NTKERNELAPI VOID NTAPI ExDeleteResource(PERESOURCE Resource);
NTKERNELAPI BOOLEAN NTAPI ExAcquireResourceExclusive(PERESOURCE Resource, BOOLEAN Wait);
NTKERNELAPI VOID NTAPI ExReleaseResourceForThread(PERESOURCE Resource, ERESOURCE_THREAD Thread);


/*
 * What the NT4 import library cannot provide: ExAcquireResourceShared is
 * not in it, and NT 3.1's FsRtlCopyRead/Write take 7 arguments. These are
 * looked up in the kernel's export table at load time.
 */
typedef BOOLEAN (NTAPI *PEXF31_ACQUIRE_SHARED)(PERESOURCE Resource, BOOLEAN Wait);

typedef struct _EXF_NT31_KERNEL {
    ULONG KernelApcDisable;     /* KTHREAD */
    ULONG TopLevelIrp;          /* ETHREAD */
    ULONG DeviceToVerify;       /* ETHREAD */
    PEXF31_ACQUIRE_SHARED AcquireResourceShared;
    PVOID FsRtlCopyRead;
    PVOID FsRtlCopyWrite;
} EXF_NT31_KERNEL;

extern EXF_NT31_KERNEL ExfNt31;

#define ExAcquireResourceSharedLite(R,W)    ExfNt31.AcquireResourceShared((PERESOURCE)(R), (W))
#define EXF31_COPY_READ                     ExfNt31.FsRtlCopyRead
#define EXF31_COPY_WRITE                    ExfNt31.FsRtlCopyWrite

#define ExfThreadField(Type, Thread, Offset) \
    (*(Type *)((PUCHAR)(Thread) + (Offset)))

#undef KeEnterCriticalRegion
#undef IoGetTopLevelIrp
#undef IoSetTopLevelIrp
#undef IoGetDeviceToVerify
#undef IoSetDeviceToVerify

#define KeEnterCriticalRegion() \
    (ExfThreadField(LONG, KeGetCurrentThread(), ExfNt31.KernelApcDisable) -= 1)
#define IoGetTopLevelIrp() \
    ExfThreadField(PIRP, KeGetCurrentThread(), ExfNt31.TopLevelIrp)
#define IoSetTopLevelIrp(Irp) \
    (ExfThreadField(PIRP, KeGetCurrentThread(), ExfNt31.TopLevelIrp) = (Irp))
#define IoGetDeviceToVerify(Thread) \
    ExfThreadField(PDEVICE_OBJECT, (Thread), ExfNt31.DeviceToVerify)
#define IoSetDeviceToVerify(Thread, Device) \
    (ExfThreadField(PDEVICE_OBJECT, (Thread), ExfNt31.DeviceToVerify) = (Device))

NTSTATUS ExfFindNt31Kernel(VOID);

#else  /* FAKE_NTIFS_H */

#define ExAcquireResourceSharedLite(R,W)    ExAcquireResourceShared((PERESOURCE)(R), (W))
#define EXF31_COPY_READ                     FsRtlCopyRead
#define EXF31_COPY_WRITE                    FsRtlCopyWrite

#endif /* !FAKE_NTIFS_H */

/* NT 3.1 fast I/O: the same routines without the DeviceObject argument */
typedef BOOLEAN (NTAPI *PEXF31_FAST_IO_CHECK_IF_POSSIBLE)(PFILE_OBJECT, PLARGE_INTEGER, ULONG,
                                                          BOOLEAN, ULONG, BOOLEAN, PIO_STATUS_BLOCK);
typedef BOOLEAN (NTAPI *PEXF31_FAST_IO_QUERY_BASIC_INFO)(PFILE_OBJECT, BOOLEAN,
                                                         PFILE_BASIC_INFORMATION, PIO_STATUS_BLOCK);
typedef BOOLEAN (NTAPI *PEXF31_FAST_IO_QUERY_STANDARD_INFO)(PFILE_OBJECT, BOOLEAN,
                                                            PFILE_STANDARD_INFORMATION, PIO_STATUS_BLOCK);

/* Major functions NT 3.1 knows: IRP_MJ_CREATE to IRP_MJ_SET_SECURITY */
#define EXF_MAX_MAJOR_FUNCTION      0x15

#endif /* _EXFNT31_H_ */
