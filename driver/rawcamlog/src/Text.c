/*
 * Bounded wide-character text builder used to compose the ETW messages.
 * No CRT or ntstrsafe formatting, so it is safe at DISPATCH_LEVEL.
 */
#include "RawCamLog.h"

/* WCHARs kept free for the "..." written when the text is truncated. */
#define RCL_TEXT_ELLIPSIS_CHARS 3

static const CHAR RclHexDigits[] = "0123456789ABCDEF";

VOID
RclTextInit(
    _Out_ PRCL_TEXT Text,
    _Out_writes_(Capacity) PWCHAR Buffer,
    _In_ ULONG Capacity)
{
    NT_ASSERT(Capacity > RCL_TEXT_ELLIPSIS_CHARS + 1);

    Text->Buffer = Buffer;
    Text->Capacity = Capacity;
    Text->Length = 0;
    Text->Truncated = FALSE;
    Buffer[0] = L'\0';
}

static VOID
RclTextPutChar(
    _Inout_ PRCL_TEXT Text,
    _In_ CHAR Char)
{
    ULONG i;

    if (Text->Truncated) {
        return;
    }

    if (Text->Length + RCL_TEXT_ELLIPSIS_CHARS + 1 < Text->Capacity) {
        Text->Buffer[Text->Length++] = (WCHAR)(UCHAR)Char;
        Text->Buffer[Text->Length] = L'\0';
        return;
    }

    Text->Truncated = TRUE;
    for (i = 0; i < RCL_TEXT_ELLIPSIS_CHARS; i++) {
        Text->Buffer[Text->Length++] = L'.';
    }
    Text->Buffer[Text->Length] = L'\0';
}

VOID
RclTextAppend(
    _Inout_ PRCL_TEXT Text,
    _In_z_ PCSTR String)
{
    while (*String != '\0') {
        RclTextPutChar(Text, *String++);
    }
}

VOID
RclTextAppendUnsigned(
    _Inout_ PRCL_TEXT Text,
    _In_ ULONG Value)
{
    CHAR digits[10];
    ULONG count = 0;

    do {
        digits[count++] = (CHAR)('0' + (Value % 10));
        Value /= 10;
    } while (Value != 0);

    while (count != 0) {
        RclTextPutChar(Text, digits[--count]);
    }
}

VOID
RclTextAppendSigned(
    _Inout_ PRCL_TEXT Text,
    _In_ LONG Value)
{
    if (Value < 0) {
        RclTextPutChar(Text, '-');
        RclTextAppendUnsigned(Text, 0u - (ULONG)Value);
    } else {
        RclTextAppendUnsigned(Text, (ULONG)Value);
    }
}

/* Appends "0x" followed by exactly Digits hex digits (1-8). */
VOID
RclTextAppendHex(
    _Inout_ PRCL_TEXT Text,
    _In_ ULONG Value,
    _In_ ULONG Digits)
{
    if (Digits == 0 || Digits > 8) {
        Digits = 8;
    }

    RclTextAppend(Text, "0x");
    while (Digits != 0) {
        Digits--;
        RclTextPutChar(Text, RclHexDigits[(Value >> (Digits * 4)) & 0xF]);
    }
}

/*
 * Appends up to RCL_RAW_DUMP_MAX bytes as "AA BB CC". When ReturnedLength is
 * smaller than Length, a "|" marks where the bytes returned to the caller end.
 */
VOID
RclTextAppendRawBytes(
    _Inout_ PRCL_TEXT Text,
    _In_reads_bytes_opt_(Length) const UCHAR *Buffer,
    _In_ ULONG Length,
    _In_ ULONG ReturnedLength)
{
    ULONG count;
    ULONG i;

    if (Length == 0) {
        RclTextAppend(Text, "(none)");
        return;
    }
    if (Buffer == NULL) {
        RclTextAppend(Text, "(not captured)");
        return;
    }

    count = (Length < RCL_RAW_DUMP_MAX) ? Length : RCL_RAW_DUMP_MAX;
    for (i = 0; i < count; i++) {
        if (i != 0) {
            RclTextPutChar(Text, ' ');
        }
        if (i == ReturnedLength) {
            RclTextAppend(Text, "| ");
        }
        RclTextPutChar(Text, RclHexDigits[Buffer[i] >> 4]);
        RclTextPutChar(Text, RclHexDigits[Buffer[i] & 0xF]);
    }

    if (count < Length) {
        RclTextAppend(Text, " ... (+");
        RclTextAppendUnsigned(Text, Length - count);
        RclTextAppend(Text, " bytes)");
    }
}
