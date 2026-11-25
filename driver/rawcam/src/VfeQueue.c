/*
 * Consumer of the VFE message queue of qccamvfe8930.sys.
 *
 * The VFE driver does not complete IRPs for its messages (frame done,
 * statistics, SOF...): it puts them in a queue of its device context and
 * sets the KEVENT registered by its client with internal IOCTL 0x232003:
 *   Type3InputBuffer = PKEVENT (the pointer itself)
 *   UserBuffer       = descriptor, +0x00 = PVOID* receiving the queue
 *                      address, or NULL to flush the queue.
 *
 * Queue header (20 bytes): +0x00 KSPIN_LOCK, +0x04 LIST_ENTRY of pending
 * entries, +0x0C entry count (512), +0x10 entries.
 * Entry (16 bytes): +0x00 LIST_ENTRY, +0x08 message pointer, +0x0C free flag.
 * Message (136 bytes): +0x00 id, +0x04 payload size (128), +0x08 payload.
 * The VFE inserts with ExInterlockedInsertTailList on the header lock; the
 * client removes the entries and sets their free flag back to 1.
 *
 * The VFE keeps the event pointer until the next client registers (AVS at
 * its next camera session), and may set it at any time until then. So the
 * event is allocated once from non-paged pool and never freed.
 */
#include "RawCam.h"

#define RC_VFE_QUEUE_ENTRIES    512
#define RC_VFE_MESSAGE_SIZE     136

static PKEVENT RcVfeEvent;      /* shared by all sessions of this driver instance, intentionally leaked */

typedef struct _RC_VFE_DESCRIPTOR {
    PVOID *QueueAddress;
    ULONG Reserved1;
    ULONG Reserved2;
    ULONG Result;
    PVOID Queue;                /* target of QueueAddress */
} RC_VFE_DESCRIPTOR, *PRC_VFE_DESCRIPTOR;

static NTSTATUS
RcVfeRegisterEvent(
    _In_ PRC_SESSION Session,
    _In_ BOOLEAN GetQueue,
    _Out_opt_ PVOID *Queue)
{
    PRC_VFE_DESCRIPTOR descriptor;
    NTSTATUS status;

    descriptor = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*descriptor), RC_POOL_TAG);
    if (descriptor == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(descriptor, sizeof(*descriptor));
    descriptor->QueueAddress = GetQueue ? &descriptor->Queue : NULL;

    /*
     * The input length must not be 0: the VFE driver only calls
     * WdfRequestRetrieveInputBuffer when it isn't, and otherwise stores a NULL
     * event, which silently disables every message (qccamavs8930 sends 16).
     */
    status = RcSendInternalIoctl(&Session->Vfe, RC_VFE_REGISTER_EVENT, RcVfeEvent, sizeof(KEVENT), descriptor, 16);
    if (Queue != NULL) {
        *Queue = descriptor->Queue;
    }

    ExFreePoolWithTag(descriptor, RC_POOL_TAG);
    return status;
}

NTSTATUS
RcVfeQueueRegister(
    _Inout_ PRC_SESSION Session)
{
    PVOID queue = NULL;
    NTSTATUS status;

    if (RcVfeEvent == NULL) {
        RcVfeEvent = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(KEVENT), RC_POOL_TAG);
        if (RcVfeEvent == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        KeInitializeEvent(RcVfeEvent, SynchronizationEvent, FALSE);
    }

    /* Drop what the previous client (AVS) may have left, then get the queue. */
    status = RcVfeRegisterEvent(Session, FALSE, NULL);
    if (NT_SUCCESS(status)) {
        status = RcVfeRegisterEvent(Session, TRUE, &queue);
    }
    if (!NT_SUCCESS(status)) {
        RcTrace(RC_LEVEL_ERROR, L"VFE event registration failed 0x%08X", status);
        return status;
    }

    if (queue == NULL || *(PULONG)((PUCHAR)queue + 0x0C) != RC_VFE_QUEUE_ENTRIES) {
        RcTrace(RC_LEVEL_ERROR, L"unexpected VFE queue 0x%p", queue);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    KeClearEvent(RcVfeEvent);
    Session->VfeEvent = RcVfeEvent;
    Session->VfeQueue = (PUCHAR)queue;
    RcTrace(RC_LEVEL_INFORMATION, L"VFE message queue at 0x%p", queue);
    return STATUS_SUCCESS;
}

VOID
RcVfeQueueUnregister(
    _Inout_ PRC_SESSION Session)
{
    if (Session->VfeQueue == NULL) {
        return;
    }
    /* Flush: every entry is freed. The event stays registered (never freed). */
    RcVfeRegisterEvent(Session, FALSE, NULL);
    Session->VfeQueue = NULL;
    Session->VfeEvent = NULL;
}

NTSTATUS
RcVfeQueueWait(
    _Inout_ PRC_SESSION Session,
    _In_ ULONG TimeoutMs,
    _Out_ RAWCAM_VFE_MESSAGE *Message)
{
    PKSPIN_LOCK lock;
    PLIST_ENTRY list;
    LARGE_INTEGER timeout;
    LARGE_INTEGER deadline;
    LARGE_INTEGER now;

    if (Session->VfeQueue == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    lock = (PKSPIN_LOCK)(Session->VfeQueue + 0x00);
    list = (PLIST_ENTRY)(Session->VfeQueue + 0x04);

    KeQuerySystemTime(&deadline);
    deadline.QuadPart += (LONGLONG)TimeoutMs * 10000;

    for (;;) {
        PLIST_ENTRY entry = ExInterlockedRemoveHeadList(list, lock);
        if (entry != NULL) {
            PUCHAR raw = (PUCHAR)entry;
            PUCHAR message = *(PUCHAR *)(raw + 0x08);

            Message->Id = *(PULONG)(message + 0x00);
            Message->Size = *(PULONG)(message + 0x04);
            RtlCopyMemory(Message->Payload, message + 0x08, sizeof(Message->Payload));
            Message->Pending = IsListEmpty(list) ? 0 : 1;

            /* Give the entry back to the VFE. */
            InterlockedExchange((volatile LONG *)(raw + 0x0C), 1);

            RcTrace(RC_LEVEL_VERBOSE, L"VFE message id=%u", Message->Id);
            return STATUS_SUCCESS;
        }

        KeQuerySystemTime(&now);
        if (now.QuadPart >= deadline.QuadPart) {
            return STATUS_TIMEOUT;
        }
        timeout.QuadPart = now.QuadPart - deadline.QuadPart;     /* relative, negative */
        KeWaitForSingleObject(Session->VfeEvent, Executive, KernelMode, FALSE, &timeout);
    }
}
