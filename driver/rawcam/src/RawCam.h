/*
 * wp81rawcam - legacy (non-PnP) WDM control driver that gives user mode a
 * controlled access to the Lumia 520 camera hardware drivers, below the
 * AVStream camera driver (qccamavs8930.sys):
 *   - back sensor   qccamsensor8930.sys   {5e34e1c5-c5bc-4c7f-b6fc-e2443e45be67}
 *   - VFE / ISPIF   qccamvfe8930.sys      {3e9c0fdb-cef9-4c4a-8e85-36e4a82eb80f}
 *   - platform      qccamplatform8930.sys {8d73ce35-93cf-4795-8db9-40bbe130d859}
 *   - SMMU (VCM)    qcvcm8930.sys export driver, VFE context banks 13 and 14
 *
 * The devices are opened like qccamavs8930.sys does (IoGetDeviceInterfaces +
 * IoGetDeviceObjectPointer) and driven with the same METHOD_NEITHER internal
 * IOCTLs. Buffers given to the camera drivers are always non-paged copies.
 *
 * Unloadable: "sc stop wp81rawcam" / "sc start wp81rawcam".
 */
#pragma once

#include <wdm.h>

#include "../../common/rawcam_ioctl.h"

#define RC_POOL_TAG             'mcRw'

/* Full ARM barrier (KeMemoryBarrier needs _ReadWriteBarrier, which clang does not provide). */
#define RC_MEMORY_BARRIER()     __asm__ __volatile__("dmb sy" ::: "memory")

/* ETW provider "WP81-RawCam" {6D1C2B7A-4E8F-4A63-B1D2-8F0E5C3A9B47}. */
#define RC_LEVEL_ERROR          2
#define RC_LEVEL_WARNING        3
#define RC_LEVEL_INFORMATION    4
#define RC_LEVEL_VERBOSE        5

/* Internal IOCTL of a camera command n. */
#define RC_CAMERA_IOCTL(n)      ((((ULONG)0x8FF + (n)) << 2) | 0x230003)
#define RC_VFE_REGISTER_EVENT   0x232003

/* Platform internal IOCTLs. */
#define RC_PLATFORM_I2C_READ    0x23200B
#define RC_PLATFORM_I2C_WRITE   0x23200F

typedef struct _RC_TARGET {
    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT FileObject;
} RC_TARGET, *PRC_TARGET;

typedef struct _RC_FRAME {
    PMDL Mdl;
    PVOID Reservation;                  /* qcvcm8930 reservation */
    PUCHAR SystemAddress;
    ULONG DeviceAddress;
    ULONG Size;
} RC_FRAME, *PRC_FRAME;

typedef struct _RC_SESSION {
    PFILE_OBJECT Owner;                 /* file object that opened the session */
    ULONG OpenedFlags;
    RC_TARGET Sensor;
    RC_TARGET Vfe;
    RC_TARGET Platform;

    /* VFE message queue (owned by qccamvfe8930.sys). */
    PKEVENT VfeEvent;                   /* never freed, see VfeQueue.c */
    PUCHAR VfeQueue;

    /* SMMU */
    PVOID Vcm;
    PVOID Assoc[2];
    RC_FRAME Frames[RAWCAM_MAX_FRAMES];

    /* What we started, so the teardown can undo it. */
    BOOLEAN SensorPowered;
    BOOLEAN SensorStreaming;
    BOOLEAN VfePowered;
    BOOLEAN VfeStarted;
} RC_SESSION, *PRC_SESSION;

/* Trace.c */
VOID RcTraceRegister(VOID);
VOID RcTraceUnregister(VOID);
VOID RcTrace(_In_ UCHAR Level, _In_z_ _Printf_format_string_ PCWSTR Format, ...);

/* Camera.c */
NTSTATUS RcOpenTarget(_In_ const GUID *InterfaceGuid, _Out_ PRC_TARGET Target);
VOID RcCloseTarget(_Inout_ PRC_TARGET Target);
NTSTATUS RcSendInternalIoctl(_In_ PRC_TARGET Target, _In_ ULONG IoControlCode,
                             _In_opt_ PVOID InputBuffer, _In_ ULONG InputLength,
                             _In_opt_ PVOID OutputBuffer, _In_ ULONG OutputLength);
NTSTATUS RcCameraCommand(_In_ PRC_TARGET Target, _In_ ULONG Command,
                         _In_reads_bytes_opt_(InputLength) const VOID *Input, _In_ ULONG InputLength,
                         _Out_writes_bytes_opt_(ResponseSize) PVOID Response, _In_ ULONG ResponseSize,
                         _Out_ PULONG ResponseLength, _Out_ PULONG CameraResult);
NTSTATUS RcI2cTransfer(_In_ PRC_TARGET Platform, _In_ BOOLEAN Write, _Inout_ RAWCAM_I2C *Request);

/* Smmu.c */
NTSTATUS RcSmmuCreate(_Inout_ PRC_SESSION Session);
VOID RcSmmuDetach(_Inout_ PRC_SESSION Session);
VOID RcSmmuDestroy(_Inout_ PRC_SESSION Session);
NTSTATUS RcFrameAllocate(_Inout_ PRC_SESSION Session, _In_ const RAWCAM_ALLOC_INPUT *Input,
                         _Out_ RAWCAM_ALLOC_OUTPUT *Output);
NTSTATUS RcFrameFree(_Inout_ PRC_SESSION Session, _In_ ULONG Index);

/* VfeQueue.c */
NTSTATUS RcVfeQueueRegister(_Inout_ PRC_SESSION Session);
VOID RcVfeQueueUnregister(_Inout_ PRC_SESSION Session);
NTSTATUS RcVfeQueueWait(_Inout_ PRC_SESSION Session, _In_ ULONG TimeoutMs, _Out_ RAWCAM_VFE_MESSAGE *Message);
