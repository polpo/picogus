#!/usr/bin/env python3

# Used to fail the build if any non-panic code calls the raw printf-family
# wrappers that route to printf_none_assert.

import re
import subprocess
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(
            "usage: check_printf_leak.py <objdump> <addr2line> <elf>",
            file=sys.stderr,
        )
        return 2

    objdump, addr2line, elf = argv[1:4]

    dis = subprocess.run(
        [objdump, "-d", elf], check=True, capture_output=True, text=True
    ).stdout

    offenders = []
    call_re = re.compile(
        r"^\s*([0-9a-f]+):\s.*\bbl\s+[0-9a-f]+\s+<(__wrap_v?s?n?printf)>"
    )
    for line in dis.splitlines():
        m = call_re.match(line)
        if not m:
            continue
        call_site, wrap = m.group(1), m.group(2)
        res = subprocess.run(
            [addr2line, "-fe", elf, call_site],
            check=True,
            capture_output=True,
            text=True,
        )
        lines = res.stdout.strip().splitlines()
        if not lines:
            continue
        caller = lines[0].strip()
        # Skip pico_stdio wrappers calling each other internally
        # (__wrap_printf -> __wrap_vprintf etc)
        if caller.startswith("__wrap_"):
            continue
        location = lines[1].strip() if len(lines) > 1 else "<unknown>"
        offenders.append((call_site, wrap, caller, location))

    if not offenders:
        return 0

    print(
        f"error: {elf} contains printf call(s) that will panic:",
        file=sys.stderr,
    )
    for addr, wrap, caller, loc in offenders:
        print(f"  {loc}: {caller}() -> {wrap} [@0x{addr}]", file=sys.stderr)
    print(
        "Replace with DBG_PRINTF from sw/include/pg_debug.h (no-ops when PGDEBUG is off).",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
