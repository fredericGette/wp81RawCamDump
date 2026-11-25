#!/usr/bin/env python3
# Independent verifier for the hand-assembled 0xF90F replacement routines.
# Decodes the byte string (loaded at 0xF90F), checks instruction lengths,
# recomputes every relative/absolute branch target from the bytes, and emits
# the <=32-byte i2cw upload chunks.

BASE = 0xF90F

# opcode -> (length, mnemonic template). Only the opcodes used by the routines.
OPC = {
    0x90: (3, "MOV DPTR,#{imm16}"),
    0x74: (2, "MOV A,#{d8}"),
    0x75: (3, "MOV {dir},#{d8b}"),
    0xE0: (1, "MOVX A,@DPTR"),
    0xF0: (1, "MOVX @DPTR,A"),
    0x93: (1, "MOVC A,@A+DPTR"),
    0xE4: (1, "CLR A"),
    0xE5: (2, "MOV A,{dir}"),
    0xF5: (2, "MOV {dir},A"),
    0x78: (2, "MOV R0,#{d8}"),
    0x7C: (2, "MOV R4,#{d8}"),
    0x7D: (2, "MOV R5,#{d8}"),
    0xE8: (1, "MOV A,R0"),
    0xEB: (1, "MOV A,R3"),
    0xFB: (1, "MOV R3,A"),
    0xFE: (1, "MOV R6,A"),
    0xFF: (1, "MOV R7,A"),
    0x8C: (2, "MOV {dir},R4"),
    0x8D: (2, "MOV {dir},R5"),
    0x8E: (2, "MOV {dir},R6"),
    0x8F: (2, "MOV {dir},R7"),
    0x08: (1, "INC R0"),
    0x0C: (1, "INC R4"),
    0x0D: (1, "INC R5"),
    0x76: (2, "MOV @R0,#{d8}"),
    0xF6: (1, "MOV @R0,A"),
    0x60: (2, "JZ {rel}"),
    0x80: (2, "SJMP {rel}"),
    0xBD: (3, "CJNE R5,#{d8b},{rel}"),
    0xB8: (3, "CJNE R0,#{d8b},{rel}"),
    0x02: (3, "LJMP {abs16}"),
}

SFR = {0x82: "DPL", 0x83: "DPH"}

def decode(name, hexstr, expect_end, chunk_at=None):
    b = bytes.fromhex(hexstr.replace(" ", ""))
    print(f"== {name}: {len(b)} bytes, 0x{BASE:04X}..0x{BASE+len(b)-1:04X} ==")
    pc = BASE
    i = 0
    ok = True
    targets = []
    starts = []
    while i < len(b):
        op = b[i]
        if op not in OPC:
            print(f"  0x{pc:04X}: {op:02X}  *** UNKNOWN OPCODE ***")
            ok = False
            break
        ln, tmpl = OPC[op]
        raw = b[i:i+ln]
        starts.append(pc)
        text = tmpl
        if "{imm16}" in text:
            text = text.replace("{imm16}", f"0x{(raw[1]<<8)|raw[2]:04X}")
        if "{abs16}" in text:
            tgt = (raw[1]<<8)|raw[2]
            text = text.replace("{abs16}", f"0x{tgt:04X}")
            targets.append(("LJMP", pc, tgt))
        if "{dir}" in text:
            text = text.replace("{dir}", SFR.get(raw[1], f"0x{raw[1]:02X}"))
        if "{d8b}" in text:          # immediate is byte 2 (after a direct/opcode)
            text = text.replace("{d8b}", f"0x{raw[2]:02X}") if ln == 3 and op == 0x75 else text.replace("{d8b}", f"0x{raw[1]:02X}")
        if "{d8}" in text:
            text = text.replace("{d8}", f"0x{raw[1]:02X}")
        if "{rel}" in text:
            rel = raw[-1]
            srel = rel - 256 if rel >= 0x80 else rel
            tgt = pc + ln + srel
            text = text.replace("{rel}", f"0x{tgt:04X} (rel {srel:+d})")
            targets.append((tmpl.split()[0], pc, tgt))
        print(f"  0x{pc:04X}: {raw.hex(' ').upper():<11} {text}")
        pc += ln
        i += ln
    end = BASE + len(b)
    print(f"  -> ends at 0x{end:04X} (next free). expected 0x{expect_end:04X}: "
          + ("OK" if end == expect_end else "*** MISMATCH ***"))
    if end != expect_end:
        ok = False
    startset = set(starts)
    for kind, src, tgt in targets:
        if kind == "LJMP":
            print(f"  branch {kind} @0x{src:04X} -> 0x{tgt:04X} (ROM, external)")
            continue
        good = (BASE <= tgt < end) and (tgt in startset)
        print(f"  branch {kind} @0x{src:04X} -> 0x{tgt:04X}  boundary={tgt in startset}  "
              + ("OK" if good else "*** BAD TARGET ***"))
        if not good:
            ok = False
    # Emit i2cw chunks (<=32 bytes).
    print("  i2cw chunks:")
    for off in range(0, len(b), 32):
        chunk = b[off:off+32]
        addr = BASE + off
        print(f"    i2cw 0x{addr:04X}  " + " ".join(f"{x:02X}" for x in chunk))
    if chunk_at is not None:
        print(f"  NOTE: patch the source address at 0x{chunk_at:04X} (hi) / 0x{chunk_at+1:04X} (lo) "
              f"each chunk, e.g. i2cw 0x{chunk_at:04X} <hi> <lo>")
    print("  RESULT:", "PASS" if ok else "FAIL")
    print()
    return ok

# PROBE: unchanged (marker write, no MOVC).
probe = (
    "90 35 F7 74 A5 F0 90 36 14 74 5A F0 90 30 47 74 98 F0 90 30 36 74 1E F0 "
    "90 30 42 74 24 F0 E5 53 60 06 78 2B 76 01 80 04 78 2B E4 F6 90 30 88 02 1D 4F"
)

# DUMPER v2: source address is the immediate of MOV DPTR,#SRC at 0xF916 (90 hi lo),
# rewritten by the host before each stream start. This avoids the broken
# I2C-write -> 8051-read path for 0x35F5 (the v1 bug: MOVX read of 0x35F5 = 0).
dumper = (
    "7C 35 7D F7 78 00 E8 90 00 00 93 FB 8D 82 8C 83 "
    "EB F0 0D BD 00 01 0C 08 B8 1E EB 90 30 47 74 98 "
    "F0 90 30 36 74 1E F0 90 30 42 74 24 F0 E5 53 60 "
    "06 78 2B 76 01 80 04 78 2B E4 F6 90 30 88 02 1D 4F"
)

a = decode("PROBE", probe, 0xF941)
# SRC operand: opcode 90 at 0xF916 -> hi at 0xF917, lo at 0xF918.
b = decode("DUMPER", dumper, 0xF950, chunk_at=0xF917)
print("ALL:", "PASS" if (a and b) else "FAIL")
