/*
 * Control device, IOCTL dispatch and session lifetime.
 *
 * One session at a time. It belongs to the file object that sent
 * IOCTL_RAWCAM_OPEN and is torn down by IOCTL_RAWCAM_CLOSE, by the cleanup
 * of that handle (application exit or crash), or by the driver unload.
 * The teardown undoes what the session started, in this order: VFE stop and
 * power off, sensor stop and power off, VFE message queue flush, SMMU
 * mappings, device references.
 */
#include "RawCam.h"

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD RcUnload;
static DRIVER_DISPATCH RcDispatchCreateClose;
static DRIVER_DISPATCH RcDispatchCleanup;
static DRIVER_DISPATCH RcDispatchDeviceControl;

/* {5e34e1c5-c5bc-4c7f-b6fc-e2443e45be67} back sensor, qccamsensor8930.sys */
static const GUID RcSensorInterface =
    { 0x5e34e1c5, 0xc5bc, 0x4c7f, { 0xb6, 0xfc, 0xe2, 0x44, 0x3e, 0x45, 0xbe, 0x67 } };
/* {3e9c0fdb-cef9-4c4a-8e85-36e4a82eb80f} VFE, qccamvfe8930.sys */
static const GUID RcVfeInterface =
    { 0x3e9c0fdb, 0xcef9, 0x4c4a, { 0x8e, 0x85, 0x36, 0xe4, 0xa8, 0x2e, 0xb8, 0x0f } };
/* {8d73ce35-93cf-4795-8db9-40bbe130d859} camera platform, qccamplatform8930.sys */
static const GUID RcPlatformInterface =
    { 0x8d73ce35, 0x93cf, 0x4795, { 0x8d, 0xb9, 0x40, 0xbb, 0xe1, 0x30, 0xd8, 0x59 } };

/* Sensor commands */
#define RC_SENSOR_POWER_ON      0
#define RC_SENSOR_POWER_OFF     1
#define RC_SENSOR_START_STREAM  2
#define RC_SENSOR_STOP_STREAM   3
/* VFE commands */
#define RC_VFE_POWER_ON         1
#define RC_VFE_POWER_OFF        2
#define RC_VFE_START            4
#define RC_VFE_CAPTURE          102
#define RC_VFE_STOP             109
#define RC_VFE_FIRST_FLUSH      130
#define RC_VFE_LAST_FLUSH       138

static KMUTEX RcLock;
static RC_SESSION RcSession;
static BOOLEAN RcSessionOpen;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING deviceName;
    UNICODE_STRING linkName;
    PDEVICE_OBJECT deviceObject;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&deviceName, RAWCAM_DEVICE_NAME);
    RtlInitUnicodeString(&linkName, RAWCAM_SYMLINK_NAME);

    RcTraceRegister();
    KeInitializeMutex(&RcLock, 0);

    status = IoCreateDevice(DriverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                            FALSE, &deviceObject);
    if (!NT_SUCCESS(status)) {
        RcTraceUnregister();
        return status;
    }

    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        RcTraceUnregister();
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = RcDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = RcDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = RcDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RcDispatchDeviceControl;
    DriverObject->DriverUnload = RcUnload;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    RcTrace(RC_LEVEL_INFORMATION, L"wp81rawcam loaded");
    return STATUS_SUCCESS;
}

static VOID
RcLockAcquire(VOID)
{
    KeWaitForSingleObject(&RcLock, Executive, KernelMode, FALSE, NULL);
}

static VOID
RcLockRelease(VOID)
{
    KeReleaseMutex(&RcLock, FALSE);
}

static VOID
RcVfeCommandNoData(
    _In_ PRC_SESSION Session,
    _In_ ULONG Command)
{
    ULONG length;
    ULONG result;
    NTSTATUS status = RcCameraCommand(&Session->Vfe, Command, NULL, 0, NULL, 0, &length, &result);

    RcTrace(RC_LEVEL_INFORMATION, L"teardown VFE n=%u status=0x%08X result=%u", Command, status, result);
}

static VOID
RcSensorCommandNoData(
    _In_ PRC_SESSION Session,
    _In_ ULONG Command)
{
    ULONG length;
    ULONG result;
    NTSTATUS status = RcCameraCommand(&Session->Sensor, Command, NULL, 0, NULL, 0, &length, &result);

    RcTrace(RC_LEVEL_INFORMATION, L"teardown sensor n=%u status=0x%08X result=%u", Command, status, result);
}

/* Called with RcLock held. */
static VOID
RcSessionClose(VOID)
{
    PRC_SESSION session = &RcSession;
    ULONG command;

    if (!RcSessionOpen) {
        return;
    }

    if (session->VfeStarted) {
        RcVfeCommandNoData(session, RC_VFE_STOP);
    }
    if (session->VfePowered) {
        for (command = RC_VFE_FIRST_FLUSH; command <= RC_VFE_LAST_FLUSH; command++) {
            RcVfeCommandNoData(session, command);
        }
        /* The IOMMU must be released while the VFE clocks still run. */
        RcSmmuDetach(session);
        RcVfeCommandNoData(session, RC_VFE_POWER_OFF);
    }
    if (session->SensorStreaming) {
        RcSensorCommandNoData(session, RC_SENSOR_STOP_STREAM);
    }
    if (session->SensorPowered) {
        RcSensorCommandNoData(session, RC_SENSOR_POWER_OFF);
    }

    RcVfeQueueUnregister(session);

    /* Only once the VFE is stopped: it must not write to unmapped addresses. */
    RcSmmuDestroy(session);

    RcCloseTarget(&session->Platform);
    RcCloseTarget(&session->Vfe);
    RcCloseTarget(&session->Sensor);

    RtlZeroMemory(session, sizeof(*session));
    RcSessionOpen = FALSE;
    RcTrace(RC_LEVEL_INFORMATION, L"session closed");
}

/* Called with RcLock held. */
static NTSTATUS
RcSessionOpenFor(
    _In_ PFILE_OBJECT Owner,
    _In_ ULONG Flags,
    _Out_ RAWCAM_OPEN_OUTPUT *Output)
{
    PRC_SESSION session = &RcSession;
    NTSTATUS status = STATUS_SUCCESS;

    if (RcSessionOpen) {
        return STATUS_DEVICE_BUSY;
    }

    RtlZeroMemory(session, sizeof(*session));
    session->Owner = Owner;
    RcSessionOpen = TRUE;

    if (Flags & RAWCAM_OPEN_SENSOR) {
        status = RcOpenTarget(&RcSensorInterface, &session->Sensor);
        if (!NT_SUCCESS(status)) goto Fail;
        session->OpenedFlags |= RAWCAM_OPEN_SENSOR;
    }
    if (Flags & RAWCAM_OPEN_PLATFORM) {
        status = RcOpenTarget(&RcPlatformInterface, &session->Platform);
        if (!NT_SUCCESS(status)) goto Fail;
        session->OpenedFlags |= RAWCAM_OPEN_PLATFORM;
    }
    if (Flags & RAWCAM_OPEN_VFE) {
        status = RcOpenTarget(&RcVfeInterface, &session->Vfe);
        if (!NT_SUCCESS(status)) goto Fail;
        status = RcVfeQueueRegister(session);
        if (!NT_SUCCESS(status)) goto Fail;
        session->OpenedFlags |= RAWCAM_OPEN_VFE;
    }
    if (Flags & RAWCAM_OPEN_SMMU) {
        /* The VCM is attached at the first frame allocation, once the VFE is powered (Smmu.c). */
        session->OpenedFlags |= RAWCAM_OPEN_SMMU;
    }

    Output->OpenedFlags = session->OpenedFlags;
    Output->VfeQueueAddress = (ULONG)(ULONG_PTR)session->VfeQueue;
    RcTrace(RC_LEVEL_INFORMATION, L"session opened, flags 0x%X", session->OpenedFlags);
    return STATUS_SUCCESS;

Fail:
    RcTrace(RC_LEVEL_ERROR, L"session open failed 0x%08X (opened 0x%X)", status, session->OpenedFlags);
    RcSessionClose();
    return status;
}

/* Remembers what was started, for the teardown. */
static VOID
RcTrackCommand(
    _In_ PRC_SESSION Session,
    _In_ BOOLEAN Sensor,
    _In_ ULONG Command)
{
    if (Sensor) {
        switch (Command) {
        case RC_SENSOR_POWER_ON:     Session->SensorPowered = TRUE; break;
        case RC_SENSOR_POWER_OFF:    Session->SensorPowered = FALSE; Session->SensorStreaming = FALSE; break;
        case RC_SENSOR_START_STREAM: Session->SensorStreaming = TRUE; break;
        case RC_SENSOR_STOP_STREAM:  Session->SensorStreaming = FALSE; break;
        default: break;
        }
    } else {
        switch (Command) {
        case RC_VFE_POWER_ON:  Session->VfePowered = TRUE; break;
        case RC_VFE_POWER_OFF: Session->VfePowered = FALSE; Session->VfeStarted = FALSE; break;
        case RC_VFE_START:
        case RC_VFE_CAPTURE:   Session->VfeStarted = TRUE; break;
        case RC_VFE_STOP:      Session->VfeStarted = FALSE; break;
        default: break;
        }
    }
}

static NTSTATUS
RcCommandIoctl(
    _In_ PRC_SESSION Session,
    _In_ BOOLEAN Sensor,
    _In_ PVOID SystemBuffer,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    const RAWCAM_CMD_INPUT *input = (const RAWCAM_CMD_INPUT *)SystemBuffer;
    RAWCAM_CMD_INPUT header;
    RAWCAM_CMD_OUTPUT *output = (RAWCAM_CMD_OUTPUT *)SystemBuffer;
    PUCHAR inputCopy = NULL;
    PUCHAR response = NULL;
    ULONG responseLength = 0;
    ULONG cameraResult = 1;
    NTSTATUS status;
    PRC_TARGET target = Sensor ? &Session->Sensor : &Session->Vfe;

    if (InputLength < RAWCAM_CMD_INPUT_HEADER) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(&header, input, RAWCAM_CMD_INPUT_HEADER);
    if (header.InputLength > RAWCAM_MAX_CMD_INPUT ||
        header.InputLength > InputLength - RAWCAM_CMD_INPUT_HEADER ||
        header.ResponseLength > RAWCAM_MAX_CMD_RESPONSE ||
        OutputLength < RAWCAM_CMD_OUTPUT_HEADER ||
        header.ResponseLength > OutputLength - RAWCAM_CMD_OUTPUT_HEADER) {
        return STATUS_INVALID_PARAMETER;
    }
    if (target->DeviceObject == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* Input and output share the system buffer: copy the input first. */
    if (header.InputLength != 0) {
        inputCopy = ExAllocatePoolWithTag(NonPagedPoolNx, header.InputLength, RC_POOL_TAG);
        if (inputCopy == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(inputCopy, input->Input, header.InputLength);
    }
    if (header.ResponseLength != 0) {
        response = ExAllocatePoolWithTag(NonPagedPoolNx, header.ResponseLength, RC_POOL_TAG);
        if (response == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
    }

    if (!Sensor && header.Command == RC_VFE_POWER_OFF && Session->VfePowered) {
        /* Release the IOMMU while the VFE clocks still run (the frames stay readable). */
        if (Session->VfeStarted) {
            status = STATUS_DEVICE_BUSY;
            goto Exit;
        }
        RcSmmuDetach(Session);
    }

    status = RcCameraCommand(target, header.Command, inputCopy, header.InputLength,
                             response, header.ResponseLength, &responseLength, &cameraResult);

    /*
     * POWER_OFF switches the clocks off even when it reports a failure (the VFE
     * handler idles its power component after a failed ISPIF reset): any more
     * register access would then hang the bus. So a power off that reached the
     * driver always counts as done.
     */
    if (NT_SUCCESS(status) &&
        (cameraResult == 0 ||
         (!Sensor && header.Command == RC_VFE_POWER_OFF) ||
         (Sensor && header.Command == RC_SENSOR_POWER_OFF))) {
        RcTrackCommand(Session, Sensor, header.Command);
    }

    /* The IOCTL itself succeeds: the command status and result are in the output. */
    output->Status = status;
    output->CameraResult = cameraResult;
    output->ResponseLength = responseLength;
    if (responseLength != 0) {
        RtlCopyMemory(output->Response, response, responseLength);
    }
    *Information = RAWCAM_CMD_OUTPUT_HEADER + responseLength;
    status = STATUS_SUCCESS;

Exit:
    if (response != NULL) {
        ExFreePoolWithTag(response, RC_POOL_TAG);
    }
    if (inputCopy != NULL) {
        ExFreePoolWithTag(inputCopy, RC_POOL_TAG);
    }
    return status;
}

static NTSTATUS
RcPlatformIoctl(
    _In_ PRC_SESSION Session,
    _In_ const RAWCAM_PLATFORM_INPUT *Input)
{
    PULONG value;
    NTSTATUS status;
    ULONG code = Input->IoControlCode;

    if (Session->Platform.DeviceObject == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    /* Buffer-less codes and the two small-value ones; the I2C codes have their own IOCTLs. */
    if (code != 0x232003 && code != 0x232007 && code != 0x232013 && code != 0x232017 &&
        code != 0x23201B && code != 0x23201F) {
        return STATUS_INVALID_PARAMETER;
    }

    value = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(ULONG) * 2, RC_POOL_TAG);
    if (value == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    value[0] = Input->Value[0];
    value[1] = Input->Value[1];

    if (code == 0x232017) {
        /* GPIO: ULONG id, then the value byte. */
        status = RcSendInternalIoctl(&Session->Platform, code, value, 8, NULL, 0);
    } else if (code == 0x232013) {
        status = RcSendInternalIoctl(&Session->Platform, code, value, 4, NULL, 0);
    } else {
        status = RcSendInternalIoctl(&Session->Platform, code, NULL, 0, NULL, 0);
    }

    ExFreePoolWithTag(value, RC_POOL_TAG);
    return status;
}

static NTSTATUS
RcReadFrame(
    _In_ PRC_SESSION Session,
    _In_ PIRP Irp,
    _In_ const RAWCAM_READ_INPUT *Input,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    PRC_FRAME frame;
    PVOID destination;

    if (Input->Index >= RAWCAM_MAX_FRAMES) {
        return STATUS_INVALID_PARAMETER;
    }
    frame = &Session->Frames[Input->Index];
    if (frame->SystemAddress == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Input->Offset > frame->Size || OutputLength > frame->Size - Input->Offset) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputLength == 0) {
        *Information = 0;
        return STATUS_SUCCESS;
    }
    if (Irp->MdlAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    destination = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
    if (destination == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RC_MEMORY_BARRIER();
    RtlCopyMemory(destination, frame->SystemAddress + Input->Offset, OutputLength);
    *Information = OutputLength;
    return STATUS_SUCCESS;
}

static NTSTATUS
RcDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    PRC_SESSION session = &RcSession;
    ULONG_PTR information = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);

    RcLockAcquire();

    if (code != (ULONG)IOCTL_RAWCAM_OPEN && (!RcSessionOpen || session->Owner != stack->FileObject)) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Exit;
    }

    switch (code) {
    case IOCTL_RAWCAM_OPEN:
        if (inputLength < sizeof(RAWCAM_OPEN_INPUT) || outputLength < sizeof(RAWCAM_OPEN_OUTPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            RAWCAM_OPEN_OUTPUT output = { 0 };
            ULONG flags = ((RAWCAM_OPEN_INPUT *)buffer)->Flags;
            status = RcSessionOpenFor(stack->FileObject, flags, &output);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(buffer, &output, sizeof(output));
                information = sizeof(output);
            }
        }
        break;

    case IOCTL_RAWCAM_CLOSE:
        RcSessionClose();
        status = STATUS_SUCCESS;
        break;

    case IOCTL_RAWCAM_SENSOR_CMD:
    case IOCTL_RAWCAM_VFE_CMD:
        status = RcCommandIoctl(session, code == (ULONG)IOCTL_RAWCAM_SENSOR_CMD, buffer, inputLength, outputLength,
                                &information);
        break;

    case IOCTL_RAWCAM_I2C_READ:
    case IOCTL_RAWCAM_I2C_WRITE:
        if (inputLength < sizeof(RAWCAM_I2C) || outputLength < sizeof(RAWCAM_I2C)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (session->Platform.DeviceObject == NULL) {
            status = STATUS_DEVICE_NOT_CONNECTED;
            break;
        }
        {
            RAWCAM_I2C request;
            RtlCopyMemory(&request, buffer, sizeof(request));
            RcI2cTransfer(&session->Platform, code == (ULONG)IOCTL_RAWCAM_I2C_WRITE, &request);
            RtlCopyMemory(buffer, &request, sizeof(request));
            information = sizeof(request);
            status = STATUS_SUCCESS;        /* transfer status in request.Status */
        }
        break;

    case IOCTL_RAWCAM_PLATFORM_CMD:
        if (inputLength < sizeof(RAWCAM_PLATFORM_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            RAWCAM_PLATFORM_INPUT input;
            RtlCopyMemory(&input, buffer, sizeof(input));
            status = RcPlatformIoctl(session, &input);
        }
        break;

    case IOCTL_RAWCAM_ALLOC_FRAME:
        if (inputLength < sizeof(RAWCAM_ALLOC_INPUT) || outputLength < sizeof(RAWCAM_ALLOC_OUTPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            RAWCAM_ALLOC_INPUT input;
            RAWCAM_ALLOC_OUTPUT output = { 0 };
            RtlCopyMemory(&input, buffer, sizeof(input));
            status = RcFrameAllocate(session, &input, &output);
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(buffer, &output, sizeof(output));
                information = sizeof(output);
            }
        }
        break;

    case IOCTL_RAWCAM_FREE_FRAME:
        if (inputLength < sizeof(RAWCAM_FREE_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (session->VfeStarted) {
            /* The VFE may still write into it. */
            status = STATUS_DEVICE_BUSY;
            break;
        }
        status = RcFrameFree(session, ((RAWCAM_FREE_INPUT *)buffer)->Index);
        break;

    case IOCTL_RAWCAM_WAIT_VFE_MSG:
        if (inputLength < sizeof(RAWCAM_WAIT_INPUT) || outputLength < sizeof(RAWCAM_VFE_MESSAGE)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            RAWCAM_VFE_MESSAGE message;
            ULONG timeout = ((RAWCAM_WAIT_INPUT *)buffer)->TimeoutMs;
            RtlZeroMemory(&message, sizeof(message));
            status = RcVfeQueueWait(session, (timeout > 60000) ? 60000 : timeout, &message);
            if (status == STATUS_SUCCESS) {
                RtlCopyMemory(buffer, &message, sizeof(message));
                information = sizeof(message);
            }
        }
        break;

    case IOCTL_RAWCAM_READ_FRAME:
        if (inputLength < sizeof(RAWCAM_READ_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        {
            RAWCAM_READ_INPUT input;
            RtlCopyMemory(&input, buffer, sizeof(input));
            status = RcReadFrame(session, Irp, &input, outputLength, &information);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

Exit:
    RcLockRelease();

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS
RcDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
RcDispatchCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);

    RcLockAcquire();
    if (RcSessionOpen && RcSession.Owner == stack->FileObject) {
        RcTrace(RC_LEVEL_WARNING, L"handle closed with an open session: teardown");
        RcSessionClose();
    }
    RcLockRelease();

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID
RcUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING linkName;

    RtlInitUnicodeString(&linkName, RAWCAM_SYMLINK_NAME);

    RcLockAcquire();
    RcSessionClose();
    RcLockRelease();

    IoDeleteSymbolicLink(&linkName);
    if (DriverObject->DeviceObject != NULL) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }

    RcTrace(RC_LEVEL_INFORMATION, L"wp81rawcam unloaded");
    RcTraceUnregister();
}
