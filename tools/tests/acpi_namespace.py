"""Evaluate the generated AML's NT device contract using ACPICA.

Usage: python tools/tests/acpi_namespace.py path/to/acpiexec.exe tools/acpi/dsdt.aml
The compiler/evaluator is a host development tool, not part of the firmware.
"""
import re
import subprocess
import sys

root = r"\_SB.PCI0.ISA"
expected = {
    root + "._ADR": 0x10000,
    root + ".KBD._HID": 0x0303D041,  # PNP0303, enhanced PS/2 keyboard
    root + ".KBD._STA": 15,
    root + ".KBD._CRS": bytes.fromhex("47 01 60 00 60 00 01 01 47 01 64 00 64 00 01 01 22 02 00 79 00"),
    root + ".SBAU._HID": 0x03B0D041,  # PNPB003, Sound Blaster 16
    root + ".SBAU._CRS": bytes.fromhex(
        "47 01 20 02 20 02 01 10 47 01 30 03 30 03 01 02 "
        "47 01 88 03 88 03 01 04 22 20 00 2a 02 00 2a 20 02 79 00"
    ),
    root + ".MOU._HID": 0x130FD041,  # PNP0F13, PS/2 mouse
    root + ".MOU._STA": 15,
    root + ".MOU._CRS": bytes.fromhex("22 00 10 79 00"),
}
commands = "; ".join("evaluate " + path for path in expected)
run = subprocess.run([sys.argv[1], "-b", commands, sys.argv[2]],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True)
output = run.stdout
for path, value in expected.items():
    marker = "Evaluating " + path + "\n"
    if marker not in output:
        raise SystemExit("FAIL: no AML evaluation for " + path)
    section = output.split(marker, 1)[1].split("\nEvaluating ", 1)[0]
    if "failed" in section.lower() or "returned object" not in section:
        raise SystemExit("FAIL: missing AML object " + path)
    if isinstance(value, int):
        actual = re.search(r"\[Integer\]\s*=\s*([0-9A-Fa-f]+)", section)
        if not actual or int(actual[1], 16) != value:
            raise SystemExit("FAIL: wrong AML integer " + path)
    else:
        chunks = re.findall(r"\b[0-9A-Fa-f]{4}:\s+((?:[0-9A-Fa-f]{2}\s+)+)", section)
        if bytes.fromhex(" ".join(chunks)) != value:
            raise SystemExit("FAIL: wrong AML resource buffer " + path)
sb_status = root + ".SBAU._STA"
for enabled, xp, status in ((0, True, 0), (1, True, 15), (1, False, 0)):
    command = "evaluate " + sb_status
    if not xp:
        command = r'osi remove "Windows 2001"; evaluate \_SB._INI; ' + command
    run = subprocess.run(
        [sys.argv[1], "-fv", str(enabled), "-b", command, sys.argv[2]],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True,
    )
    result = run.stdout.split("Evaluating " + sb_status + "\n", 1)[-1]
    actual = re.search(r"\[Integer\]\s*=\s*([0-9A-Fa-f]+)", result)
    if not actual or int(actual[1], 16) != status:
        raise SystemExit(f"FAIL: wrong AML Sound Blaster status for enabled={enabled}, XP={xp}")
print("PASS: evaluated ISA PCI address, PS/2 IDs and resources, and SB16 ID, resources and status")
if "Outstanding cache allocations" in output:
    print("NOTE: ACPICA evaluator reports cache allocations during tool shutdown")
