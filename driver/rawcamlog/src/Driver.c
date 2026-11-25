/*
 * Driver entry, filter device creation and request forwarding.
 */
#include "RawCamLog.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD RclEvtDeviceAdd;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP RclEvtDriverCleanup;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL RclEvtIoDeviceControl;
static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL RclEvtIoInternalDeviceControl;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE RclEvtRequestCompletion;

static volatile LONG RclNextRequestId;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    RclTraceRegister();

    WDF_DRIVER_CONFIG_INIT(&config, RclEvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = RclEvtDriverCleanup;

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        RclTraceMessage(RCL_LEVEL_ERROR, "WdfDriverCreate failed", NULL, status);
        RclTraceUnregister();
        return status;
    }

    RclTraceMessage(RCL_LEVEL_INFORMATION, "Driver loaded", NULL, status);
    return status;
}

static VOID
RclEvtDriverCleanup(
    _In_ WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    RclTraceMessage(RCL_LEVEL_INFORMATION, "Driver unloading", NULL, STATUS_SUCCESS);
    RclTraceUnregister();
}

/*
 * Finds which camera device this filter instance sits on from the hardware
 * IDs of the PDO (REG_MULTI_SZ, e.g. "ACPI\QCOM0710").
 */
static VOID
RclIdentifyDevice(
    _In_ WDFDEVICE Device,
    _Out_ PRCL_DEVICE_CONTEXT Context)
{
    static const struct {
        PCWSTR Id;
        RCL_DEVICE_KIND Kind;
        PCSTR Name;
    } ids[] = {
        { L"QCOM0710", RclDeviceSensor,   "SENSOR"   },
        { L"QCOM0700", RclDeviceVfe,      "VFE"      },
        { L"QCOM0775", RclDevicePlatform, "PLATFORM" },
    };
    WDFMEMORY memory = NULL;
    PWCHAR hardwareIds;
    size_t size;
    size_t i;
    ULONG j;

    Context->Kind = RclDeviceUnknown;
    Context->Name = "?";

    if (!NT_SUCCESS(WdfDeviceAllocAndQueryProperty(Device, DevicePropertyHardwareID, NonPagedPoolNx,
                                                   WDF_NO_OBJECT_ATTRIBUTES, &memory))) {
        return;
    }

    hardwareIds = (PWCHAR)WdfMemoryGetBuffer(memory, &size);
    size /= sizeof(WCHAR);

    /* Search each id of the hardware id list for one of the ACPI _HIDs. */
    for (i = 0; i + 8 <= size && Context->Kind == RclDeviceUnknown; i++) {
        for (j = 0; j < ARRAYSIZE(ids); j++) {
            ULONG k;
            for (k = 0; k < 8; k++) {
                if (hardwareIds[i + k] != ids[j].Id[k]) {
                    break;
                }
            }
            if (k == 8) {
                Context->Kind = ids[j].Kind;
                Context->Name = ids[j].Name;
                break;
            }
        }
    }

    WdfObjectDelete(memory);
}

static NTSTATUS
RclEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PRCL_DEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, RCL_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &requestAttributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, RCL_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        RclTraceMessage(RCL_LEVEL_ERROR, "WdfDeviceCreate failed", NULL, status);
        return status;
    }

    deviceContext = RclGetDeviceContext(device);
    RclIdentifyDevice(device, deviceContext);

    /*
     * Parallel and not power managed: the filter adds no serialization and
     * never holds requests; the lower drivers' sequential queues still order
     * them.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = RclEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = RclEvtIoInternalDeviceControl;
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        RclTraceMessage(RCL_LEVEL_ERROR, "WdfIoQueueCreate failed", deviceContext->Name, status);
        return status;
    }

    RclTraceMessage(RCL_LEVEL_INFORMATION, "Filter attached", deviceContext->Name, status);
    return status;
}

static VOID
RclForward(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode,
    _In_ BOOLEAN Internal)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PRCL_REQUEST_CONTEXT context = RclGetRequestContext(Request);
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;

    context->RequestId = (ULONG)InterlockedIncrement(&RclNextRequestId);
    context->IoControlCode = IoControlCode;
    context->Internal = Internal;
    context->KernelCaller = (irp->RequestorMode == KernelMode);
    context->InputLength = (InputBufferLength > MAXULONG) ? MAXULONG : (ULONG)InputBufferLength;
    context->OutputLength = (OutputBufferLength > MAXULONG) ? MAXULONG : (ULONG)OutputBufferLength;
    context->Device = RclGetDeviceContext(device);
    context->StartTime = KeQueryPerformanceCounter(NULL);

    /*
     * The raw METHOD_NEITHER pointers are only kept for kernel callers: they
     * are kernel addresses then, valid in any context. User callers (the
     * sensor driver also accepts IRP_MJ_DEVICE_CONTROL) are only logged.
     */
    if (context->KernelCaller && (IoControlCode & 3) == METHOD_NEITHER) {
        context->InputBuffer = stack->Parameters.DeviceIoControl.Type3InputBuffer;
        context->OutputBuffer = irp->UserBuffer;
    } else {
        context->InputBuffer = NULL;
        context->OutputBuffer = NULL;
    }

    RclTraceRequest(context);

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, RclEvtRequestCompletion, WDF_NO_CONTEXT);

    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(Request);
        RclTraceMessage(RCL_LEVEL_ERROR, "WdfRequestSend failed", context->Device->Name, status);
        RclTraceCompletion(context, status, 0);
        WdfRequestComplete(Request, status);
    }
}

static VOID
RclEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    RclForward(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode, FALSE);
}

static VOID
RclEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    RclForward(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode, TRUE);
}

static VOID
RclEvtRequestCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PRCL_REQUEST_CONTEXT context = RclGetRequestContext(Request);
    NTSTATUS status = Params->IoStatus.Status;
    ULONG_PTR information = Params->IoStatus.Information;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    RclTraceCompletion(context, status, information);

    WdfRequestCompleteWithInformation(Request, status, information);
}
