#!/usr/bin/env python3
"""pb_to_competitive.py - turn the old PunkBuster cvar rules (pbsv.cfg / pbsvuser.cfg) into
competitive.cfg candidates, and say what cannot be expressed.

    python3 scripts/pb_to_competitive.py <pb dir> [--game <CoD1 install>] [--current competitive.cfg]

PB rule grammar:   pb_sv_cvar "<cvar>" IN <min> <max>      value must be inside  -> "cvar min max"
                                                              (min == max)        -> "cvar value"
                   pb_sv_cvar "<cvar>" OUT <min> <max>     value must be outside -> NOT expressible
                   pb_sv_cvar "<cvar>" INCLUDE <v> ...     one of                -> "cvar v" if single
                   pb_sv_cvar "<cvar>" EXCLUDE <v> ...     none of               -> NOT expressible
competitive.cfg (competitive_sv.c): "cvar value" = forced + write-locked on the client,
"cvar min max" = clamped, INTEGER bounds only (atoi); whole spec <= 4 chunks x 220 chars.

A rule is worth pushing only if the cvar exists in the CLIENT (CoDMP.exe, cgame_mp_x86.dll,
ui_mp_x86.dll string tables) - PB's list is full of AI / single-player / server-only cvars.
"""
import argparse
import os
import re
import sys
from collections import OrderedDict

RULE = re.compile(r'^\s*pb_sv_cvar\s+"?([A-Za-z0-9_]+)"?\s+(IN|OUT|INCLUDE|EXCLUDE)\s+(.*?)\s*$', re.I)
SERVER_PREFIXES = ("sv_", "g_", "ai_", "scr_", "bg_", "net_", "dedicated", "rcon", "pb_", "fs_", "logfile",
                   "sv", "developer", "anim", "physics_", "vehicle", "cheat", "actor", "hud_", "player_")


def parse_pb(pb_dir):
    rules = OrderedDict()
    for fn in sorted(os.listdir(pb_dir)):
        if not fn.lower().startswith(("pbsv", "pbsvuser")) or not fn.lower().endswith((".cfg", ".bak")):
            continue
        p = os.path.join(pb_dir, fn)
        if os.path.islink(p) and not os.path.exists(p):
            continue
        for line in open(p, encoding="latin-1", errors="replace"):
            m = RULE.match(line.rstrip("\r\n"))
            if not m:
                continue
            name, mode, rest = m.group(1), m.group(2).upper(), m.group(3).split()
            rules.setdefault(name.lower(), []).append((name, mode, rest, fn))
    return rules


def client_strings(game_dir):
    """Every ASCII token that looks like a cvar name in the three client binaries."""
    names = set()
    for rel in ("CoDMP.exe", os.path.join("Main", "cgame_mp_x86.dll"), os.path.join("Main", "ui_mp_x86.dll")):
        p = os.path.join(game_dir, rel)
        if not os.path.exists(p):
            print(f"!! missing {p}", file=sys.stderr)
            continue
        data = open(p, "rb").read()
        for m in re.finditer(rb"[A-Za-z_][A-Za-z0-9_]{2,40}", data):
            names.add(m.group(0).decode().lower())
    return names


def is_int(s):
    return re.fullmatch(r"-?\d+", s) is not None


def to_rule(name, mode, vals):
    """-> (rule line or None, why)"""
    if mode == "IN" and len(vals) == 2:
        lo, hi = vals
        if lo == hi:
            return f"{name} {lo}", "forced"
        if is_int(lo) and is_int(hi):
            return f"{name} {lo} {hi}", "range"
        return None, f"range with non-integer bounds ({lo} {hi}): the parser clamps integers only"
    if mode == "INCLUDE" and len(vals) == 1:
        return f"{name} {vals[0]}", "forced (INCLUDE single)"
    if mode == "INCLUDE":
        return None, f"INCLUDE {' '.join(vals)}: a set of allowed values cannot be expressed"
    if mode == "OUT":
        return None, f"OUT {' '.join(vals)}: 'outside a range' cannot be expressed"
    if mode == "EXCLUDE":
        return None, f"EXCLUDE {' '.join(vals)}: a forbidden set cannot be expressed"
    return None, f"unparsed {mode} {' '.join(vals)}"


def read_current(path):
    cur = OrderedDict()
    if not path or not os.path.exists(path):
        return cur
    for line in open(path, encoding="latin-1"):
        line = line.split("#", 1)[0].split(";", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        cur[parts[0].lower()] = " ".join(parts)
    return cur


def spec_len(rules):
    # competitive_sv.c spec: "name=v" or "name=lo:hi" joined by spaces
    out = []
    for r in rules:
        p = r.split()
        out.append(f"{p[0]}={p[1]}" if len(p) == 2 else f"{p[0]}={p[1]}:{p[2]}")
    return len(" ".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pb_dir")
    ap.add_argument("--game", default=r"C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6 - dev")
    ap.add_argument("--current", default=os.path.join(os.path.dirname(__file__), "..", "competitive.cfg"))
    ap.add_argument("--out", default=None, help="write the candidate list here")
    a = ap.parse_args()

    rules = parse_pb(a.pb_dir)
    client = client_strings(a.game)
    current = read_current(a.current)
    print(f"PB rules: {sum(len(v) for v in rules.values())} lines, {len(rules)} distinct cvars; "
          f"client string table: {len(client)} tokens; current competitive.cfg: {len(current)} rules")

    kept, dropped_server, dropped_unknown, not_expr, conflicts, same = [], [], [], [], [], []
    for key, lst in rules.items():
        name, mode, vals, src = lst[0]
        if key.startswith(SERVER_PREFIXES) or key in ("sv_cheats",):
            dropped_server.append(key)
            continue
        if key not in client:
            dropped_unknown.append(key)
            continue
        rule, why = to_rule(name, mode, vals)
        if rule is None:
            not_expr.append((name, mode, " ".join(vals), why))
            continue
        if key in current:
            if current[key].lower() == rule.lower():
                same.append(rule)
            else:
                conflicts.append((current[key], f"PB: {rule}"))
            continue
        kept.append((rule, why))

    print(f"\n== dropped: {len(dropped_server)} server/AI-side cvars, {len(dropped_unknown)} not in the client binaries")
    print(f"== already identical in competitive.cfg: {len(same)}")
    print(f"\n== CONFLICTS (current competitive.cfg vs PB) - keep the current unless PB is stricter for a reason:")
    for cur, pb in conflicts:
        print(f"   {cur:32s} | {pb}")
    print(f"\n== NOT EXPRESSIBLE in competitive.cfg ({len(not_expr)}):")
    for name, mode, vals, why in not_expr:
        print(f"   {name} {mode} {vals}  -> {why}")
    print(f"\n== CANDIDATES to add ({len(kept)}), client cvar, expressible:")
    forced = [r for r, w in kept if w.startswith("forced")]
    ranges = [r for r, w in kept if w == "range"]
    for r, w in kept:
        print(f"   {r:32s}  ({w})")
    cur_len = spec_len(list(current.values()))
    add_len = spec_len([r for r, _ in kept])
    print(f"\n== spec size: current {cur_len} chars, +candidates {add_len} -> {cur_len + add_len + 1} "
          f"(limit 4 x 220 = 880 chars on both sides today)")
    if a.out:
        with open(a.out, "w", encoding="latin-1") as f:
            f.write("# candidates derived from PunkBuster rules (client cvars only, expressible ones)\n")
            for r, w in kept:
                f.write(f"{r}\n")
        print("wrote", a.out)


if __name__ == "__main__":
    main()
