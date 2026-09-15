#!/usr/bin/env python3
"""
Flash WiCyS CYD ducks by serial number.

  ./flash_duck.py --list              # answer key
  ./flash_duck.py 3                   # flash Duck-3
  ./flash_duck.py 1 2 3               # batch: prompt between boards
  ./flash_duck.py --batch 1-8         # same, serials 1 through 8
  ./flash_duck.py 4 -p /dev/cu.usbserial-2120
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(ROOT, "CYD")
IDENTITY = os.path.join(SKETCH, "duck_identity.h")

FQBN = (
    "esp32:esp32:esp32:"
    "PartitionScheme=huge_app,FlashFreq=80,FlashMode=qio,"
    "FlashSize=4M,CPUFreq=240,UploadSpeed=115200"
)

# Keep in sync with GAMES_BY_DUCK in CYD/CYD.ino
DUCKS = [
    {
        "n": 1,
        "g1": ("QUACK", "POND", "UE9ORA==", "HOME"),
        "g1f": ("WiCyS{d01_g1_quack}", "WiCyS{d01_g1_pond}", "WiCyS{d01_g1_home}"),
        "g2": ("WADDLE", "BREAD", "QlJFQUQ=", "NEST"),
        "g2f": ("WiCyS{d01_g2_waddle}", "WiCyS{d01_g2_bread}", "WiCyS{d01_g2_nest}"),
    },
    {
        "n": 2,
        "g1": ("HONK", "LAKE", "TEFLRQ==", "NEST"),
        "g1f": ("WiCyS{d02_g1_honk}", "WiCyS{d02_g1_lake}", "WiCyS{d02_g1_nest}"),
        "g2": ("PADDLE", "CRUMB", "Q1JVTUI=", "PERCH"),
        "g2f": ("WiCyS{d02_g2_paddle}", "WiCyS{d02_g2_crumb}", "WiCyS{d02_g2_perch}"),
    },
    {
        "n": 3,
        "g1": ("PEEP", "POOL", "UE9PTA==", "BARN"),
        "g1f": ("WiCyS{d03_g1_peep}", "WiCyS{d03_g1_pool}", "WiCyS{d03_g1_barn}"),
        "g2": ("DABBLE", "SEEDS", "U0VFRFM=", "BRANCH"),
        "g2f": ("WiCyS{d03_g2_dabble}", "WiCyS{d03_g2_seeds}", "WiCyS{d03_g2_branch}"),
    },
    {
        "n": 4,
        "g1": ("CHIRP", "REED", "UkVFRA==", "COOP"),
        "g1f": ("WiCyS{d04_g1_chirp}", "WiCyS{d04_g1_reed}", "WiCyS{d04_g1_coop}"),
        "g2": ("BOBBLE", "CORN", "Q09STg==", "LEDGE"),
        "g2f": ("WiCyS{d04_g2_bobble}", "WiCyS{d04_g2_corn}", "WiCyS{d04_g2_ledge}"),
    },
    {
        "n": 5,
        "g1": ("SQUAWK", "DOCK", "RE9DSw==", "YARD"),
        "g1f": ("WiCyS{d05_g1_squawk}", "WiCyS{d05_g1_dock}", "WiCyS{d05_g1_yard}"),
        "g2": ("TODDLE", "GRAIN", "R1JBSU4=", "TWIG"),
        "g2f": ("WiCyS{d05_g2_toddle}", "WiCyS{d05_g2_grain}", "WiCyS{d05_g2_twig}"),
    },
    {
        "n": 6,
        "g1": ("FLAP", "WAVE", "V0FWRQ==", "PATH"),
        "g1f": ("WiCyS{d06_g1_flap}", "WiCyS{d06_g1_wave}", "WiCyS{d06_g1_path}"),
        "g2": ("AMBLE", "WHEAT", "V0hFQVQ=", "LIMB"),
        "g2f": ("WiCyS{d06_g2_amble}", "WiCyS{d06_g2_wheat}", "WiCyS{d06_g2_limb}"),
    },
    {
        "n": 7,
        "g1": ("DIVE", "RAIN", "UkFJTg==", "GATE"),
        "g1f": ("WiCyS{d07_g1_dive}", "WiCyS{d07_g1_rain}", "WiCyS{d07_g1_gate}"),
        "g2": ("SHUFFLE", "OATS", "T0FUUw==", "BOUGH"),
        "g2f": ("WiCyS{d07_g2_shuffle}", "WiCyS{d07_g2_oats}", "WiCyS{d07_g2_bough}"),
    },
    {
        "n": 8,
        "g1": ("SWIM", "SHIP", "U0hJUA==", "ROOST"),
        "g1f": ("WiCyS{d08_g1_swim}", "WiCyS{d08_g1_ship}", "WiCyS{d08_g1_roost}"),
        "g2": ("SCUFFLE", "MAIZE", "TUFJWkU=", "STICK"),
        "g2f": ("WiCyS{d08_g2_scuffle}", "WiCyS{d08_g2_maize}", "WiCyS{d08_g2_stick}"),
    },
]

COLOUR_G1 = "YELLOW"
COLOUR_G2 = "GREEN"


def print_table() -> None:
    print()
    print("Game 1  (stage3 colour = YELLOW)")
    print(f"{'#':>2}  {'S1':<8} {'S2':<6} {'Base64':<10} {'S3':<6}  Flags")
    print("-" * 88)
    for d in DUCKS:
        a, b, b64, c = d["g1"]
        f1, f2, f3 = d["g1f"]
        print(f"{d['n']:>2}  {a:<8} {b:<6} {b64:<10} {c:<6}  {f1}")
        print(f"{'':>2}  {'':<8} {'':<6} {'':<10} {'':<6}  {f2}")
        print(f"{'':>2}  {'':<8} {'':<6} {'':<10} {'':<6}  {f3}")
    print()
    print("Game 2  (stage3 colour = GREEN)")
    print(f"{'#':>2}  {'S1':<8} {'S2':<6} {'Base64':<10} {'S3':<6}  Flags")
    print("-" * 88)
    for d in DUCKS:
        a, b, b64, c = d["g2"]
        f1, f2, f3 = d["g2f"]
        print(f"{d['n']:>2}  {a:<8} {b:<6} {b64:<10} {c:<6}  {f1}")
        print(f"{'':>2}  {'':<8} {'':<6} {'':<10} {'':<6}  {f2}")
        print(f"{'':>2}  {'':<8} {'':<6} {'':<10} {'':<6}  {f3}")
    print()


def parse_serial_list(values: list[str]) -> list[int]:
    """Accept 3 | 1 2 3 | 1-4 | 1,2,5-8"""
    out: list[int] = []
    for token in values:
        for part in token.split(","):
            part = part.strip()
            if not part:
                continue
            if "-" in part:
                a, b = part.split("-", 1)
                lo, hi = int(a), int(b)
                if lo > hi:
                    lo, hi = hi, lo
                out.extend(range(lo, hi + 1))
            else:
                out.append(int(part))
    # unique, stable order
    seen: set[int] = set()
    ordered: list[int] = []
    for n in out:
        if n < 1 or n > 8:
            sys.exit(f"Duck serial must be 1..8 (got {n})")
        if n not in seen:
            seen.add(n)
            ordered.append(n)
    return ordered


def list_ports() -> list[str]:
    cands = sorted(
        p
        for p in glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*")
        if "Bluetooth" not in p
    )
    if not cands:
        cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    return cands


def find_port(explicit: str | None) -> str:
    if explicit:
        if not os.path.exists(explicit):
            sys.exit(f"Port not found: {explicit}")
        return explicit
    cands = list_ports()
    if not cands:
        sys.exit("No serial port found. Plug in the CYD or pass --port.")
    if len(cands) > 1:
        print("Ports:", ", ".join(cands))
        print(f"Using {cands[0]} (override with --port)")
    return cands[0]


def wait_for_port(preferred: str | None, label: str) -> str:
    """Wait until a CYD serial port appears (after user plugs the next board)."""
    print(f"\n>>> Plug in {label}, then press Enter ", end="", flush=True)
    try:
        input()
    except EOFError:
        sys.exit(1)

    for _ in range(40):
        cands = list_ports()
        if preferred and preferred in cands:
            return preferred
        if cands:
            if preferred and preferred not in cands:
                print(f"Note: {preferred} gone; using {cands[0]}")
            return cands[0]
        time.sleep(0.25)
    sys.exit("Timed out waiting for serial port.")


def set_serial(n: int) -> None:
    text = open(IDENTITY, encoding="utf-8").read()
    new, count = re.subn(
        r"(#ifndef DUCK_SERIAL\n#define DUCK_SERIAL )\d+",
        rf"\g<1>{n}",
        text,
        count=1,
    )
    if count != 1:
        new, count = re.subn(
            r"(#define DUCK_SERIAL )\d+",
            rf"\g<1>{n}",
            text,
            count=1,
        )
    if count != 1:
        sys.exit(f"Could not update DUCK_SERIAL in {IDENTITY}")
    open(IDENTITY, "w", encoding="utf-8").write(new)
    print(f"duck_identity.h → DUCK_SERIAL {n}")


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(r.returncode)


def flash(n: int, port: str, compile_only: bool) -> None:
    duck = next(d for d in DUCKS if d["n"] == n)
    a, b, _, c = duck["g1"]
    print(f"\n=== Duck-{n}  BLE: Yellow-Duck-{n} (BOOT cycles colour) ===")
    print(f"Game 1: {a} → {b} → [{COLOUR_G1}] {c}")
    print(f"Flags:  {', '.join(duck['g1f'])}")
    print()

    set_serial(n)
    extra = f"-DDUCK_SERIAL={n}"
    run(
        [
            "arduino-cli",
            "compile",
            "--fqbn",
            FQBN,
            "--build-property",
            f"compiler.cpp.extra_flags={extra}",
            "--export-binaries",
            SKETCH,
        ]
    )
    if compile_only:
        print("Compile-only done.")
        return
    run(["arduino-cli", "upload", "-p", port, "--fqbn", FQBN, SKETCH])
    print(f"\n✓ Flashed Duck-{n} on {port}")
    print(f"  nRF Connect → Yellow-Duck-{n} (or other colour)")
    print("  First swipe → 3-cross touch cal, then admin PIN 24650")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Flash WiCyS CYD duck(s) by serial #",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  ./flash_duck.py --list
  ./flash_duck.py 3
  ./flash_duck.py 1 2 3
  ./flash_duck.py --batch 1-8
  ./flash_duck.py --batch 1,3,5-8 -p /dev/cu.usbserial-2120
""",
    )
    ap.add_argument(
        "serials",
        nargs="*",
        help="Duck number(s): 3   or   1 2 3   or   1-4",
    )
    ap.add_argument(
        "--batch",
        "-b",
        metavar="SPEC",
        help="Batch serials, e.g. 1-8 or 1,2,5-8 (prompts between boards)",
    )
    ap.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")
    ap.add_argument("--list", "-l", action="store_true", help="Print answer key table")
    ap.add_argument("--compile-only", action="store_true", help="Build but do not upload")
    ap.add_argument(
        "--no-prompt",
        action="store_true",
        help="Batch without pausing (same port must stay connected)",
    )
    args = ap.parse_args()

    if args.list:
        print_table()
        return

    specs: list[str] = []
    if args.batch:
        specs.append(args.batch)
    specs.extend(args.serials)

    if not specs:
        print_table()
        try:
            raw = input("Duck number(s) to flash (e.g. 3 or 1-8): ").strip()
        except EOFError:
            sys.exit(1)
        if not raw:
            sys.exit("No serial given")
        specs = [raw]

    nums = parse_serial_list(specs)
    batch = len(nums) > 1

    if batch:
        print(f"\nBatch flash: {', '.join(f'Duck-{n}' for n in nums)}")
        print("Each board needs its own compile (unique serial / answers).\n")

    preferred = args.port
    for i, n in enumerate(nums):
        if batch and not args.compile_only:
            if args.no_prompt and i > 0:
                port = find_port(preferred)
            else:
                port = wait_for_port(preferred, f"Duck-{n}")
                preferred = port  # remember last good path for next round
        else:
            port = find_port(preferred)

        flash(n, port, args.compile_only)

        if batch and i < len(nums) - 1 and not args.compile_only:
            print("\nUnplug this board before continuing.")

    if batch:
        print(f"\nDone — flashed {len(nums)} duck(s): {nums}")


if __name__ == "__main__":
    main()
