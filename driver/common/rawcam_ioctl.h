/*
 * Interface between wp81RawCamDump.exe and wp81rawcam.sys.
 * Shared by the kernel driver and the user-mode application: only basic
 * Windows types are used, and CTL_CODE must already be defined.
 *
 * The driver is a thin, generic bridge to the camera hardware drivers. All
 * the capture logic (sequence, parameters) is in the application.
 *
 * Device: \\.\wp81rawcam  (kernel: \Device\wp81rawcam, \DosDevices\wp81rawcam)
 */
#pragma once

#define RAWCAM_DEVICE_NAME      L"\\Device\\wp81rawcam"
#define RAWCAM_SYMLINK_NAME     L"\\DosDevices\\wp81rawcam"
#define RAWCAM_USER_PATH        "\\\\.\\wp81rawcam"

#define RAWCAM_DEVICE_TYPE      0x8000

/* One session at a time, owned by the file handle that opened it. Closing the handle tears it down. */
#define IOCTL_RAWCAM_OPEN           CTL_CODE(RAWCAM_DEVICE_TYPE, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RAWCAM_CLOSE          CTL_CODE(RAWCAM_DEVICE_TYPE, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Command passthrough: in RAWCAM_CMD_INPUT, out RAWCAM_CMD_OUTPUT. */
#define IOCTL_RAWCAM_SENSOR_CMD     CTL_CODE(RAWCAM_DEVICE_TYPE, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RAWCAM_VFE_CMD        CTL_CODE(RAWCAM_DEVICE_TYPE, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Camera I2C through qccamplatform8930.sys: in/out RAWCAM_I2C. */
#define IOCTL_RAWCAM_I2C_READ       CTL_CODE(RAWCAM_DEVICE_TYPE, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RAWCAM_I2C_WRITE      CTL_CODE(RAWCAM_DEVICE_TYPE, 0x905, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Frame buffers in the VFE SMMU context: in RAWCAM_ALLOC_INPUT, out RAWCAM_ALLOC_OUTPUT. */
#define IOCTL_RAWCAM_ALLOC_FRAME    CTL_CODE(RAWCAM_DEVICE_TYPE, 0x906, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RAWCAM_FREE_FRAME     CTL_CODE(RAWCAM_DEVICE_TYPE, 0x907, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* in RAWCAM_WAIT_INPUT, out RAWCAM_VFE_MESSAGE. STATUS_TIMEOUT (ERROR_TIMEOUT / WAIT_TIMEOUT) when nothing arrived. */
#define IOCTL_RAWCAM_WAIT_VFE_MSG   CTL_CODE(RAWCAM_DEVICE_TYPE, 0x908, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* in RAWCAM_READ_INPUT, out = the frame bytes (direct I/O). */
#define IOCTL_RAWCAM_READ_FRAME     CTL_CODE(RAWCAM_DEVICE_TYPE, 0x909, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
/* in RAWCAM_PLATFORM_INPUT: platform power/clock/GPIO internal IOCTLs without buffers. */
#define IOCTL_RAWCAM_PLATFORM_CMD   CTL_CODE(RAWCAM_DEVICE_TYPE, 0x90A, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RAWCAM_MAX_CMD_INPUT        16384
#define RAWCAM_MAX_CMD_RESPONSE     65536
#define RAWCAM_MAX_FRAMES           4
#define RAWCAM_MAX_FRAME_SIZE       (64u * 1024 * 1024)

/* Flags of IOCTL_RAWCAM_OPEN. */
#define RAWCAM_OPEN_SENSOR          0x1     /* back sensor interface   */
#define RAWCAM_OPEN_VFE             0x2     /* VFE interface + message queue */
#define RAWCAM_OPEN_PLATFORM        0x4     /* camera platform (I2C)   */
#define RAWCAM_OPEN_SMMU            0x8     /* VCM on the VFE SMMU contexts 13 and 14 */
#define RAWCAM_OPEN_ALL             0xF

typedef struct _RAWCAM_OPEN_INPUT {
    unsigned long Flags;
} RAWCAM_OPEN_INPUT;

typedef struct _RAWCAM_OPEN_OUTPUT {
    unsigned long OpenedFlags;          /* RAWCAM_OPEN_* actually opened */
    unsigned long VfeQueueAddress;      /* kernel address of the VFE message queue (diagnostic) */
} RAWCAM_OPEN_OUTPUT;

/*
 * Sensor / VFE command: internal IOCTL ((0x8FF + Command) << 2) | 0x230003
 * with Input as Type3InputBuffer and a 16-byte response descriptor built by
 * the driver.
 */
typedef struct _RAWCAM_CMD_INPUT {
    unsigned long Command;              /* n */
    unsigned long InputLength;          /* bytes in Input, <= RAWCAM_MAX_CMD_INPUT */
    unsigned long ResponseLength;       /* size of the response buffer given to the command, 0 = none */
    unsigned char Input[1];             /* InputLength bytes */
} RAWCAM_CMD_INPUT;

#define RAWCAM_CMD_INPUT_HEADER     (3 * sizeof(unsigned long))

typedef struct _RAWCAM_CMD_OUTPUT {
    long          Status;               /* NTSTATUS of the IRP */
    unsigned long CameraResult;         /* descriptor +0x0C, 0 = success */
    unsigned long ResponseLength;       /* bytes written to Response (reported length, capped) */
    unsigned char Response[1];
} RAWCAM_CMD_OUTPUT;

#define RAWCAM_CMD_OUTPUT_HEADER    (3 * sizeof(unsigned long))

/* SMIA 16-bit register access at slave 0x20 by default. */
typedef struct _RAWCAM_I2C {
    unsigned char  Slave;               /* 8-bit address, 0 = 0x20 (back sensor) */
    unsigned char  Reserved;
    unsigned short Register;
    unsigned long  Flags;               /* platform flags, 0 = 2 (16-bit register address) */
    unsigned long  Length;              /* 1..32 */
    unsigned char  Data[32];
    long           Status;              /* out: NTSTATUS */
} RAWCAM_I2C;

typedef struct _RAWCAM_PLATFORM_INPUT {
    unsigned long IoControlCode;        /* 0x232003 ... 0x232023, buffer-less ones only, or 0x232013/0x232017 */
    unsigned long Value[2];             /* ULONG input for 0x232013 (kHz) / 0x232017 (GPIO id, value) */
} RAWCAM_PLATFORM_INPUT;

typedef struct _RAWCAM_ALLOC_INPUT {
    unsigned long Size;                 /* bytes, rounded up to 4 KB */
    unsigned long Fill;                 /* byte pattern written before use (detects unwritten areas) */
} RAWCAM_ALLOC_INPUT;

typedef struct _RAWCAM_ALLOC_OUTPUT {
    unsigned long Index;                /* 0 .. RAWCAM_MAX_FRAMES-1 */
    unsigned long DeviceAddress;        /* address to give to the VFE */
    unsigned long Size;
} RAWCAM_ALLOC_OUTPUT;

typedef struct _RAWCAM_FREE_INPUT {
    unsigned long Index;
} RAWCAM_FREE_INPUT;

typedef struct _RAWCAM_READ_INPUT {
    unsigned long Index;
    unsigned long Offset;
} RAWCAM_READ_INPUT;                    /* reads OutputBufferLength bytes */

typedef struct _RAWCAM_WAIT_INPUT {
    unsigned long TimeoutMs;
} RAWCAM_WAIT_INPUT;

/* VFE message (136 bytes in the VFE driver queue). */
typedef struct _RAWCAM_VFE_MESSAGE {
    unsigned long Id;
    unsigned long Size;
    unsigned char Payload[128];
    unsigned long Pending;              /* messages still queued after this one */
} RAWCAM_VFE_MESSAGE;
