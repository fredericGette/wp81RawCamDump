/*
 * wp81rawcamlog - KMDF 1.11 upper filter that logs the internal IOCTLs sent
 * to the Lumia 520 camera hardware drivers:
 *   - qccamsensor8930.sys   (ACPI QCOM0710, back sensor)
 *   - qccamvfe8930.sys      (ACPI QCOM0700, VFE + ISPIF)
 *   - qccamplatform8930.sys (ACPI QCOM0775, camera I2C / GPIO / power)
 *
 * All their internal IOCTLs are METHOD_NEITHER and sent by kernel drivers
 * (qccamavs8930.sys, qccamsensor8930.sys):
 *   - Type3InputBuffer = the command structure (kernel address)
 *   - Irp->UserBuffer  = a 16-byte descriptor for sensor/VFE commands:
 *       +0x00 response buffer, +0x04 response size,
 *       +0x08 PULONG response length, +0x0C camera result
 *     or the 36-byte I2C transfer for the platform read (0x23200B).
 *
 * Every request is logged when it arrives (command name and full input bytes)
 * and when it completes (status, camera result and response bytes). Events
 * are plain strings written with EtwWriteString to the provider
 * "WP81-RawCamLog" {0B6F5E42-1C3D-4D2B-9C8E-5A7D21F0C3A1}.
 */
#pragma once

#include <wdm.h>
#include <wdf.h>

#define RCL_KEYWORD_REQUEST     0x1ULL  /* request received, input dumped     */
#define RCL_KEYWORD_COMPLETION  0x2ULL  /* request completed, response dumped */
#define RCL_KEYWORD_DIAGNOSTIC  0x4ULL  /* load / attach / failures           */
#define RCL_KEYWORD_I2C         0x8ULL  /* platform I2C transfers (decoded)   */

#define RCL_LEVEL_ERROR         2
#define RCL_LEVEL_WARNING       3
#define RCL_LEVEL_INFORMATION   4
#define RCL_LEVEL_VERBOSE       5

/* Bytes per hex dump line (one ETW event per line) and maximum dumped per buffer. */
#define RCL_DUMP_LINE_BYTES     64
#define RCL_DUMP_MAX_BYTES      4096

/* Used by Text.c for short dumps. */
#define RCL_RAW_DUMP_MAX        64

typedef enum _RCL_DEVICE_KIND {
    RclDeviceUnknown = 0,
    RclDeviceSensor,
    RclDeviceVfe,
    RclDevicePlatform,
} RCL_DEVICE_KIND;

typedef struct _RCL_DEVICE_CONTEXT {
    RCL_DEVICE_KIND Kind;
    PCSTR Name;                         /* "SENSOR", "VFE", "PLATFORM", "?" */
} RCL_DEVICE_CONTEXT, *PRCL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RCL_DEVICE_CONTEXT, RclGetDeviceContext)

typedef struct _RCL_REQUEST_CONTEXT {
    ULONG RequestId;
    ULONG IoControlCode;
    BOOLEAN Internal;
    BOOLEAN KernelCaller;               /* Irp->RequestorMode == KernelMode */
    PVOID InputBuffer;                  /* Type3InputBuffer                  */
    ULONG InputLength;
    PVOID OutputBuffer;                 /* Irp->UserBuffer                   */
    ULONG OutputLength;
    LARGE_INTEGER StartTime;            /* KeQueryPerformanceCounter         */
    PRCL_DEVICE_CONTEXT Device;
} RCL_REQUEST_CONTEXT, *PRCL_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RCL_REQUEST_CONTEXT, RclGetRequestContext)

typedef struct _RCL_TEXT {
    PWCHAR  Buffer;
    ULONG   Capacity;
    ULONG   Length;
    BOOLEAN Truncated;
} RCL_TEXT, *PRCL_TEXT;

/* Text.c */
VOID RclTextInit(_Out_ PRCL_TEXT Text, _Out_writes_(Capacity) PWCHAR Buffer, _In_ ULONG Capacity);
VOID RclTextAppend(_Inout_ PRCL_TEXT Text, _In_z_ PCSTR String);
VOID RclTextAppendUnsigned(_Inout_ PRCL_TEXT Text, _In_ ULONG Value);
VOID RclTextAppendSigned(_Inout_ PRCL_TEXT Text, _In_ LONG Value);
VOID RclTextAppendHex(_Inout_ PRCL_TEXT Text, _In_ ULONG Value, _In_ ULONG Digits);
VOID RclTextAppendRawBytes(_Inout_ PRCL_TEXT Text, _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
                           _In_ ULONG Length, _In_ ULONG ReturnedLength);

/* Names.c */
PCSTR RclSensorCommandName(_In_ ULONG Command);
PCSTR RclVfeCommandName(_In_ ULONG Command);
PCSTR RclPlatformIoctlName(_In_ ULONG IoControlCode);
VOID RclAppendNtStatus(_Inout_ PRCL_TEXT Text, _In_ NTSTATUS Status);

/* Trace.c */
NTSTATUS RclTraceRegister(VOID);
VOID RclTraceUnregister(VOID);
BOOLEAN RclTraceEnabled(_In_ UCHAR Level, _In_ ULONGLONG Keyword);
VOID RclTraceMessage(_In_ UCHAR Level, _In_z_ PCSTR Message, _In_opt_z_ PCSTR Detail, _In_ NTSTATUS Status);
VOID RclTraceRequest(_In_ const RCL_REQUEST_CONTEXT *Context);
VOID RclTraceCompletion(_In_ const RCL_REQUEST_CONTEXT *Context, _In_ NTSTATUS Status, _In_ ULONG_PTR Information);
