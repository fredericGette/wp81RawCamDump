#pragma once

#include "stdafx.h"

// Thin wrapper around \\.\wp81rawcam (see driver/common/rawcam_ioctl.h).
class RawCamDevice {
public:
	RawCamDevice();
	~RawCamDevice();

	bool Connect();                                         // opens the control device
	bool Open(unsigned long flags);                         // starts a session
	void Close();                                           // ends the session (also done by the handle close)

	// Sensor / VFE command. Returns false when the IOCTL itself fails;
	// status and cameraResult tell how the command went.
	bool Command(bool sensor, unsigned long n, const unsigned char *input, unsigned long inputLength,
		unsigned char *response, unsigned long responseSize, unsigned long *responseLength,
		long *status, unsigned long *cameraResult);

	bool I2cRead(unsigned char slave, unsigned short reg, unsigned char *data, unsigned long length, long *status);
	bool I2cWrite(unsigned char slave, unsigned short reg, const unsigned char *data, unsigned long length, long *status);
	bool Platform(unsigned long code, unsigned long value0, unsigned long value1);

	bool AllocFrame(unsigned long size, unsigned long fill, RAWCAM_ALLOC_OUTPUT *output);
	bool FreeFrame(unsigned long index);
	// Returns 1 when a message was received, 0 on timeout, -1 on error.
	int WaitVfeMessage(unsigned long timeoutMs, RAWCAM_VFE_MESSAGE *message);
	bool ReadFrame(unsigned long index, unsigned long offset, unsigned char *buffer, unsigned long length);

	unsigned long OpenedFlags() const { return m_openedFlags; }
	unsigned long VfeQueue() const { return m_vfeQueue; }
	DWORD LastError() const { return m_lastError; }

private:
	bool Ioctl(DWORD code, void *input, DWORD inputLength, void *output, DWORD outputLength, DWORD *returned);

	HANDLE m_handle;
	unsigned long m_openedFlags;
	unsigned long m_vfeQueue;
	DWORD m_lastError;
	unsigned char *m_buffer;                                // IOCTL scratch buffer
};
