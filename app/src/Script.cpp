#include "Script.h"

void HexDump(const char *prefix, const unsigned char *data, unsigned long length, unsigned long max)
{
	unsigned long shown = (max && length > max) ? max : length;
	for (unsigned long offset = 0; offset < shown; offset += 16) {
		printf("%s%04lX:", prefix, offset);
		for (unsigned long i = offset; i < offset + 16 && i < shown; i++) {
			printf(" %02X", data[i]);
		}
		printf("\n");
	}
	if (shown < length) {
		printf("%s... (%lu bytes)\n", prefix, length);
	}
}

Script::Script(RawCamDevice &device)
	: m_device(device), m_verbose(false), m_variableCount(0), m_responseLength(0), m_source(""), m_lineNumber(0)
{
	m_payload = (unsigned char *)malloc(MaxPayload);
	m_response = (unsigned char *)malloc(RAWCAM_MAX_CMD_RESPONSE);
}

bool Script::SetVariable(const char *name, unsigned long value)
{
	for (int i = 0; i < m_variableCount; i++) {
		if (_stricmp(m_variables[i].name, name) == 0) {
			m_variables[i].value = value;
			return true;
		}
	}
	if (m_variableCount >= MaxVariables || strlen(name) >= MaxNameLength) {
		printf("error: too many variables or name too long: %s\n", name);
		return false;
	}
	strcpy(m_variables[m_variableCount].name, name);
	m_variables[m_variableCount].value = value;
	m_variableCount++;
	return true;
}

bool Script::GetVariable(const char *name, unsigned long *value) const
{
	for (int i = 0; i < m_variableCount; i++) {
		if (_stricmp(m_variables[i].name, name) == 0) {
			*value = m_variables[i].value;
			return true;
		}
	}
	return false;
}

// Terms joined by + - * /, left to right. A term is a number or $NAME.
bool Script::Evaluate(const char *expression, unsigned long *value)
{
	const char *p = expression;
	unsigned long result = 0;
	char op = '+';

	for (;;) {
		unsigned long term;
		if (*p == '$') {
			char name[MaxNameLength];
			size_t n = 0;
			p++;
			while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < sizeof(name)) {
				name[n++] = *p++;
			}
			name[n] = '\0';
			if (!GetVariable(name, &term)) {
				printf("error: unknown variable $%s\n", name);
				return false;
			}
		} else {
			char *end;
			term = strtoul(p, &end, 0);
			if (end == p) {
				printf("error: bad number in '%s'\n", expression);
				return false;
			}
			p = end;
		}

		switch (op) {
		case '+': result += term; break;
		case '-': result -= term; break;
		case '*': result *= term; break;
		case '/':
			if (term == 0) { printf("error: division by zero in '%s'\n", expression); return false; }
			result /= term;
			break;
		}

		if (*p == '\0') break;
		if (*p != '+' && *p != '-' && *p != '*' && *p != '/') {
			printf("error: bad expression '%s'\n", expression);
			return false;
		}
		op = *p++;
	}

	*value = result;
	return true;
}

static int HexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool Script::ParseBytes(char **tokens, int count, unsigned char *payload, unsigned long *length)
{
	unsigned long n = 0;
	for (int t = 0; t < count; t++) {
		const char *token = tokens[t];
		unsigned long value;
		int size = 0;

		if (strncmp(token, "u8:", 3) == 0) { size = 1; token += 3; }
		else if (strncmp(token, "u16:", 4) == 0) { size = 2; token += 4; }
		else if (strncmp(token, "u32:", 4) == 0) { size = 4; token += 4; }

		if (size) {
			if (!Evaluate(token, &value)) return false;
			if (n + size > MaxPayload) goto TooLong;
			for (int i = 0; i < size; i++) payload[n++] = (unsigned char)(value >> (8 * i));
		} else if (token[0] == 'z' && isdigit((unsigned char)token[1])) {
			value = strtoul(token + 1, NULL, 10);
			if (n + value > MaxPayload) goto TooLong;
			memset(payload + n, 0, value);
			n += value;
		} else {
			size_t len = strlen(token);
			if (len % 2) {
				printf("error: odd number of hex digits in '%s'\n", token);
				return false;
			}
			for (size_t i = 0; i < len; i += 2) {
				int hi = HexValue(token[i]), lo = HexValue(token[i + 1]);
				if (hi < 0 || lo < 0) {
					printf("error: bad hex byte in '%s'\n", token);
					return false;
				}
				if (n + 1 > MaxPayload) goto TooLong;
				payload[n++] = (unsigned char)(hi * 16 + lo);
			}
		}
	}
	*length = n;
	return true;

TooLong:
	printf("error: payload longer than %d bytes\n", (int)MaxPayload);
	return false;
}

void Script::Expand(const char *text, char *out, size_t outSize)
{
	size_t n = 0;
	while (*text && n + 1 < outSize) {
		if (*text == '$') {
			char name[MaxNameLength];
			size_t k = 0;
			const char *p = text + 1;
			while ((isalnum((unsigned char)*p) || *p == '_') && k + 1 < sizeof(name)) name[k++] = *p++;
			name[k] = '\0';
			unsigned long value;
			if (k && GetVariable(name, &value)) {
				n += _snprintf(out + n, outSize - n, "%lu (0x%lX)", value, value);
				if (n >= outSize) n = outSize - 1;
				text = p;
				continue;
			}
		}
		out[n++] = *text++;
	}
	out[n] = '\0';
}

void Script::PrintMessage(const RAWCAM_VFE_MESSAGE &message)
{
	printf("  VFE message id=%lu size=%lu:", message.Id, message.Size);
	const unsigned long *words = (const unsigned long *)message.Payload;
	for (int i = 0; i < 12; i++) printf(" %08lX", words[i]);
	printf("\n");
}

bool Script::DoCommand(bool sensor, char **tokens, int count)
{
	unsigned long n;
	unsigned long responseSize = 0;
	bool check = true;
	int byteTokens = 0;
	char *bytes[MaxTokens];

	if (count < 2 || !Evaluate(tokens[1], &n)) {
		printf("error: %s needs a command number\n", tokens[0]);
		return false;
	}
	for (int i = 2; i < count; i++) {
		if (strncmp(tokens[i], "resp=", 5) == 0) {
			if (!Evaluate(tokens[i] + 5, &responseSize)) return false;
		} else if (strcmp(tokens[i], "nocheck") == 0) {
			check = false;
		} else {
			bytes[byteTokens++] = tokens[i];
		}
	}

	unsigned long inputLength = 0;
	if (!ParseBytes(bytes, byteTokens, m_payload, &inputLength)) return false;

	if (m_verbose) {
		printf("> %s %lu in=%lu resp=%lu\n", sensor ? "sensor" : "vfe", n, inputLength, responseSize);
		HexDump("    in ", m_payload, inputLength, 256);
	}

	long status = 0;
	unsigned long result = 1;
	m_responseLength = 0;
	if (!m_device.Command(sensor, n, m_payload, inputLength, m_response, responseSize, &m_responseLength,
		&status, &result)) {
		printf("error: %s %lu: DeviceIoControl failed, error %lu\n", sensor ? "sensor" : "vfe", n, m_device.LastError());
		return false;
	}

	SetVariable("STATUS", (unsigned long)status);
	SetVariable("RESULT", result);
	SetVariable("RESP_LEN", m_responseLength);

	if (m_verbose || status < 0 || result != 0) {
		printf("  %s %lu: status=0x%08lX result=%lu resp=%lu\n", sensor ? "sensor" : "vfe", n,
			(unsigned long)status, result, m_responseLength);
	}
	if (m_verbose && m_responseLength) {
		HexDump("    out ", m_response, m_responseLength, 256);
	}

	if (check && (status < 0 || result != 0)) {
		printf("error: %s command %lu failed (status=0x%08lX, camera result=%lu)\n",
			sensor ? "sensor" : "vfe", n, (unsigned long)status, result);
		return false;
	}
	return true;
}

/*
 * Waits until a VFE message carries the device address of one of the named
 * buffers (the message ids are not decoded yet, the address is the reliable
 * part). Every message is printed in verbose mode.
 */
bool Script::DoWaitFrame(char **tokens, int count)
{
	unsigned long addresses[8];
	char names[8][MaxNameLength];
	int buffers = 0;
	unsigned long timeout = 5000;
	unsigned long skip = 0;             // frames to let go by before the one kept
	unsigned long requeue = 0;          // VFE enqueue command used to give a skipped buffer back

	for (int i = 1; i < count; i++) {
		if (strncmp(tokens[i], "timeout=", 8) == 0) {
			if (!Evaluate(tokens[i] + 8, &timeout)) return false;
		} else if (strncmp(tokens[i], "skip=", 5) == 0) {
			if (!Evaluate(tokens[i] + 5, &skip)) return false;
		} else if (strncmp(tokens[i], "requeue=", 8) == 0) {
			if (!Evaluate(tokens[i] + 8, &requeue)) return false;
		} else if (buffers < 8) {
			if (!GetVariable(tokens[i], &addresses[buffers])) {
				printf("error: unknown buffer %s\n", tokens[i]);
				return false;
			}
			strncpy(names[buffers], tokens[i], MaxNameLength - 1);
			names[buffers][MaxNameLength - 1] = '\0';
			buffers++;
		}
	}

	ULONGLONG deadline = GetTickCount64() + timeout;
	for (;;) {
		ULONGLONG now = GetTickCount64();
		if (now >= deadline) {
			printf("error: no frame within %lu ms\n", timeout);
			return false;
		}
		RAWCAM_VFE_MESSAGE message;
		memset(&message, 0, sizeof(message));
		int got = m_device.WaitVfeMessage((unsigned long)(deadline - now), &message);
		if (got < 0) {
			printf("error: waiting for a VFE message failed, error %lu\n", m_device.LastError());
			return false;
		}
		if (got == 0) continue;

		if (m_verbose) PrintMessage(message);

		const unsigned long *words = (const unsigned long *)message.Payload;
		for (int w = 0; w < 32; w++) {
			for (int b = 0; b < buffers; b++) {
				if (words[w] == addresses[b]) {
					char name[64];
					unsigned long value = 0;
					if (skip) {
						skip--;
						printf("  frame skipped in %s (message id %lu)\n", names[b], message.Id);
						if (requeue) {
							long status = 0;
							unsigned long result = 1, length = 0;
							unsigned char address[4] = {
								(unsigned char)addresses[b], (unsigned char)(addresses[b] >> 8),
								(unsigned char)(addresses[b] >> 16), (unsigned char)(addresses[b] >> 24) };
							if (!m_device.Command(false, requeue, address, 4, NULL, 0, &length, &status, &result) ||
								status < 0 || result != 0) {
								printf("error: re-enqueue of %s failed (status 0x%08lX result %lu)\n",
									names[b], (unsigned long)status, result);
								return false;
							}
						}
						w = 32;     // next message
						break;
					}
					printf("  frame done in %s (message id %lu)\n", names[b], message.Id);
					SetVariable("FRAME", addresses[b]);
					SetVariable("FRAME_MSG", message.Id);
					_snprintf(name, sizeof(name), "%s_IDX", names[b]); name[sizeof(name) - 1] = '\0';
					GetVariable(name, &value);
					SetVariable("FRAME_IDX", value);
					_snprintf(name, sizeof(name), "%s_SIZE", names[b]); name[sizeof(name) - 1] = '\0';
					value = 0;
					GetVariable(name, &value);
					SetVariable("FRAME_SIZE", value);
					return true;
				}
			}
		}
	}
}

bool Script::DoSave(char **tokens, int count)
{
	char name[64];
	unsigned long index, size;

	if (count < 3) {
		printf("error: save NAME FILE [LEN]\n");
		return false;
	}
	_snprintf(name, sizeof(name), "%s_IDX", tokens[1]); name[sizeof(name) - 1] = '\0';
	if (!GetVariable(name, &index)) { printf("error: unknown buffer %s\n", tokens[1]); return false; }
	_snprintf(name, sizeof(name), "%s_SIZE", tokens[1]); name[sizeof(name) - 1] = '\0';
	GetVariable(name, &size);
	if (count > 3 && !Evaluate(tokens[3], &size)) return false;

	unsigned char *data = (unsigned char *)malloc(size);
	if (data == NULL) { printf("error: out of memory (%lu bytes)\n", size); return false; }
	if (!m_device.ReadFrame(index, 0, data, size)) {
		printf("error: reading frame %s failed, error %lu\n", tokens[1], m_device.LastError());
		free(data);
		return false;
	}

	FILE *file = fopen(tokens[2], "wb");
	if (file == NULL) {
		printf("error: cannot create %s\n", tokens[2]);
		free(data);
		return false;
	}
	size_t written = fwrite(data, 1, size, file);
	fclose(file);

	// Quick sanity check: how much of the buffer still holds the fill pattern.
	unsigned long untouched = 0;
	for (unsigned long i = 0; i < size; i++) if (data[i] == data[size - 1]) untouched++;
	printf("  saved %lu bytes to %s (%lu bytes equal to the last byte 0x%02X)\n",
		(unsigned long)written, tokens[2], untouched, data[size - 1]);
	free(data);
	return written == size;
}

bool Script::RunLine(char *line)
{
	char *tokens[MaxTokens];
	int count = 0;

	// Strip comments.
	for (char *p = line; *p; p++) {
		if (*p == '#') { *p = '\0'; break; }
	}

	// Keep the raw text for echo/fail.
	char raw[512];
	strncpy(raw, line, sizeof(raw) - 1);
	raw[sizeof(raw) - 1] = '\0';

	for (char *p = line; *p && count < MaxTokens; ) {
		while (*p == ' ' || *p == '\t' || *p == '\r') p++;
		if (!*p) break;
		tokens[count++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
		if (*p) *p++ = '\0';
	}
	if (count == 0) return true;

	// Conditional prefix.
	if (tokens[0][0] == '?') {
		unsigned long value = 0;
		if (!GetVariable(tokens[0] + 1, &value)) {
			printf("error: unknown variable %s\n", tokens[0] + 1);
			return false;
		}
		if (!value) return true;
		for (int i = 1; i < count; i++) tokens[i - 1] = tokens[i];
		count--;
		if (count == 0) return true;
		char *rest = strstr(raw, tokens[0]);
		if (rest) memmove(raw, rest, strlen(rest) + 1);
	}

	const char *cmd = tokens[0];

	if (_stricmp(cmd, "set") == 0) {
		unsigned long value;
		if (count != 3 || !Evaluate(tokens[2], &value)) { printf("error: set NAME EXPR\n"); return false; }
		return SetVariable(tokens[1], value);
	}
	if (_stricmp(cmd, "echo") == 0 || _stricmp(cmd, "fail") == 0) {
		char expanded[1024];
		const char *text = raw + strlen(cmd);
		while (*text == ' ' || *text == '\t') text++;
		Expand(text, expanded, sizeof(expanded));
		printf("%s%s\n", _stricmp(cmd, "fail") == 0 ? "error: " : "", expanded);
		return _stricmp(cmd, "echo") == 0;
	}
	if (_stricmp(cmd, "open") == 0) {
		unsigned long flags = 0;
		for (int i = 1; i < count; i++) {
			if (_stricmp(tokens[i], "sensor") == 0) flags |= RAWCAM_OPEN_SENSOR;
			else if (_stricmp(tokens[i], "vfe") == 0) flags |= RAWCAM_OPEN_VFE;
			else if (_stricmp(tokens[i], "platform") == 0) flags |= RAWCAM_OPEN_PLATFORM;
			else if (_stricmp(tokens[i], "smmu") == 0) flags |= RAWCAM_OPEN_SMMU;
			else if (_stricmp(tokens[i], "all") == 0) flags |= RAWCAM_OPEN_ALL;
			else { printf("error: unknown open target %s\n", tokens[i]); return false; }
		}
		if (!m_device.Open(flags)) {
			printf("error: session open failed, error %lu\n", m_device.LastError());
			return false;
		}
		if (m_verbose) printf("  session open, flags 0x%lX, VFE queue 0x%08lX\n", m_device.OpenedFlags(), m_device.VfeQueue());
		return true;
	}
	if (_stricmp(cmd, "close") == 0) {
		m_device.Close();
		return true;
	}
	if (_stricmp(cmd, "sensor") == 0) return DoCommand(true, tokens, count);
	if (_stricmp(cmd, "vfe") == 0) return DoCommand(false, tokens, count);
	if (_stricmp(cmd, "platform") == 0) {
		unsigned long code, v0 = 0, v1 = 0;
		if (count < 2 || !Evaluate(tokens[1], &code)) { printf("error: platform CODE [V0 [V1]]\n"); return false; }
		if (count > 2 && !Evaluate(tokens[2], &v0)) return false;
		if (count > 3 && !Evaluate(tokens[3], &v1)) return false;
		if (!m_device.Platform(code, v0, v1)) {
			printf("error: platform 0x%lX failed, error %lu\n", code, m_device.LastError());
			return false;
		}
		return true;
	}
	if (_stricmp(cmd, "i2cr") == 0 || _stricmp(cmd, "i2cw") == 0) {
		bool write = _stricmp(cmd, "i2cw") == 0;
		unsigned long reg, length = 0, slave = 0;
		char *bytes[MaxTokens];
		int byteTokens = 0;
		unsigned char data[32];
		long status = 0;

		if (count < 3 || !Evaluate(tokens[1], &reg)) { printf("error: i2cr REG LEN | i2cw REG BYTES\n"); return false; }
		for (int i = 2; i < count; i++) {
			if (strncmp(tokens[i], "slave=", 6) == 0) {
				if (!Evaluate(tokens[i] + 6, &slave)) return false;
			} else {
				bytes[byteTokens++] = tokens[i];
			}
		}
		if (write) {
			if (!ParseBytes(bytes, byteTokens, m_payload, &length)) return false;
			if (length == 0 || length > sizeof(data)) { printf("error: 1..32 bytes\n"); return false; }
			if (!m_device.I2cWrite((unsigned char)slave, (unsigned short)reg, m_payload, length, &status)) {
				printf("error: i2c write failed, error %lu\n", m_device.LastError());
				return false;
			}
		} else {
			if (byteTokens != 1 || !Evaluate(bytes[0], &length) || length == 0 || length > sizeof(data)) {
				printf("error: i2cr REG LEN (1..32)\n");
				return false;
			}
			if (!m_device.I2cRead((unsigned char)slave, (unsigned short)reg, data, length, &status)) {
				printf("error: i2c read failed, error %lu\n", m_device.LastError());
				return false;
			}
		}
		if (status < 0) {
			printf("error: i2c %s 0x%04lX failed, status 0x%08lX\n", write ? "write" : "read", reg, (unsigned long)status);
			return false;
		}
		if (!write) {
			unsigned long value = 0;
			char name[8];
			printf("  i2c 0x%04lX:", reg);
			for (unsigned long i = 0; i < length; i++) {
				printf(" %02X", data[i]);
				if (i < 4) {
					_snprintf(name, sizeof(name), "I2C%lu", i);
					SetVariable(name, data[i]);
					value = (value << 8) | data[i];
				}
			}
			printf("\n");
			SetVariable("I2CV", value);
		} else if (m_verbose) {
			printf("  i2c 0x%04lX <-", reg);
			for (unsigned long i = 0; i < length; i++) printf(" %02X", m_payload[i]);
			printf("\n");
		}
		return true;
	}
	if (_stricmp(cmd, "alloc") == 0) {
		unsigned long size, fill = 0xCD;
		RAWCAM_ALLOC_OUTPUT output;
		char name[64];
		if (count < 3 || !Evaluate(tokens[2], &size)) { printf("error: alloc NAME SIZE [fill=B]\n"); return false; }
		if (count > 3 && strncmp(tokens[3], "fill=", 5) == 0 && !Evaluate(tokens[3] + 5, &fill)) return false;
		if (!m_device.AllocFrame(size, fill, &output)) {
			printf("error: frame allocation of %lu bytes failed, error %lu\n", size, m_device.LastError());
			return false;
		}
		SetVariable(tokens[1], output.DeviceAddress);
		_snprintf(name, sizeof(name), "%s_IDX", tokens[1]); name[sizeof(name) - 1] = '\0';
		SetVariable(name, output.Index);
		_snprintf(name, sizeof(name), "%s_SIZE", tokens[1]); name[sizeof(name) - 1] = '\0';
		SetVariable(name, output.Size);
		if (m_verbose) printf("  %s: %lu bytes at device address 0x%08lX\n", tokens[1], output.Size, output.DeviceAddress);
		return true;
	}
	if (_stricmp(cmd, "waitframe") == 0) return DoWaitFrame(tokens, count);
	if (_stricmp(cmd, "wait") == 0) {
		unsigned long timeout = 1000;
		RAWCAM_VFE_MESSAGE message;
		if (count > 1 && !Evaluate(tokens[1], &timeout)) return false;
		int got = m_device.WaitVfeMessage(timeout, &message);
		if (got < 0) { printf("error: wait failed, error %lu\n", m_device.LastError()); return false; }
		if (got == 0) printf("  no VFE message\n");
		else PrintMessage(message);
		return true;
	}
	if (_stricmp(cmd, "sleep") == 0) {
		unsigned long ms;
		if (count != 2 || !Evaluate(tokens[1], &ms)) { printf("error: sleep MS\n"); return false; }
		Sleep(ms);
		return true;
	}
	if (_stricmp(cmd, "getu8") == 0 || _stricmp(cmd, "getu16") == 0 || _stricmp(cmd, "getu32") == 0) {
		unsigned long offset;
		unsigned long size = cmd[4] == '8' ? 1 : (cmd[4] == '1' ? 2 : 4);
		if (count != 3 || !Evaluate(tokens[2], &offset)) { printf("error: %s NAME OFFSET\n", cmd); return false; }
		if (offset + size > m_responseLength) {
			printf("error: offset %lu beyond the last response (%lu bytes)\n", offset, m_responseLength);
			return false;
		}
		unsigned long value = 0;
		for (unsigned long i = 0; i < size; i++) value |= (unsigned long)m_response[offset + i] << (8 * i);
		return SetVariable(tokens[1], value);
	}
	if (_stricmp(cmd, "dumpresp") == 0) {
		if (count != 2) { printf("error: dumpresp FILE\n"); return false; }
		FILE *file = fopen(tokens[1], "wb");
		if (file == NULL) { printf("error: cannot create %s\n", tokens[1]); return false; }
		fwrite(m_response, 1, m_responseLength, file);
		fclose(file);
		return true;
	}
	if (_stricmp(cmd, "save") == 0) return DoSave(tokens, count);

	printf("error: unknown command '%s'\n", cmd);
	return false;
}

bool Script::Run(const char *text, const char *sourceName)
{
	size_t size = strlen(text);
	char *copy = (char *)malloc(size + 1);
	if (copy == NULL || m_payload == NULL || m_response == NULL) {
		printf("error: out of memory\n");
		free(copy);
		return false;
	}
	memcpy(copy, text, size + 1);

	m_source = sourceName;
	m_lineNumber = 0;
	bool ok = true;
	for (char *line = copy; line && *line; ) {
		char *next = strchr(line, '\n');
		if (next) *next++ = '\0';
		m_lineNumber++;
		if (!RunLine(line)) {
			printf("  at %s:%d\n", m_source, m_lineNumber);
			ok = false;
			break;
		}
		line = next;
	}

	free(copy);
	return ok;
}
