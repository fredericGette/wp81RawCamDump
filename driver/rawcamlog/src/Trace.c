/*
 * ETW provider "WP81-RawCamLog" {0B6F5E42-1C3D-4D2B-9C8E-5A7D21F0C3A1}.
 *
 * Plain string events (EtwWriteString, no manifest needed).
 *
 *   REQ #12 VFE I 0x23241B n=7 OPERATION_CONFIG in=60 out=16 qpc=0x1234abcd
 *   REQ #12 in+0000: 01 00 00 00 ...
 *   CPL #12 VFE n=7 OPERATION_CONFIG status=0x00000000 STATUS_SUCCESS info=0 result=0 dt=123
 *   CPL #12 resp+0000: ...
 *
 * Long buffers are split in lines of RCL_DUMP_LINE_BYTES bytes, one event
 * per line, up to RCL_DUMP_MAX_BYTES per buffer. "dt" and "qpc" are in
 * performance counter ticks; the frequency is written in the load event.
 */
#include "RawCamLog.h"

/* {0B6F5E42-1C3D-4D2B-9C8E-5A7D21F0C3A1} */
static const GUID RclProviderGuid =
    { 0x0b6f5e42, 0x1c3d, 0x4d2b, { 0x9c, 0x8e, 0x5a, 0x7d, 0x21, 0xf0, 0xc3, 0xa1 } };

/* 512 WCHARs = 1 KB of stack: the completion routine can run nested in the lower driver. */
#define RCL_TRACE_CHARS 512

#define RCL_CAMERA_IOCTL_BASE   0x2323FF    /* ((0x8FF + 0) << 2) | 0x230003 */

/* 32-bit ARM Windows: the kernel address space starts at 2 GB. */
#define RCL_SYSTEM_RANGE_START  0x80000000UL

/* ntddk.h is not in the installed kits. */
NTKERNELAPI BOOLEAN MmIsAddressValid(_In_ PVOID VirtualAddress);

static REGHANDLE RclEtwHandle;
static BOOLEAN RclEtwRegistered;

NTSTATUS
RclTraceRegister(VOID)
{
    NTSTATUS status = EtwRegister(&RclProviderGuid, NULL, NULL, &RclEtwHandle);

    RclEtwRegistered = NT_SUCCESS(status);
    return status;
}

VOID
RclTraceUnregister(VOID)
{
    if (RclEtwRegistered) {
        RclEtwRegistered = FALSE;
        EtwUnregister(RclEtwHandle);
    }
}

BOOLEAN
RclTraceEnabled(
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword)
{
    return RclEtwRegistered && EtwProviderEnabled(RclEtwHandle, Level, Keyword);
}

static VOID
RclTraceWrite(
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ const RCL_TEXT *Text)
{
    EtwWriteString(RclEtwHandle, Level, Keyword, NULL, Text->Buffer);
}

VOID
RclTraceMessage(
    _In_ UCHAR Level,
    _In_z_ PCSTR Message,
    _In_opt_z_ PCSTR Detail,
    _In_ NTSTATUS Status)
{
    WCHAR buffer[RCL_TRACE_CHARS];
    RCL_TEXT text;
    LARGE_INTEGER frequency;

    if (!RclTraceEnabled(Level, RCL_KEYWORD_DIAGNOSTIC)) {
        return;
    }

    KeQueryPerformanceCounter(&frequency);

    RclTextInit(&text, buffer, ARRAYSIZE(buffer));
    RclTextAppend(&text, "RawCamLog: ");
    RclTextAppend(&text, Message);
    if (Detail != NULL) {
        RclTextAppend(&text, " [");
        RclTextAppend(&text, Detail);
        RclTextAppend(&text, "]");
    }
    RclTextAppend(&text, ", status=");
    RclAppendNtStatus(&text, Status);
    RclTextAppend(&text, ", qpcfreq=");
    RclTextAppendUnsigned(&text, frequency.LowPart);

    RclTraceWrite(Level, RCL_KEYWORD_DIAGNOSTIC, &text);
}

/*
 * TRUE when every page of [Address, Address + Length) is resident, so it can
 * be read at any IRQL. The buffers come from other kernel drivers; a bad
 * pointer is skipped rather than dereferenced.
 */
static BOOLEAN
RclIsRangeReadable(
    _In_opt_ const VOID *Address,
    _In_ ULONG Length)
{
    ULONG_PTR start = (ULONG_PTR)Address;
    ULONG_PTR end;
    ULONG_PTR page;

    if (Address == NULL || Length == 0) {
        return FALSE;
    }
    if (start < RCL_SYSTEM_RANGE_START) {
        return FALSE;
    }
    end = start + Length - 1;
    if (end < start) {
        return FALSE;
    }

    for (page = start & ~(ULONG_PTR)(PAGE_SIZE - 1); ; page += PAGE_SIZE) {
        if (!MmIsAddressValid((PVOID)page)) {
            return FALSE;
        }
        if (page >= (end & ~(ULONG_PTR)(PAGE_SIZE - 1))) {
            break;
        }
    }
    return TRUE;
}

static ULONG
RclReadUlong(
    _In_ const UCHAR *Buffer,
    _In_ ULONG Offset)
{
    return (ULONG)Buffer[Offset] | ((ULONG)Buffer[Offset + 1] << 8) |
           ((ULONG)Buffer[Offset + 2] << 16) | ((ULONG)Buffer[Offset + 3] << 24);
}

static USHORT
RclReadUshort(
    _In_ const UCHAR *Buffer,
    _In_ ULONG Offset)
{
    return (USHORT)(Buffer[Offset] | (Buffer[Offset + 1] << 8));
}

/* "REQ #12 " / "CPL #12 " */
static VOID
RclAppendPrefix(
    _Inout_ PRCL_TEXT Text,
    _In_z_ PCSTR Kind,
    _In_ ULONG RequestId)
{
    RclTextAppend(Text, Kind);
    RclTextAppend(Text, " #");
    RclTextAppendUnsigned(Text, RequestId);
    RclTextAppend(Text, " ");
}

/*
 * Dumps a buffer as lines "REQ #12 in+0040: AA BB ...", one event per line.
 * The caller checked that the range is readable.
 */
static VOID
RclTraceDump(
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_z_ PCSTR Kind,
    _In_ ULONG RequestId,
    _In_z_ PCSTR Label,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    WCHAR buffer[RCL_TRACE_CHARS];
    RCL_TEXT text;
    ULONG dumped = (Length > RCL_DUMP_MAX_BYTES) ? RCL_DUMP_MAX_BYTES : Length;
    ULONG offset;

    for (offset = 0; offset < dumped; offset += RCL_DUMP_LINE_BYTES) {
        ULONG count = dumped - offset;
        if (count > RCL_DUMP_LINE_BYTES) {
            count = RCL_DUMP_LINE_BYTES;
        }

        RclTextInit(&text, buffer, ARRAYSIZE(buffer));
        RclAppendPrefix(&text, Kind, RequestId);
        RclTextAppend(&text, Label);
        RclTextAppend(&text, "+");
        RclTextAppendHex(&text, offset, 4);
        RclTextAppend(&text, ": ");
        RclTextAppendRawBytes(&text, Buffer + offset, count, count);
        if (offset + count == dumped && dumped < Length) {
            RclTextAppend(&text, " (truncated, total ");
            RclTextAppendUnsigned(&text, Length);
            RclTextAppend(&text, ")");
        }
        RclTraceWrite(Level, Keyword, &text);
    }
}

/* Command number of a sensor/VFE internal IOCTL ((0x8FF + n) << 2 | 0x230003), or MAXULONG. */
static ULONG
RclCameraCommand(
    _In_ ULONG IoControlCode)
{
    if ((IoControlCode & 0xFFFF0003) != 0x00230003 || IoControlCode < RCL_CAMERA_IOCTL_BASE) {
        return MAXULONG;
    }
    return (IoControlCode - RCL_CAMERA_IOCTL_BASE) >> 2;
}

static VOID
RclAppendRequestName(
    _Inout_ PRCL_TEXT Text,
    _In_ const RCL_REQUEST_CONTEXT *Context)
{
    ULONG command = RclCameraCommand(Context->IoControlCode);

    switch (Context->Device->Kind) {
    case RclDeviceSensor:
        if (command != MAXULONG) {
            RclTextAppend(Text, "n=");
            RclTextAppendUnsigned(Text, command);
            RclTextAppend(Text, " ");
            RclTextAppend(Text, RclSensorCommandName(command));
            return;
        }
        break;
    case RclDeviceVfe:
        if (Context->IoControlCode == 0x232003) {
            RclTextAppend(Text, "REGISTER_MESSAGE_EVENT");
            return;
        }
        if (command != MAXULONG) {
            RclTextAppend(Text, "n=");
            RclTextAppendUnsigned(Text, command);
            RclTextAppend(Text, " ");
            RclTextAppend(Text, RclVfeCommandName(command));
            return;
        }
        break;
    case RclDevicePlatform:
        RclTextAppend(Text, RclPlatformIoctlName(Context->IoControlCode));
        return;
    default:
        break;
    }
    RclTextAppend(Text, "?");
}

/*
 * Platform I2C transfer (36 bytes): +0x04 slave, +0x08 flags, +0x10 data
 * pointer, +0x14 register, +0x16 byte count, +0x18 write count (sequential).
 */
static VOID
RclTraceI2c(
    _In_z_ PCSTR Kind,
    _In_ const RCL_REQUEST_CONTEXT *Context,
    _In_ const UCHAR *Transfer,
    _In_ BOOLEAN DumpData)
{
    WCHAR buffer[RCL_TRACE_CHARS];
    RCL_TEXT text;
    ULONG flags = RclReadUlong(Transfer, 0x08);
    const UCHAR *data = (const UCHAR *)(ULONG_PTR)RclReadUlong(Transfer, 0x10);
    USHORT reg = RclReadUshort(Transfer, 0x14);
    USHORT count = RclReadUshort(Transfer, 0x16);
    USHORT writes = RclReadUshort(Transfer, 0x18);
    ULONG dataLength = count;

    if (Context->IoControlCode == 0x232023) {
        /* Table of [register (1 or 2 bytes), value...] entries. */
        dataLength = (ULONG)writes * (((flags == 2) ? 2u : 1u) + count);
    }

    RclTextInit(&text, buffer, ARRAYSIZE(buffer));
    RclAppendPrefix(&text, Kind, Context->RequestId);
    RclTextAppend(&text, "i2c slave=");
    RclTextAppendHex(&text, Transfer[0x04], 2);
    RclTextAppend(&text, " flags=");
    RclTextAppendHex(&text, flags, 3);
    RclTextAppend(&text, " reg=");
    RclTextAppendHex(&text, reg, 4);
    RclTextAppend(&text, " count=");
    RclTextAppendUnsigned(&text, count);
    if (Context->IoControlCode == 0x232023) {
        RclTextAppend(&text, " writes=");
        RclTextAppendUnsigned(&text, writes);
    }
    if (DumpData) {
        RclTextAppend(&text, " data=");
        if (RclIsRangeReadable(data, dataLength) && dataLength <= RCL_DUMP_LINE_BYTES / 2) {
            RclTextAppendRawBytes(&text, data, dataLength, dataLength);
        } else if (!RclIsRangeReadable(data, dataLength)) {
            RclTextAppend(&text, "(not readable)");
        } else {
            RclTextAppend(&text, "(see dump)");
        }
    }
    RclTraceWrite(RCL_LEVEL_INFORMATION, RCL_KEYWORD_I2C, &text);

    if (DumpData && dataLength > RCL_DUMP_LINE_BYTES / 2 && RclIsRangeReadable(data, dataLength)) {
        RclTraceDump(RCL_LEVEL_INFORMATION, RCL_KEYWORD_I2C, Kind, Context->RequestId, "i2cdata", data, dataLength);
    }
}

VOID
RclTraceRequest(
    _In_ const RCL_REQUEST_CONTEXT *Context)
{
    WCHAR buffer[RCL_TRACE_CHARS];
    RCL_TEXT text;
    const UCHAR *input = (const UCHAR *)Context->InputBuffer;

    if (!RclTraceEnabled(RCL_LEVEL_INFORMATION, RCL_KEYWORD_REQUEST)) {
        return;
    }

    RclTextInit(&text, buffer, ARRAYSIZE(buffer));
    RclAppendPrefix(&text, "REQ", Context->RequestId);
    RclTextAppend(&text, Context->Device->Name);
    RclTextAppend(&text, Context->Internal ? " I " : " D ");
    RclTextAppendHex(&text, Context->IoControlCode, 8);
    RclTextAppend(&text, " ");
    RclAppendRequestName(&text, Context);
    RclTextAppend(&text, " in=");
    RclTextAppendUnsigned(&text, Context->InputLength);
    RclTextAppend(&text, " out=");
    RclTextAppendUnsigned(&text, Context->OutputLength);
    RclTextAppend(&text, Context->KernelCaller ? " caller=K" : " caller=U");
    RclTextAppend(&text, " irql=");
    RclTextAppendUnsigned(&text, KeGetCurrentIrql());
    RclTextAppend(&text, " qpc=");
    RclTextAppendHex(&text, Context->StartTime.LowPart, 8);
    if (Context->Device->Kind == RclDeviceVfe && Context->IoControlCode == 0x232003) {
        /* The input "buffer" is the client's KEVENT pointer itself. */
        RclTextAppend(&text, " event=");
        RclTextAppendHex(&text, (ULONG)(ULONG_PTR)Context->InputBuffer, 8);
        input = NULL;
    }
    RclTraceWrite(RCL_LEVEL_INFORMATION, RCL_KEYWORD_REQUEST, &text);

    if (Context->Device->Kind == RclDevicePlatform) {
        if ((Context->IoControlCode == 0x23200F || Context->IoControlCode == 0x232023) &&
            Context->InputLength >= 0x24 && RclIsRangeReadable(input, 0x24)) {
            RclTraceI2c("REQ", Context, input, TRUE);
        }
        if (Context->IoControlCode == 0x23200B &&
            RclIsRangeReadable(Context->OutputBuffer, 0x24)) {
            RclTraceI2c("REQ", Context, (const UCHAR *)Context->OutputBuffer, FALSE);
        }
        if (Context->IoControlCode == 0x23200B || Context->IoControlCode == 0x23200F ||
            Context->IoControlCode == 0x232023) {
            return;
        }
    }

    if (input != NULL && Context->InputLength != 0 && RclIsRangeReadable(input, Context->InputLength)) {
        RclTraceDump(RCL_LEVEL_INFORMATION, RCL_KEYWORD_REQUEST, "REQ", Context->RequestId,
                     "in", input, Context->InputLength);
    }
}

VOID
RclTraceCompletion(
    _In_ const RCL_REQUEST_CONTEXT *Context,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    WCHAR buffer[RCL_TRACE_CHARS];
    RCL_TEXT text;
    UCHAR level = NT_ERROR(Status) ? RCL_LEVEL_WARNING : RCL_LEVEL_INFORMATION;
    const UCHAR *descriptor = (const UCHAR *)Context->OutputBuffer;
    const UCHAR *response = NULL;
    ULONG responseLength = 0;
    BOOLEAN hasDescriptor;
    LARGE_INTEGER now;

    if (!RclTraceEnabled(level, RCL_KEYWORD_COMPLETION)) {
        return;
    }

    now = KeQueryPerformanceCounter(NULL);

    /* Sensor and VFE commands carry the 16-byte response descriptor. */
    hasDescriptor = (Context->Device->Kind == RclDeviceSensor || Context->Device->Kind == RclDeviceVfe) &&
                    Context->Internal && Context->OutputLength >= 16 &&
                    RclIsRangeReadable(descriptor, 16);

    RclTextInit(&text, buffer, ARRAYSIZE(buffer));
    RclAppendPrefix(&text, "CPL", Context->RequestId);
    RclTextAppend(&text, Context->Device->Name);
    RclTextAppend(&text, " ");
    RclAppendRequestName(&text, Context);
    RclTextAppend(&text, " status=");
    RclAppendNtStatus(&text, Status);
    RclTextAppend(&text, " info=");
    RclTextAppendUnsigned(&text, (ULONG)Information);
    RclTextAppend(&text, " dt=");
    RclTextAppendUnsigned(&text, (ULONG)(now.QuadPart - Context->StartTime.QuadPart));

    if (hasDescriptor) {
        const UCHAR *responseBuffer = (const UCHAR *)(ULONG_PTR)RclReadUlong(descriptor, 0x00);
        ULONG responseSize = RclReadUlong(descriptor, 0x04);
        const UCHAR *lengthPointer = (const UCHAR *)(ULONG_PTR)RclReadUlong(descriptor, 0x08);

        RclTextAppend(&text, " result=");
        RclTextAppendUnsigned(&text, RclReadUlong(descriptor, 0x0C));

        if (Context->Device->Kind == RclDeviceVfe && Context->IoControlCode == 0x232003) {
            /* +0x00 points to a PVOID that receives the message queue address. */
            RclTextAppend(&text, " queuePtr=");
            if (RclIsRangeReadable(responseBuffer, 4)) {
                RclTextAppendHex(&text, RclReadUlong(responseBuffer, 0), 8);
            } else {
                RclTextAppend(&text, "(flush)");
            }
        } else {
            responseLength = responseSize;
            if (RclIsRangeReadable(lengthPointer, 4)) {
                ULONG reported = RclReadUlong(lengthPointer, 0);
                if (reported < responseLength) {
                    responseLength = reported;
                }
            }
            RclTextAppend(&text, " resp=");
            RclTextAppendUnsigned(&text, responseLength);
            RclTextAppend(&text, "/");
            RclTextAppendUnsigned(&text, responseSize);
            if (responseLength != 0 && RclIsRangeReadable(responseBuffer, responseLength)) {
                response = responseBuffer;
            }
        }
    }
    RclTraceWrite(level, RCL_KEYWORD_COMPLETION, &text);

    if (response != NULL) {
        RclTraceDump(level, RCL_KEYWORD_COMPLETION, "CPL", Context->RequestId, "resp", response, responseLength);
    }

    if (Context->Device->Kind == RclDevicePlatform && Context->IoControlCode == 0x23200B &&
        NT_SUCCESS(Status) && RclIsRangeReadable(descriptor, 0x24)) {
        RclTraceI2c("CPL", Context, descriptor, TRUE);
    }
}
