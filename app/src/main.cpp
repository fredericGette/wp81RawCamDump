/*
 * wp81RawCamDump - raw Bayer capture from the Lumia 520 rear sensor through
 * wp81rawcam.sys, without the manufacturing mode and without the VFE pixel
 * pipeline (the frame goes CSI -> ISPIF -> RDI0 -> memory, untouched).
 *
 * The capture is a script (see Script.h). The built-in recipes are generated
 * from the command line options; "--dump-script" prints them so they can be
 * edited and run with "run".
 */
#include "Script.h"

#define DEFAULT_OUTPUT "C:\\Data\\Users\\Public\\Documents\\rawcam.raw"

struct Options {
	const char *output;
	unsigned long exposureUs;
	unsigned long iso;
	long lens;                  // -1 = don't move
	unsigned long drop;
	unsigned long width;        // ConfigResolution request
	unsigned long height;
	unsigned long rdiWidth;     // sensor output, as received on RDI0
	unsigned long rdiHeight;
	unsigned long rdiFormat;
	unsigned long rawSize;      // 0 = 8, 1 = 10, 2 = 12 bits
	unsigned long cid;
	unsigned long rdiRateKbps;
	unsigned long pixelPattern;
	unsigned long testPattern;
	bool noSensorCorrection;   // default true: SMIA on-sensor corrections off
	bool dumpScript;
	bool verbose;
};

static void Usage()
{
	printf(
		"wp81RawCamDump - raw Bayer dump of the Lumia 520 rear camera (needs wp81rawcam.sys)\n"
		"\n"
		"usage:\n"
		"  wp81RawCamDump capture [options]   capture one raw frame\n"
		"  wp81RawCamDump info [-v]           sensor identity, lens, modes, correction registers\n"
		"  wp81RawCamDump run FILE [-D NAME=VALUE]... [-v]   run a script\n"
		"  wp81RawCamDump cmd sensor|vfe N [BYTES...] [resp=N]   one command (sensor powered on/off around it)\n"
		"  wp81RawCamDump i2c r REG [LEN] | w REG BYTES...       sensor register access\n"
		"\n"
		"capture options:\n"
		"  -o FILE      output (default " DEFAULT_OUTPUT "), sidecar FILE.txt\n"
		"  -e US        exposure time in microseconds (default 66666, max 6428220: capped by the sensor driver)\n"
		"  -i ISO       ISO, 100 = analog gain 1x (default 100)\n"
		"  -f POS       lens position (default: not moved)\n"
		"  -d N         frames dropped before the captured one (default 2)\n"
		"  -t N         sensor test pattern (SMIA 0x0600: 1 solid, 2 colour bars, 3 fade, 4 PN9)\n"
		"  --sensor-correction  keep the SMIA on-sensor corrections (black level, defects, shading);\n"
		"               by default they are switched off (registers 0x0B00, 0x0B04, 0x0B05, 0x0B06)\n"
		"  --size WxH   resolution requested to the sensor (default 2592x1944)\n"
		"  --rdi WxH    frame size received on RDI0 (default: SMIA output size 0x034C after the mode change)\n"
		"  --format N   RDI output format (default 24 = CSI passthrough)\n"
		"  --cid N      ISPIF CID of the pixel data (default 0)\n"
		"  --rate KBPS  RDI interface data rate (default 1670400)\n"
		"  --dump-script   print the generated script instead of running it\n"
		"  -v           verbose (every command, response and VFE message)\n");
}

static bool ParseSize(const char *text, unsigned long *w, unsigned long *h)
{
	char *end;
	*w = strtoul(text, &end, 10);
	if (*end != 'x' && *end != 'X') return false;
	*h = strtoul(end + 1, &end, 10);
	return *end == '\0' && *w && *h;
}

// Appends a formatted line to a growing script buffer.
struct Text {
	char *data;
	size_t length;
	size_t capacity;

	Text() : data(NULL), length(0), capacity(0) {}
	~Text() { free(data); }

	void Add(const char *format, ...)
	{
		char line[1024];
		va_list arguments;
		va_start(arguments, format);
		int n = _vsnprintf(line, sizeof(line) - 2, format, arguments);
		va_end(arguments);
		if (n < 0) n = (int)sizeof(line) - 2;
		line[n++] = '\n';
		line[n] = '\0';
		if (length + n + 1 > capacity) {
			size_t newCapacity = (capacity + n + 1) * 2;
			char *p = (char *)realloc(data, newCapacity);
			if (p == NULL) return;
			data = p;
			capacity = newCapacity;
		}
		memcpy(data + length, line, n + 1);
		length += n;
	}
};

/*
 * Capture recipe, modelled on the preview sequence of qccamavs8930.sys
 * recorded by wp81rawcamlog (docs/etw_camera_photo.txt), where RDI0 already
 * runs in continuous mode for the embedded data:
 *   CONFIG_RESOLUTION, ISPIF_CONFIG, OPERATION_CONFIG, RDI_CONFIG,
 *   AXI_OUTPUT_CONFIG, RDI0 enqueues, START, START_STREAM, START_CAPTURE_ACK_REQUEST
 * The differences: the image CID (0) is routed to RDI0 instead of the PIX
 * interface, no other output path, no CAMIF / pipeline / statistics config.
 */
static void BuildCaptureScript(const Options &o, Text &s)
{
	unsigned long gainQ8 = o.iso * 256 / 100;
	unsigned long interval = o.exposureUs + 20000 > 83333 ? o.exposureUs + 20000 : 83333;
	unsigned long lines = (unsigned long)((unsigned long long)o.exposureUs * 1000 / 16463);   // 16.463 us per line
	unsigned long timeoutMs = 3000 + (o.drop + 2) * (interval / 1000 + 50);

	if (lines == 0) lines = 1;
	if (lines > 0xFFFF) lines = 0xFFFF;

	s.Add("# wp81RawCamDump capture recipe (generated)");
	s.Add("set W %lu", o.width);
	s.Add("set H %lu", o.height);
	s.Add("set OPMODE 2                # sensor op mode: 1 preview/video, 2 still");
	s.Add("set RDI_AUTO %d            # 1 = RDI frame size read from the sensor output size registers", o.rdiWidth ? 0 : 1);
	s.Add("set RDI_W %lu", o.rdiWidth);
	s.Add("set RDI_H %lu", o.rdiHeight);
	s.Add("set RDI_FORMAT %lu        # 24 = CSI passthrough, as for the embedded data RDI of qccamavs", o.rdiFormat);
	s.Add("set RAW_SIZE %lu          # 0 = 8, 1 = 10, 2 = 12 bits", o.rawSize);
	s.Add("set RDI_RATE %lu    # kbps (qccamavs: 1670400 = 2 lanes x 835.2 Mbps)", o.rdiRateKbps);
	s.Add("set PIXEL_PATTERN %lu       # qccamavs uses 3", o.pixelPattern);
	s.Add("set FRAME_INTERVAL_US %lu", interval);
	s.Add("set EXPOSURE_US %lu", o.exposureUs);
	s.Add("set LINES %lu             # initial line count (16.463 us per line)", lines);
	s.Add("set GAIN_Q8 %lu           # ISO %lu", gainQ8, o.iso);
	s.Add("set HAS_LENS %d", o.lens >= 0 ? 1 : 0);
	s.Add("set LENS %ld", o.lens >= 0 ? o.lens : 0);
	s.Add("set TEST_PATTERN %lu", o.testPattern);
	s.Add("set NO_CORRECTION %d", o.noSensorCorrection ? 1 : 0);
	s.Add("set DROP %lu", o.drop);
	s.Add("");
	s.Add("open all");
	s.Add("sensor 13 resp=8 nocheck                 # CSI state (diagnostic)");
	s.Add("");
	s.Add("# ---- sensor: power, mode, exposure, gain, focus");
	s.Add("sensor 0                                 # POWER_ON");
	s.Add("sensor 68 resp=32                        # GET_SENSOR_IDENTITY");
	s.Add("getu16 MODEL 0");
	s.Add("echo sensor model $MODEL");
	s.Add("# CONFIG_RESOLUTION (40 bytes, layout from the qccamavs trace):");
	s.Add("#   width, height, 1, 1, 1, op mode, gain 8.8, line count, ?, max frame interval (us)");
	s.Add("sensor 45 u32:$W u32:$H u32:1 u32:1 u32:1 u32:$OPMODE u32:$GAIN_Q8 u32:$LINES u32:0 u32:$FRAME_INTERVAL_US");
	s.Add("?NO_CORRECTION i2cw 0x0B00 00              # SMIA shading_correction_enable");
	s.Add("?NO_CORRECTION i2cw 0x0B04 00              # SMIA black_level_correction_enable (1 after power on)");
	s.Add("?NO_CORRECTION i2cw 0x0B05 00              # SMIA mapped_defect_correction_enable (1 after power on)");
	s.Add("?NO_CORRECTION i2cw 0x0B06 00              # SMIA single_defect_correction_enable");
	s.Add("i2cr 0x0B00 8                            # corrections now in effect");
	s.Add("# Frame size: SMIA output size of the selected mode (x_output_size, y_output_size)");
	s.Add("i2cr 0x034C 4");
	s.Add("?RDI_AUTO set RDI_W $I2C0*256+$I2C1");
	s.Add("?RDI_AUTO set RDI_H $I2C2*256+$I2C3");
	s.Add("set RDI_STRIDE %s       # packed row bytes", o.rawSize == 1 ? "$RDI_W*10/8" : (o.rawSize == 2 ? "$RDI_W*12/8" : "$RDI_W"));
	s.Add("set FRAME_BYTES $RDI_STRIDE+7/8*8*$RDI_H+65536   # left to right: row increment x lines + margin");
	s.Add("echo RDI frame $RDI_W x $RDI_H, stride $RDI_STRIDE, buffer $FRAME_BYTES");
	s.Add("?TEST_PATTERN i2cw 0x0600 u8:0 u8:$TEST_PATTERN   # SMIA test_pattern_mode");
	s.Add("# MANUAL_EXPOSURE: +4 exposure (us), +8 analog gain 8.8; applied at once if streaming, else at stream start");
	s.Add("sensor 67 u32:0 u32:$EXPOSURE_US u16:$GAIN_Q8 u16:0 resp=12");
	s.Add("getu32 EXPOSURE_READ 4");
	s.Add("getu16 GAIN_READ 8");
	s.Add("?HAS_LENS sensor 56 u32:$LENS              # SET_LENS_POSITION");
	s.Add("?HAS_LENS sensor 55 resp=4                 # GET_LENS_POSITION");
	s.Add("?HAS_LENS getu32 LENS_READ 0");
	s.Add("");
	s.Add("# ---- VFE: RDI0 only (ISPIF mode 0), no pixel pipeline");
	s.Add("vfe 1                                    # POWER_ON");
	s.Add("vfe 3                                    # RESET");
	s.Add("vfe 98 resp=12                           # GET_HW_VERSION");
	{
		// ISPIF_CONFIG: RDI input CSID 0, PIX input CSID 0, pixCID[16] none, rdiCID[16] = CID, PIX rate 0, RDI0 rate.
		// qccamavs: pixCID[0] = 1 (image), rdiCID[1] = 1 (embedded data).
		char rdiCid[40];
		for (unsigned long i = 0; i < 16; i++) {
			sprintf(rdiCid + i * 2, "%02X", i == o.cid ? 1 : 0);
		}
		s.Add("vfe 11 u32:0 u32:0 z16 %s u32:0 u32:$RDI_RATE   # ISPIF_CONFIG (48 bytes)", rdiCid);
	}
	s.Add("# OPERATION_CONFIG: raw size, path types (out1-3, camif raw, ideal raw, RDI0-2), input CAMIF,");
	s.Add("# pixel pattern, then +0x2C 3, +0x30 0, +0x34 composite stats 0, +0x38 2 (values of qccamavs)");
	s.Add("vfe 7 u32:$RAW_SIZE u32:0 u32:0 u32:0 u32:0 u32:0 u32:1 u32:0 u32:0 u32:0 u32:$PIXEL_PATTERN u32:3 u32:0 u8:0 z3 u32:2");
	s.Add("vfe 42 u8:0 u8:0 u8:0                    # RDI_CONFIG: no frame skip");
	s.Add("# AXI_OUTPUT_CONFIG (260 bytes): burst 2, stats class 1, frame interval; only the RDI0 path at +0xBC:");
	s.Add("#   width, height, buffer height, stride (bytes), format, raw size, frame interval (us)");
	s.Add("vfe 8 u32:2 u32:1 u32:$FRAME_INTERVAL_US z108 u32:0 u16:0 u16:0 u32:0 u32:0 u32:0 z24 z24 u16:$RDI_W u16:$RDI_H u16:$RDI_H u16:$RDI_STRIDE u32:$RDI_FORMAT u32:$RAW_SIZE u32:$FRAME_INTERVAL_US u8:0 z3 z24 z24");
	s.Add("alloc F0 $FRAME_BYTES fill=0xCD");
	s.Add("alloc F1 $FRAME_BYTES fill=0xCD");
	s.Add("alloc F2 $FRAME_BYTES fill=0xCD");
	s.Add("vfe 118 u32:$F0                          # RDI0_BUFFER_ENQUEUE");
	s.Add("vfe 118 u32:$F1");
	s.Add("vfe 118 u32:$F2");
	s.Add("");
	s.Add("# ---- stream (same order as qccamavs)");
	s.Add("vfe 4 u32:0                              # START");
	s.Add("sensor 2                                 # START_STREAM");
	s.Add("vfe 149 nocheck                          # START_CAPTURE_ACK_REQUEST");
	s.Add("# Keep the frame after DROP others (the new exposure takes a few frames); skipped buffers are re-enqueued.");
	s.Add("waitframe F0 F1 F2 skip=$DROP requeue=118 timeout=%lu", timeoutMs);
	s.Add("");
	s.Add("# ---- teardown (the driver also does it if the script fails)");
	s.Add("vfe 109 nocheck                          # STOP");
	s.Add("vfe 3 nocheck                            # RESET");
	s.Add("vfe 136 nocheck                          # RDI0_OUTPUT_FLUSH_BUFFER_QUEUE");
	s.Add("vfe 2                                    # POWER_OFF");
	s.Add("sensor 3 nocheck                         # STOP_STREAM");
	s.Add("sensor 1                                 # POWER_OFF");
	s.Add("save FRAME %s", o.output);
	s.Add("close");
}

static void BuildInfoScript(Text &s)
{
	s.Add("open sensor platform");
	s.Add("sensor 13 resp=8 nocheck        # CSI state");
	s.Add("sensor 0                        # POWER_ON");
	s.Add("echo -- identity (SMIA 0x0000, 0x0002, 0x0010, 0x0003, serial, camera id)");
	s.Add("sensor 68 resp=32");
	s.Add("echo -- lens details (rest, far end, infinity, hyperfocal, macro, near end)");
	s.Add("sensor 57 resp=36 nocheck");
	s.Add("sensor 55 resp=4 nocheck        # lens position");
	s.Add("sensor 59 resp=20 nocheck       # sensor details");
	s.Add("sensor 58 resp=28 nocheck       # sensor settings");
	s.Add("sensor 29 resp=8 nocheck        # current exposure config");
	s.Add("echo -- sensor modes (op mode masks 1, 2, 4, 8) -> modes1.bin ... modes8.bin");
	s.Add("sensor 23 u32:1 resp=27124 nocheck");
	s.Add("dumpresp C:\\Data\\Users\\Public\\Documents\\modes1.bin");
	s.Add("sensor 23 u32:2 resp=27124 nocheck");
	s.Add("dumpresp C:\\Data\\Users\\Public\\Documents\\modes2.bin");
	s.Add("sensor 23 u32:4 resp=27124 nocheck");
	s.Add("dumpresp C:\\Data\\Users\\Public\\Documents\\modes4.bin");
	s.Add("sensor 23 u32:8 resp=27124 nocheck");
	s.Add("dumpresp C:\\Data\\Users\\Public\\Documents\\modes8.bin");
	s.Add("echo -- SMIA registers: model, revision, manufacturer");
	s.Add("i2cr 0x0000 4");
	s.Add("echo -- frame/line length, output size");
	s.Add("i2cr 0x0340 4");
	s.Add("i2cr 0x034C 4");
	s.Add("echo -- correction: 0x0B00 shading, 0x0B05 mapped defect, 0x0B06 single defect");
	s.Add("i2cr 0x0B00 8");
	s.Add("echo -- test pattern mode");
	s.Add("i2cr 0x0600 2");
	s.Add("sensor 1                        # POWER_OFF");
	s.Add("close");
}

static bool WriteSidecar(const Options &o, Script &script)
{
	char path[MAX_PATH];
	unsigned long v;
	_snprintf(path, sizeof(path), "%s.txt", o.output);
	path[sizeof(path) - 1] = '\0';
	FILE *f = fopen(path, "w");
	if (f == NULL) return false;

	// Keys read by tools/raw2png.py: format, width, height, stride, fill, order.
	// format=MIPI (CSI RAW10 passthrough) and order=GBRG verified with the SMIA colour bars (2026-11-25);
	// stride = write master row increment ((packed row + 7) / 8 words, as for the embedded data RDI
	// of qccamavs: 1620 bytes -> 203 words). "raw2png --stride auto" checks it against the data.
	fprintf(f, "source=RDI0\n");
	fprintf(f, "format=%s\n", o.rdiFormat == 24 && o.rawSize == 1 ? "MIPI" : "UNKNOWN");
	unsigned long w = o.rdiWidth, h = o.rdiHeight, stride = 0;
	script.GetVariable("RDI_W", &w);
	script.GetVariable("RDI_H", &h);
	if (script.GetVariable("RDI_STRIDE", &stride)) stride = (stride + 7) / 8 * 8;
	fprintf(f, "width=%lu\nheight=%lu\n", w, h);
	if (stride) fprintf(f, "stride=%lu\n", stride); else fprintf(f, "stride=auto\n");
	fprintf(f, "fill=0xCD\n");
	fprintf(f, "order=GBRG\n");
	// Pedestal ~192 with the on-sensor black level correction off: estimated by raw2png.
	fprintf(f, "black=auto\n");
	fprintf(f, "rdi_format=%lu\n", o.rdiFormat);
	fprintf(f, "raw_size=%lu\n", o.rawSize);
	fprintf(f, "requested=%lux%lu\n", o.width, o.height);
	if (script.GetVariable("FRAME_SIZE", &v)) fprintf(f, "buffer_bytes=%lu\n", v);
	fprintf(f, "exposure_us=%lu\n", o.exposureUs);
	if (script.GetVariable("EXPOSURE_READ", &v)) fprintf(f, "exposure_read_us=%lu\n", v);
	fprintf(f, "iso=%lu\ngain_q8=%lu\n", o.iso, o.iso * 256 / 100);
	if (script.GetVariable("GAIN_READ", &v)) fprintf(f, "gain_read_q8=%lu\n", v);
	if (o.lens >= 0) fprintf(f, "lens=%ld\n", o.lens);
	if (script.GetVariable("LENS_READ", &v)) fprintf(f, "lens_read=%lu\n", v);
	if (script.GetVariable("MODEL", &v)) fprintf(f, "sensor_model=0x%04lX\n", v);
	fprintf(f, "dropped_frames=%lu\n", o.drop);
	fprintf(f, "test_pattern=%lu\n", o.testPattern);
	fprintf(f, "sensor_correction=%s\n", o.noSensorCorrection ? "disabled (0x0B00 0x0B04 0x0B05 0x0B06 = 0)" : "sensor default");
	if (script.GetVariable("FRAME_MSG", &v)) fprintf(f, "vfe_message_id=%lu\n", v);
	fclose(f);
	printf("sidecar %s\n", path);
	return true;
}

static char *ReadTextFile(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = (char *)malloc(size + 1);
	if (text) {
		size_t n = fread(text, 1, size, f);
		text[n] = '\0';
	}
	fclose(f);
	return text;
}

static bool Connect(RawCamDevice &device)
{
	if (!device.Connect()) {
		printf("error: cannot open %s (error %lu). Is wp81rawcam.sys started (sc start wp81rawcam)?\n",
			RAWCAM_USER_PATH, device.LastError());
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	// Unbuffered: when the output goes to a pipe (ssh) and the phone crashes, every line printed
	// before the crash must already have left the process.
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc < 2) {
		Usage();
		return 1;
	}

	Options o;
	memset(&o, 0, sizeof(o));
	o.output = DEFAULT_OUTPUT;
	o.exposureUs = 66666;
	o.iso = 100;
	o.lens = -1;
	o.drop = 2;
	o.width = 2592;
	o.height = 1944;
	o.rdiWidth = 0;            // 0 = read from the sensor (0x034C)
	o.rdiHeight = 0;
	o.noSensorCorrection = true;
	o.rdiFormat = 24;
	o.rawSize = 1;
	o.cid = 0;
	o.rdiRateKbps = 1670400;
	o.pixelPattern = 3;

	const char *mode = argv[1];
	RawCamDevice device;
	Script script(device);

	if (_stricmp(mode, "capture") == 0) {
		for (int i = 2; i < argc; i++) {
			const char *a = argv[i];
			const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
			bool hasValue = next != NULL;
			if (strcmp(a, "-v") == 0) o.verbose = true;
			else if (strcmp(a, "--dump-script") == 0) o.dumpScript = true;
			else if (strcmp(a, "--no-sensor-correction") == 0) o.noSensorCorrection = true;
			else if (strcmp(a, "--sensor-correction") == 0) o.noSensorCorrection = false;
			else if (strcmp(a, "-o") == 0 && hasValue) { o.output = next; i++; }
			else if (strcmp(a, "-e") == 0 && hasValue) { o.exposureUs = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "-i") == 0 && hasValue) { o.iso = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "-f") == 0 && hasValue) { o.lens = strtol(next, NULL, 0); i++; }
			else if (strcmp(a, "-d") == 0 && hasValue) { o.drop = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "-t") == 0 && hasValue) { o.testPattern = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "--format") == 0 && hasValue) { o.rdiFormat = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "--cid") == 0 && hasValue) { o.cid = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "--rate") == 0 && hasValue) { o.rdiRateKbps = strtoul(next, NULL, 0); i++; }
			else if (strcmp(a, "--size") == 0 && hasValue && ParseSize(next, &o.width, &o.height)) i++;
			else if (strcmp(a, "--rdi") == 0 && hasValue && ParseSize(next, &o.rdiWidth, &o.rdiHeight)) i++;
			else { printf("error: bad option %s\n", a); Usage(); return 1; }
		}
		if (o.iso < 100 || o.iso > 1600 || o.exposureUs == 0 || o.cid > 15) {
			printf("error: ISO must be 100..1600, exposure > 0, CID 0..15\n");
			return 1;
		}

		Text text;
		BuildCaptureScript(o, text);
		if (o.dumpScript) {
			fputs(text.data, stdout);
			return 0;
		}
		if (!Connect(device)) return 1;
		script.SetVerbose(o.verbose);
		bool ok = script.Run(text.data, "capture");
		if (!ok) {
			printf("capture failed\n");
			return 2;
		}
		WriteSidecar(o, script);
		{
			// qccamsensor8930.sys caps the exposure silently (0xFFFF lines, 6428220 us measured).
			unsigned long readUs = 0;
			if (script.GetVariable("EXPOSURE_READ", &readUs) && readUs + readUs / 100 < o.exposureUs) {
				printf("warning: exposure capped by the sensor driver: %lu us asked, %lu us applied\n",
					o.exposureUs, readUs);
			}
		}
		return 0;
	}

	if (_stricmp(mode, "info") == 0) {
		Text text;
		BuildInfoScript(text);
		if (!Connect(device)) return 1;
		script.SetVerbose(true);
		return script.Run(text.data, "info") ? 0 : 2;
	}

	if (_stricmp(mode, "run") == 0) {
		if (argc < 3) { Usage(); return 1; }
		char *text = ReadTextFile(argv[2]);
		if (text == NULL) { printf("error: cannot read %s\n", argv[2]); return 1; }
		for (int i = 3; i < argc; i++) {
			if (strcmp(argv[i], "-v") == 0) {
				script.SetVerbose(true);
			} else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
				char name[64];
				const char *eq = strchr(argv[i + 1], '=');
				if (eq == NULL || eq - argv[i + 1] >= (int)sizeof(name)) { printf("error: -D NAME=VALUE\n"); return 1; }
				memcpy(name, argv[i + 1], eq - argv[i + 1]);
				name[eq - argv[i + 1]] = '\0';
				script.SetVariable(name, strtoul(eq + 1, NULL, 0));
				i++;
			} else {
				printf("error: bad option %s\n", argv[i]);
				return 1;
			}
		}
		if (!Connect(device)) { free(text); return 1; }
		bool ok = script.Run(text, argv[2]);
		free(text);
		return ok ? 0 : 2;
	}

	if (_stricmp(mode, "cmd") == 0) {
		if (argc < 4) { Usage(); return 1; }
		bool sensor = _stricmp(argv[2], "sensor") == 0;
		if (!sensor && _stricmp(argv[2], "vfe") != 0) { Usage(); return 1; }
		Text text;
		text.Add("open sensor vfe platform");
		text.Add("sensor 0");
		char line[2048];
		int n = _snprintf(line, sizeof(line), "%s", argv[2]);
		for (int i = 3; i < argc && n < (int)sizeof(line) - 64; i++) {
			n += _snprintf(line + n, sizeof(line) - n, " %s", argv[i]);
		}
		line[sizeof(line) - 1] = '\0';
		text.Add("%s nocheck", line);
		text.Add("sensor 1");
		text.Add("close");
		if (!Connect(device)) return 1;
		script.SetVerbose(true);
		return script.Run(text.data, "cmd") ? 0 : 2;
	}

	if (_stricmp(mode, "i2c") == 0) {
		if (argc < 4) { Usage(); return 1; }
		Text text;
		text.Add("open sensor platform");
		text.Add("sensor 0");
		char line[1024];
		int n;
		if (_stricmp(argv[2], "r") == 0) {
			n = _snprintf(line, sizeof(line), "i2cr %s %s", argv[3], argc > 4 ? argv[4] : "1");
		} else if (_stricmp(argv[2], "w") == 0 && argc > 4) {
			n = _snprintf(line, sizeof(line), "i2cw %s", argv[3]);
			for (int i = 4; i < argc && n < (int)sizeof(line) - 64; i++) {
				n += _snprintf(line + n, sizeof(line) - n, " %s", argv[i]);
			}
		} else {
			Usage();
			return 1;
		}
		line[sizeof(line) - 1] = '\0';
		text.Add("%s", line);
		text.Add("sensor 1");
		text.Add("close");
		if (!Connect(device)) return 1;
		script.SetVerbose(true);
		return script.Run(text.data, "i2c") ? 0 : 2;
	}

	Usage();
	return 1;
}
