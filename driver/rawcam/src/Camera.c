/*
 * Access to the camera hardware drivers through their device interfaces.
 */
#include "RawCam.h"

/*
 * Opens the first instance of a device interface, as qccamavs8930.sys does
 * (CameraDeviceManager, sub_419F80).
 */
NTSTATUS
RcOpenTarget(
    _In_ const GUID *InterfaceGuid,
    _Out_ PRC_TARGET Target)
{
    PWSTR symbolicLinks = NULL;
    UNICODE_STRING name;
    NTSTATUS status;

    Target->DeviceObject = NULL;
    Target->FileObject = NULL;

    status = IoGetDeviceInterfaces(InterfaceGuid, NULL, 0, &symbolicLinks);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (symbolicLinks == NULL || symbolicLinks[0] == L'\0') {
        if (symbolicLinks != NULL) {
            ExFreePool(symbolicLinks);
        }
        return STATUS_NO_SUCH_DEVICE;
    }

    RtlInitUnicodeString(&name, symbolicLinks);
    status = IoGetDeviceObjectPointer(&name, FILE_ALL_ACCESS, &Target->FileObject, &Target->DeviceObject);
    RcTrace(RC_LEVEL_INFORMATION, L"open %wZ -> 0x%08X", &name, status);
    ExFreePool(symbolicLinks);
    return status;
}

VOID
RcCloseTarget(
    _Inout_ PRC_TARGET Target)
{
    if (Target->FileObject != NULL) {
        ObDereferenceObject(Target->FileObject);
    }
    Target->FileObject = NULL;
    Target->DeviceObject = NULL;
}

/*
 * Synchronous internal IOCTL. With METHOD_NEITHER, IoBuildDeviceIoControlRequest
 * passes InputBuffer as Type3InputBuffer and OutputBuffer as Irp->UserBuffer,
 * which is what the camera drivers expect from qccamavs8930.sys.
 */
NTSTATUS
RcSendInternalIoctl(
    _In_ PRC_TARGET Target,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputLength,
    _In_opt_ PVOID OutputBuffer,
    _In_ ULONG OutputLength)
{
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;

    if (Target->DeviceObject == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IoControlCode, Target->DeviceObject, InputBuffer, InputLength,
                                        OutputBuffer, OutputLength, TRUE, &event, &ioStatus);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoGetNextIrpStackLocation(irp)->FileObject = Target->FileObject;

    status = IoCallDriver(Target->DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }
    return status;
}

/*
 * Sensor / VFE command n with the 16-byte response descriptor:
 *   +0x00 response buffer, +0x04 response size, +0x08 PULONG length, +0x0C result
 * The result is preset to 1 (failure), as qccamavs8930.sys does.
 */
NTSTATUS
RcCameraCommand(
    _In_ PRC_TARGET Target,
    _In_ ULONG Command,
    _In_reads_bytes_opt_(InputLength) const VOID *Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(ResponseSize) PVOID Response,
    _In_ ULONG ResponseSize,
    _Out_ PULONG ResponseLength,
    _Out_ PULONG CameraResult)
{
    struct {
        PVOID Response;
        ULONG ResponseSize;
        PULONG ResponseLength;
        ULONG Result;
        ULONG Length;               /* target of ResponseLength */
    } *descriptor;
    PUCHAR input = NULL;
    PUCHAR response = NULL;
    NTSTATUS status;

    *ResponseLength = 0;
    *CameraResult = 1;

    descriptor = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*descriptor), RC_POOL_TAG);
    if (descriptor == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(descriptor, sizeof(*descriptor));

    if (InputLength != 0) {
        input = ExAllocatePoolWithTag(NonPagedPoolNx, InputLength, RC_POOL_TAG);
        if (input == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        RtlCopyMemory(input, Input, InputLength);
    }
    if (ResponseSize != 0) {
        response = ExAllocatePoolWithTag(NonPagedPoolNx, ResponseSize, RC_POOL_TAG);
        if (response == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        RtlZeroMemory(response, ResponseSize);
        descriptor->Response = response;
        descriptor->ResponseSize = ResponseSize;
        descriptor->ResponseLength = &descriptor->Length;
    }
    descriptor->Result = 1;

    status = RcSendInternalIoctl(Target, RC_CAMERA_IOCTL(Command), input, InputLength, descriptor, 16);

    *CameraResult = descriptor->Result;
    if (response != NULL) {
        /* Some commands don't report a length: then the whole buffer is returned. */
        ULONG length = (descriptor->Length != 0 && descriptor->Length <= ResponseSize) ? descriptor->Length : ResponseSize;
        RtlCopyMemory(Response, response, length);
        *ResponseLength = length;
    }

    RcTrace(RC_LEVEL_VERBOSE, L"cmd n=%u in=%u resp=%u/%u status=0x%08X result=%u",
            Command, InputLength, descriptor->Length, ResponseSize, status, descriptor->Result);

Exit:
    if (response != NULL) {
        ExFreePoolWithTag(response, RC_POOL_TAG);
    }
    if (input != NULL) {
        ExFreePoolWithTag(input, RC_POOL_TAG);
    }
    ExFreePoolWithTag(descriptor, RC_POOL_TAG);
    return status;
}

/*
 * I2C read or write of 1..32 bytes through the camera platform driver.
 * Transfer structure (36 bytes): +0x04 slave (8-bit), +0x08 flags,
 * +0x10 data pointer, +0x14 register, +0x16 byte count.
 * The read uses the output buffer, the write the input buffer.
 */
NTSTATUS
RcI2cTransfer(
    _In_ PRC_TARGET Platform,
    _In_ BOOLEAN Write,
    _Inout_ RAWCAM_I2C *Request)
{
    struct {
        UCHAR Transfer[36];
        UCHAR Data[32];
    } *block;
    NTSTATUS status;
    ULONG flags = (Request->Flags != 0) ? Request->Flags : 2;
    UCHAR slave = (Request->Slave != 0) ? Request->Slave : 0x20;

    if (Request->Length == 0 || Request->Length > sizeof(Request->Data)) {
        return STATUS_INVALID_PARAMETER;
    }

    block = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*block), RC_POOL_TAG);
    if (block == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(block, sizeof(*block));

    block->Transfer[0x04] = slave;
    *(ULONG UNALIGNED *)&block->Transfer[0x08] = flags;
    *(PVOID UNALIGNED *)&block->Transfer[0x10] = block->Data;
    *(USHORT UNALIGNED *)&block->Transfer[0x14] = Request->Register;
    *(USHORT UNALIGNED *)&block->Transfer[0x16] = (USHORT)Request->Length;
    *(USHORT UNALIGNED *)&block->Transfer[0x18] = 1;

    if (Write) {
        RtlCopyMemory(block->Data, Request->Data, Request->Length);
        status = RcSendInternalIoctl(Platform, RC_PLATFORM_I2C_WRITE, block->Transfer, 36, NULL, 0);
    } else {
        status = RcSendInternalIoctl(Platform, RC_PLATFORM_I2C_READ, NULL, 0, block->Transfer, 36);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(Request->Data, block->Data, Request->Length);
        }
    }

    RcTrace(RC_LEVEL_VERBOSE, L"i2c %s slave=0x%02X reg=0x%04X len=%u status=0x%08X",
            Write ? L"write" : L"read", slave, Request->Register, Request->Length, status);

    Request->Status = status;
    ExFreePoolWithTag(block, RC_POOL_TAG);
    return status;
}
