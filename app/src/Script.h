#pragma once

#include "RawCamDevice.h"

/*
 * Tiny command language driving wp81rawcam.sys, so the capture sequence can
 * be changed on the phone without rebuilding (see README "Script language").
 *
 *   # comment                         ?VAR <line>   runs <line> only if VAR != 0
 *   set NAME EXPR                     echo TEXT ($VAR expanded)
 *   open [sensor] [vfe] [platform] [smmu] | open all          close
 *   sensor N [BYTES...] [resp=N] [nocheck]
 *   vfe N [BYTES...] [resp=N] [nocheck]
 *   platform CODE [V0 [V1]]
 *   i2cr REG LEN [slave=S]            -> I2C0..I2C3, I2CV (big endian value)
 *   i2cw REG BYTES... [slave=S]
 *   alloc NAME SIZE [fill=B]          -> NAME (device address), NAME_IDX, NAME_SIZE
 *   waitframe NAME... timeout=MS      -> waits for a VFE message naming one of the buffers;
 *                                        sets FRAME (address), FRAME_MSG (message id),
 *                                        FRAME_IDX and FRAME_SIZE ("save FRAME file" works)
 *   wait MS                           prints the next VFE message
 *   sleep MS
 *   getu8|getu16|getu32 NAME OFFSET   from the last command response
 *   dumpresp FILE                     writes the last response
 *   save NAME FILE [LEN]              reads frame buffer NAME into FILE
 *   fail TEXT                         stops the script
 *
 * BYTES: "0a0b0c" raw hex bytes, u8:EXPR u16:EXPR u32:EXPR little endian,
 * zN = N zero bytes. EXPR: numbers (dec/0x hex) and $VARs joined by + - * /
 * evaluated left to right, e.g. $W*$H*2+65536.
 */
class Script {
public:
	explicit Script(RawCamDevice &device);

	void SetVerbose(bool verbose) { m_verbose = verbose; }
	bool SetVariable(const char *name, unsigned long value);
	bool GetVariable(const char *name, unsigned long *value) const;

	// Runs a whole script (lines separated by '\n'). Returns false at the first failing line.
	bool Run(const char *text, const char *sourceName);

private:
	enum { MaxVariables = 96, MaxNameLength = 32, MaxTokens = 384, MaxPayload = RAWCAM_MAX_CMD_INPUT };

	struct Variable {
		char name[MaxNameLength];
		unsigned long value;
	};

	bool RunLine(char *line);
	bool Evaluate(const char *expression, unsigned long *value);
	bool ParseBytes(char **tokens, int count, unsigned char *payload, unsigned long *length);
	bool DoCommand(bool sensor, char **tokens, int count);
	bool DoWaitFrame(char **tokens, int count);
	bool DoSave(char **tokens, int count);
	void Expand(const char *text, char *out, size_t outSize);
	void PrintMessage(const RAWCAM_VFE_MESSAGE &message);

	RawCamDevice &m_device;
	bool m_verbose;
	Variable m_variables[MaxVariables];
	int m_variableCount;
	unsigned char *m_payload;
	unsigned char *m_response;
	unsigned long m_responseLength;
	const char *m_source;
	int m_lineNumber;
};

void HexDump(const char *prefix, const unsigned char *data, unsigned long length, unsigned long max);
