#include "RawCamDevice.h"

#define RAWCAM_SCRATCH_SIZE (RAWCAM_CMD_OUTPUT_HEADER + RAWCAM_MAX_CMD_RESPONSE + 16)

RawCamDevice::RawCamDevice()
	: m_handle(INVALID_HANDLE_VALUE), m_openedFlags(0), m_vfeQueue(0), m_lastError(0)
{
	m_buffer = (unsigned char *)malloc(RAWCAM_SCRATCH_SIZE);
}

RawCamDevice::~RawCamDevice()
{
	if (m_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(m_handle);              // the driver tears the session down
	}
	free(m_buffer);
}

bool RawCamDevice::Ioctl(DWORD code, void *input, DWORD inputLength, void *output, DWORD outputLength, DWORD *returned)
{
	DWORD bytes = 0;
	if (!DeviceIoControl(m_handle, code, input, inputLength, output, outputLength, &bytes, NULL)) {
		m_lastError = GetLastError();
		if (returned) *returned = 0;
		return false;
	}
	if (returned) *returned = bytes;
	return true;
}

bool RawCamDevice::Connect()
{
	m_handle = CreateFileA(RAWCAM_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (m_handle == INVALID_HANDLE_VALUE) {
		m_lastError = GetLastError();
		return false;
	}
	return true;
}

bool RawCamDevice::Open(unsigned long flags)
{
	RAWCAM_OPEN_INPUT input = { flags };
	RAWCAM_OPEN_OUTPUT output = { 0, 0 };
	if (!Ioctl(IOCTL_RAWCAM_OPEN, &input, sizeof(input), &output, sizeof(output), NULL)) {
		return false;
	}
	m_openedFlags = output.OpenedFlags;
	m_vfeQueue = output.VfeQueueAddress;
	return true;
}

void RawCamDevice::Close()
{
	Ioctl(IOCTL_RAWCAM_CLOSE, NULL, 0, NULL, 0, NULL);
	m_openedFlags = 0;
}

bool RawCamDevice::Command(bool sensor, unsigned long n, const unsigned char *input, unsigned long inputLength,
	unsigned char *response, unsigned long responseSize, unsigned long *responseLength,
	long *status, unsigned long *cameraResult)
{
	if (inputLength > RAWCAM_MAX_CMD_INPUT || responseSize > RAWCAM_MAX_CMD_RESPONSE || m_buffer == NULL) {
		m_lastError = ERROR_INVALID_PARAMETER;
		return false;
	}

	RAWCAM_CMD_INPUT *in = (RAWCAM_CMD_INPUT *)m_buffer;
	in->Command = n;
	in->InputLength = inputLength;
	in->ResponseLength = responseSize;
	if (inputLength) memcpy(in->Input, input, inputLength);

	DWORD returned = 0;
	DWORD inSize = (DWORD)(RAWCAM_CMD_INPUT_HEADER + inputLength);
	DWORD outSize = (DWORD)(RAWCAM_CMD_OUTPUT_HEADER + responseSize);
	if (!Ioctl(sensor ? IOCTL_RAWCAM_SENSOR_CMD : IOCTL_RAWCAM_VFE_CMD, m_buffer, inSize, m_buffer,
		outSize < inSize ? inSize : outSize, &returned)) {
		return false;
	}

	RAWCAM_CMD_OUTPUT *out = (RAWCAM_CMD_OUTPUT *)m_buffer;
	*status = out->Status;
	*cameraResult = out->CameraResult;
	unsigned long length = out->ResponseLength;
	if (length > responseSize) length = responseSize;
	if (length && response) memcpy(response, out->Response, length);
	if (responseLength) *responseLength = length;
	return true;
}

bool RawCamDevice::I2cRead(unsigned char slave, unsigned short reg, unsigned char *data, unsigned long length, long *status)
{
	RAWCAM_I2C request;
	memset(&request, 0, sizeof(request));
	request.Slave = slave;
	request.Register = reg;
	request.Length = length;
	if (!Ioctl(IOCTL_RAWCAM_I2C_READ, &request, sizeof(request), &request, sizeof(request), NULL)) {
		return false;
	}
	*status = request.Status;
	if (request.Status >= 0) memcpy(data, request.Data, length);
	return true;
}

bool RawCamDevice::I2cWrite(unsigned char slave, unsigned short reg, const unsigned char *data, unsigned long length, long *status)
{
	RAWCAM_I2C request;
	memset(&request, 0, sizeof(request));
	if (length > sizeof(request.Data)) {
		m_lastError = ERROR_INVALID_PARAMETER;
		return false;
	}
	request.Slave = slave;
	request.Register = reg;
	request.Length = length;
	memcpy(request.Data, data, length);
	if (!Ioctl(IOCTL_RAWCAM_I2C_WRITE, &request, sizeof(request), &request, sizeof(request), NULL)) {
		return false;
	}
	*status = request.Status;
	return true;
}

bool RawCamDevice::Platform(unsigned long code, unsigned long value0, unsigned long value1)
{
	RAWCAM_PLATFORM_INPUT input = { code, { value0, value1 } };
	return Ioctl(IOCTL_RAWCAM_PLATFORM_CMD, &input, sizeof(input), NULL, 0, NULL);
}

bool RawCamDevice::AllocFrame(unsigned long size, unsigned long fill, RAWCAM_ALLOC_OUTPUT *output)
{
	RAWCAM_ALLOC_INPUT input = { size, fill };
	return Ioctl(IOCTL_RAWCAM_ALLOC_FRAME, &input, sizeof(input), output, sizeof(*output), NULL);
}

bool RawCamDevice::FreeFrame(unsigned long index)
{
	RAWCAM_FREE_INPUT input = { index };
	return Ioctl(IOCTL_RAWCAM_FREE_FRAME, &input, sizeof(input), NULL, 0, NULL);
}

int RawCamDevice::WaitVfeMessage(unsigned long timeoutMs, RAWCAM_VFE_MESSAGE *message)
{
	RAWCAM_WAIT_INPUT input = { timeoutMs };
	DWORD returned = 0;
	// STATUS_TIMEOUT is a success code: the IOCTL succeeds with no data.
	if (!Ioctl(IOCTL_RAWCAM_WAIT_VFE_MSG, &input, sizeof(input), message, sizeof(*message), &returned)) {
		return (m_lastError == ERROR_TIMEOUT || m_lastError == WAIT_TIMEOUT) ? 0 : -1;
	}
	return (returned >= sizeof(*message)) ? 1 : 0;
}

bool RawCamDevice::ReadFrame(unsigned long index, unsigned long offset, unsigned char *buffer, unsigned long length)
{
	const unsigned long chunk = 1024 * 1024;
	while (length) {
		unsigned long n = length < chunk ? length : chunk;
		RAWCAM_READ_INPUT input = { index, offset };
		DWORD returned = 0;
		if (!Ioctl(IOCTL_RAWCAM_READ_FRAME, &input, sizeof(input), buffer, n, &returned) || returned != n) {
			return false;
		}
		buffer += n;
		offset += n;
		length -= n;
	}
	return true;
}
