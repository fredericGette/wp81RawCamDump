/*
 * Frame buffers mapped in the VFE IOMMU (SMMU context banks 13 and 14)
 * through the qcvcm8930.sys export driver.
 *
 * qccamavs8930.sys does exactly this, per camera session:
 *   - CreateVcm (sub_40DAA4, called by CaptureFilter::AcquireFilterResources):
 *       VcmCreate(0x4000, 0x80000000), then VcmAssoc(vcm, ctx, 4) +
 *       VcmActivate for the contexts 13 and 14 (table at .rdata:0x557B94)
 *   - released by sub_407FA8 (ReleaseFilterResources)
 *   - CameraBufferPool::AllocateSMMUMemory (sub_40D814):
 *       VcmReserve(vcm, size, 0x3E0), MmAllocatePagesForMdlEx, VcmBack,
 *       VcmGetDevAddr
 * So while the camera is not used by any application, the VFE contexts are
 * free and this driver can attach its own VCM. Attaching while AVS has its
 * own VCM attached would replace AVS's page tables: the application must make
 * sure the camera is idle.
 */
#include "RawCam.h"

/* qcvcm8930.sys exports (signatures from the analysis of qcvcm8930.sys). */
PVOID VcmCreate(ULONG Start, ULONG Size);
NTSTATUS VcmFree(PVOID Vcm);
PVOID VcmAssoc(PVOID Vcm, ULONG ContextId, ULONG Attributes);
NTSTATUS VcmDeassoc(PVOID Assoc);
NTSTATUS VcmActivate(PVOID Assoc);
NTSTATUS VcmDeactivate(PVOID Assoc);
PVOID VcmReserve(PVOID Vcm, ULONG Size, ULONG Attributes);
NTSTATUS VcmUnreserve(PVOID Reservation);
NTSTATUS VcmBack(PVOID Reservation, PMDL Physmem);
NTSTATUS VcmUnback(PVOID Reservation);
ULONG VcmGetDevAddr(PVOID Reservation);
PMDL VcmMapMdlToPhysmem(PMDL Mdl);
NTSTATUS VcmUnMapMdlFromPhysmem(PMDL Physmem);

#define RC_VCM_START            0x4000
#define RC_VCM_SIZE             0x80000000
#define RC_VCM_ASSOC_ATTRIBUTES 4
#define RC_VCM_RESERVE_ATTR     0x3E0       /* read/write user+supervisor, same as AVS */

#define MM_ALLOCATE_FULLY_REQUIRED_ 0x4

static const ULONG RcVfeContextIds[2] = { 13, 14 };

NTSTATUS
RcSmmuCreate(
    _Inout_ PRC_SESSION Session)
{
    NTSTATUS status;
    ULONG i;

    Session->Vcm = VcmCreate(RC_VCM_START, RC_VCM_SIZE);
    if (Session->Vcm == NULL) {
        RcTrace(RC_LEVEL_ERROR, L"VcmCreate failed");
        return STATUS_UNSUCCESSFUL;
    }

    for (i = 0; i < ARRAYSIZE(RcVfeContextIds); i++) {
        PVOID assoc = VcmAssoc(Session->Vcm, RcVfeContextIds[i], RC_VCM_ASSOC_ATTRIBUTES);
        if (assoc == NULL) {
            RcTrace(RC_LEVEL_ERROR, L"VcmAssoc(ctx %u) failed", RcVfeContextIds[i]);
            status = STATUS_UNSUCCESSFUL;
            goto Fail;
        }
        status = VcmActivate(assoc);
        if (!NT_SUCCESS(status)) {
            RcTrace(RC_LEVEL_ERROR, L"VcmActivate(ctx %u) failed 0x%08X", RcVfeContextIds[i], status);
            VcmDeassoc(assoc);
            goto Fail;
        }
        Session->Assoc[i] = assoc;
    }

    RcTrace(RC_LEVEL_INFORMATION, L"VCM attached to the VFE contexts");
    return STATUS_SUCCESS;

Fail:
    RcSmmuDetach(Session);
    return status;
}

/*
 * Removes every SMMU mapping and detaches the VCM from the VFE contexts, but
 * keeps the frame pages and their CPU mapping, so READ_FRAME still works.
 * Touches the IOMMU registers (TLB invalidation, context banks): must run
 * while the VFE is powered (see RcFrameAllocate).
 */
VOID
RcSmmuDetach(
    _Inout_ PRC_SESSION Session)
{
    ULONG i;

    for (i = 0; i < RAWCAM_MAX_FRAMES; i++) {
        PRC_FRAME frame = &Session->Frames[i];
        if (frame->Reservation != NULL) {
            VcmUnback(frame->Reservation);
            VcmUnMapMdlFromPhysmem(frame->Mdl);
            VcmUnreserve(frame->Reservation);
            frame->Reservation = NULL;
        }
    }

    for (i = ARRAYSIZE(Session->Assoc); i-- != 0; ) {
        if (Session->Assoc[i] != NULL) {
            VcmDeactivate(Session->Assoc[i]);
            VcmDeassoc(Session->Assoc[i]);
            Session->Assoc[i] = NULL;
        }
    }

    if (Session->Vcm != NULL) {
        NTSTATUS status = VcmFree(Session->Vcm);
        if (!NT_SUCCESS(status)) {
            RcTrace(RC_LEVEL_WARNING, L"VcmFree 0x%08X", status);
        }
        Session->Vcm = NULL;
    }
}

/*
 * Frees everything. The SMMU part is only done if the VFE is still powered;
 * the teardown calls RcSmmuDetach before VFE POWER_OFF, so normally nothing is
 * left to detach here.
 */
VOID
RcSmmuDestroy(
    _Inout_ PRC_SESSION Session)
{
    ULONG i;

    if (Session->Vcm != NULL) {
        if (Session->VfePowered) {
            RcSmmuDetach(Session);
        } else {
            /* Touching the IOMMU now could hang the bus: leak the VCM objects instead. */
            RcTrace(RC_LEVEL_ERROR, L"VCM still attached with the VFE off: SMMU objects leaked");
            for (i = 0; i < RAWCAM_MAX_FRAMES; i++) {
                Session->Frames[i].Reservation = NULL;
            }
            Session->Vcm = NULL;
            Session->Assoc[0] = Session->Assoc[1] = NULL;
        }
    }

    for (i = 0; i < RAWCAM_MAX_FRAMES; i++) {
        RcFrameFree(Session, i);
    }
}

NTSTATUS
RcFrameAllocate(
    _Inout_ PRC_SESSION Session,
    _In_ const RAWCAM_ALLOC_INPUT *Input,
    _Out_ RAWCAM_ALLOC_OUTPUT *Output)
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS skip;
    PRC_FRAME frame = NULL;
    ULONG size;
    ULONG index;
    NTSTATUS status;

    if (!(Session->OpenedFlags & RAWCAM_OPEN_SMMU)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    /*
     * The VFE IOMMU registers are only clocked while the VFE is powered:
     * attaching before VFE POWER_ON hung the phone. So the VCM is created at
     * the first allocation, after POWER_ON, and detached before POWER_OFF.
     */
    if (!Session->VfePowered) {
        RcTrace(RC_LEVEL_ERROR, L"frame allocation before VFE POWER_ON");
        return STATUS_DEVICE_NOT_READY;
    }
    if (Session->Vcm == NULL) {
        status = RcSmmuCreate(Session);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    if (Input->Size == 0 || Input->Size > RAWCAM_MAX_FRAME_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    size = (Input->Size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (index = 0; index < RAWCAM_MAX_FRAMES; index++) {
        if (Session->Frames[index].Mdl == NULL) {
            frame = &Session->Frames[index];
            break;
        }
    }
    if (frame == NULL) {
        return STATUS_TOO_MANY_OPENED_FILES;
    }

    low.QuadPart = 0;
    high.QuadPart = -1;
    skip.QuadPart = 0;
    frame->Mdl = MmAllocatePagesForMdlEx(low, high, skip, size, MmNonCached, MM_ALLOCATE_FULLY_REQUIRED_);
    if (frame->Mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    frame->SystemAddress = MmMapLockedPagesSpecifyCache(frame->Mdl, KernelMode, MmNonCached, NULL, FALSE,
                                                        NormalPagePriority | MdlMappingNoExecute);
    if (frame->SystemAddress == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }
    RtlFillMemory(frame->SystemAddress, size, (UCHAR)Input->Fill);
    RC_MEMORY_BARRIER();

    frame->Reservation = VcmReserve(Session->Vcm, size, RC_VCM_RESERVE_ATTR);
    if (frame->Reservation == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    status = VcmBack(frame->Reservation, VcmMapMdlToPhysmem(frame->Mdl));
    if (!NT_SUCCESS(status)) {
        VcmUnreserve(frame->Reservation);
        frame->Reservation = NULL;
        goto Fail;
    }

    frame->DeviceAddress = VcmGetDevAddr(frame->Reservation);
    frame->Size = size;

    Output->Index = index;
    Output->DeviceAddress = frame->DeviceAddress;
    Output->Size = size;

    RcTrace(RC_LEVEL_INFORMATION, L"frame %u: %u bytes at device address 0x%08X", index, size, frame->DeviceAddress);
    return STATUS_SUCCESS;

Fail:
    RcTrace(RC_LEVEL_ERROR, L"frame allocation of %u bytes failed 0x%08X", size, status);
    if (frame->SystemAddress != NULL) {
        MmUnmapLockedPages(frame->SystemAddress, frame->Mdl);
        frame->SystemAddress = NULL;
    }
    MmFreePagesFromMdl(frame->Mdl);
    ExFreePool(frame->Mdl);
    frame->Mdl = NULL;
    return status;
}

NTSTATUS
RcFrameFree(
    _Inout_ PRC_SESSION Session,
    _In_ ULONG Index)
{
    PRC_FRAME frame;

    if (Index >= RAWCAM_MAX_FRAMES) {
        return STATUS_INVALID_PARAMETER;
    }
    frame = &Session->Frames[Index];
    if (frame->Mdl == NULL) {
        return STATUS_SUCCESS;
    }

    if (frame->Reservation != NULL) {
        if (!Session->VfePowered) {
            /* The pages stay mapped in the IOMMU page tables: keep them (leak) rather than hang. */
            RcTrace(RC_LEVEL_ERROR, L"frame %u still mapped with the VFE off: leaked", Index);
            frame->Reservation = NULL;
            frame->Mdl = NULL;
            frame->SystemAddress = NULL;
            return STATUS_DEVICE_BUSY;
        }
        VcmUnback(frame->Reservation);
        VcmUnMapMdlFromPhysmem(frame->Mdl);
        VcmUnreserve(frame->Reservation);
        frame->Reservation = NULL;
    }
    if (frame->SystemAddress != NULL) {
        MmUnmapLockedPages(frame->SystemAddress, frame->Mdl);
        frame->SystemAddress = NULL;
    }
    MmFreePagesFromMdl(frame->Mdl);
    ExFreePool(frame->Mdl);
    RtlZeroMemory(frame, sizeof(*frame));
    return STATUS_SUCCESS;
}
