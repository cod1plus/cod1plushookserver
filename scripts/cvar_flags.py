#!/usr/bin/env python3
"""cvar_flags.py - list the client's registered cvars WITH their flags, statically.

  engine (CoDMP.exe 1.5): every `push flags; push default; push name; call Cvar_Get` site
                          (Cvar_Get = 0x0043b880, cdecl, args pushed right-to-left)
  cgame  (cgame_mp_x86.dll): the cvarTable_t array {vmCvar*, name*, default*, flags}
                          (16-byte entries, both pointers into the image)

Flags: 0x1 ARCHIVE, 0x2 USERINFO, 0x4 SERVERINFO, 0x8 SYSTEMINFO, 0x10 INIT, 0x20 LATCH,
0x40 ROM, 0x200 CHEAT (engine reverts it to the default unless sv_cheats 1).
    python3 scripts/cvar_flags.py [game dir] -> prints "name flags default source" lines
"""
import os
import re
import struct
import sys

import capstone
import pefile

GAME = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6 - dev"
CVAR_GET_VA = 0x0043b880


def cstr(pe, va):
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    try:
        data = pe.get_data(rva, 96)
    except Exception:
        return None
    end = data.find(b"\0")
    if end <= 0:
        return None
    s = data[:end]
    if not re.fullmatch(rb"[ -~]+", s):
        return None
    return s.decode()


def engine_cvars(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
    code = text.get_data()
    va0 = base + text.VirtualAddress
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    out = {}
    # find call sites first (E8 rel32 to CVAR_GET_VA), then walk back a few pushes
    target = CVAR_GET_VA
    # direct calls (E8 rel32 -> Cvar_Get) and the renderer's indirect `call [ri.Cvar_Get]`
    # (FF 15 imm32; the function-table slot is filled at runtime, so any indirect call whose
    # three preceding pushes look like (flags, "default", "r_name") is taken as Cvar_Get)
    sites = []
    for m in re.finditer(rb"\xe8", code):
        off = m.start()
        rel = struct.unpack_from("<i", code, off + 1)[0]
        if va0 + off + 5 + rel == target:
            sites.append((off, False))
    for m in re.finditer(rb"\xff\x15", code):
        sites.append((m.start(), True))
    for off, indirect in sites:
        # disassemble the 48 bytes before the call and take the last three pushes
        start = max(0, off - 48)
        pushes = []
        for ins in md.disasm(code[start:off], va0 + start):
            if ins.mnemonic == "push":
                pushes.append(ins.op_str)
            elif ins.mnemonic in ("call", "ret", "jmp"):
                pushes = []
        if len(pushes) < 3:
            continue
        flags_s, def_s, name_s = pushes[-3], pushes[-2], pushes[-1]
        try:
            flags = int(flags_s, 16) if flags_s.startswith("0x") else int(flags_s)
            name = cstr(pe, int(name_s, 16))
            default = cstr(pe, int(def_s, 16))
        except ValueError:
            continue
        if not name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            continue
        if indirect and not (name.startswith(("r_", "s_", "cl_", "com_", "in_", "m_", "vid_", "cg_"))
                             and default is not None and flags < 0x10000):
            continue
        if indirect and name.lower() in out:
            continue
        out[name.lower()] = (name, flags, default if default is not None else "?",
                             "engine-ri" if indirect else "engine")
    return out


def cgame_cvars(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    img_lo, img_hi = base, base + pe.OPTIONAL_HEADER.SizeOfImage
    out = {}
    for s in pe.sections:
        if not s.Name.startswith((b".data", b".rdata")):
            continue
        data = s.get_data()
        for off in range(0, len(data) - 16, 4):
            vm, nm, df, fl = struct.unpack_from("<IIII", data, off)
            if not (img_lo <= nm < img_hi and img_lo <= df < img_hi and (vm == 0 or img_lo <= vm < img_hi)):
                continue
            if fl > 0xFFFF:
                continue
            name = cstr(pe, nm)
            if not name or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{1,40}", name):
                continue
            default = cstr(pe, df)
            if default is None:
                continue
            out.setdefault(name.lower(), (name, fl, default, "cgame"))
    return out


def main():
    eng = engine_cvars(os.path.join(GAME, "CoDMP.exe"))
    cg = cgame_cvars(os.path.join(GAME, "Main", "cgame_mp_x86.dll"))
    allc = dict(eng)
    for k, v in cg.items():
        allc.setdefault(k, v)
    print(f"# engine {len(eng)} cvars, cgame {len(cg)} cvars", file=sys.stderr)
    for k in sorted(allc):
        name, fl, df, src = allc[k]
        print(f"{name}\t0x{fl:x}\t{df}\t{src}")


if __name__ == "__main__":
    main()
