/*
 * ETW provider "WP81-RawCam" {6D1C2B7A-4E8F-4A63-B1D2-8F0E5C3A9B47}.
 * Plain string events (EtwWriteString), formatted with the kernel's
 * _vsnwprintf. Only called at PASSIVE_LEVEL.
 */
#include "RawCam.h"
#include <stdarg.h>

int __cdecl _vsnwprintf(wchar_t *Buffer, size_t Count, const wchar_t *Format, va_list Arguments);

/* {6D1C2B7A-4E8F-4A63-B1D2-8F0E5C3A9B47} */
static const GUID RcProviderGuid =
    { 0x6d1c2b7a, 0x4e8f, 0x4a63, { 0xb1, 0xd2, 0x8f, 0x0e, 0x5c, 0x3a, 0x9b, 0x47 } };

static REGHANDLE RcEtwHandle;
static BOOLEAN RcEtwRegistered;

VOID
RcTraceRegister(VOID)
{
    RcEtwRegistered = NT_SUCCESS(EtwRegister(&RcProviderGuid, NULL, NULL, &RcEtwHandle));
}

VOID
RcTraceUnregister(VOID)
{
    if (RcEtwRegistered) {
        RcEtwRegistered = FALSE;
        EtwUnregister(RcEtwHandle);
    }
}

VOID
RcTrace(
    _In_ UCHAR Level,
    _In_z_ _Printf_format_string_ PCWSTR Format,
    ...)
{
    WCHAR buffer[256];
    va_list arguments;

    if (!RcEtwRegistered || !EtwProviderEnabled(RcEtwHandle, Level, 0)) {
        return;
    }

    va_start(arguments, Format);
    _vsnwprintf(buffer, ARRAYSIZE(buffer) - 1, Format, arguments);
    va_end(arguments);
    buffer[ARRAYSIZE(buffer) - 1] = L'\0';

    EtwWriteString(RcEtwHandle, Level, 0, NULL, buffer);
}
