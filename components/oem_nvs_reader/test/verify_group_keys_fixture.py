"""Verify group_history_keys() against an NVS partition dump.

Parses the C++ list out of `group_history_keys()` in oem_nvs_reader_logic.h,
walks a binary ESP-IDF NVS partition dump, finds every key in the `hx_list`
namespace starting with "Group_", and asserts:

  1. Every key in the dump's hx_list/Group_* set appears in the declared C++
     list (no OEM key would be missed by cleanup — silent leak risk).
  2. Every declared key appears in the dump (no typo / dead entry in the
     cleanup list — wasted code path risk).

Only group_history_keys() is checked — the sibling preset_name_dup_keys()
list holds testname* keys that mirror the OEM write path (not dump-verifiable)
and aren't `Group_*`, so they're intentionally out of scope here.

Supplying a dump:
  This check needs an ESP-IDF NVS partition dump that contains the `hx_list`
  namespace (e.g. dumped from an OEM device with esptool read-flash of the nvs
  partition). Point it at your dump one of two ways:

    * set the QC_NVS_FIXTURE environment variable to the dump's path, or
    * drop the dump at test/nvs-fixture.bin (next to this script).

  If no dump is present the check SKIPS (exit 0) — it is an optional
  cross-check, not a hard gate, so the test suite still passes on a clean
  checkout without a dump.

Exits 0 on match (or when skipped), 1 on any drift.
Run from run_tests.bat, or directly: `python verify_group_keys_fixture.py`.
"""
from __future__ import annotations

import os
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

PAGE_SIZE = 4096
ENTRY_SIZE = 32
ENTRY_STATE_WRITTEN = 0b10

# Resolve paths relative to this script so it runs from any CWD.
HERE = Path(__file__).resolve().parent
# Fixture location: QC_NVS_FIXTURE env var wins; otherwise a repo-local dump
# sitting next to this script. Never walk up into a parent tree.
_env_fixture = os.environ.get("QC_NVS_FIXTURE")
FIXTURE = Path(_env_fixture) if _env_fixture else HERE / "nvs-fixture.bin"
HEADER = HERE.parent / "oem_nvs_reader_logic.h"


def parse_declared_keys(header_text: str) -> list[str]:
    """Extract the string literals from `group_history_keys()`'s vector init."""
    m = re.search(
        r"group_history_keys\(\)\s*\{[^{]*static const std::vector<std::string> keys\s*=\s*\{(.*?)\};",
        header_text, re.DOTALL,
    )
    if not m:
        raise RuntimeError("could not locate group_history_keys() vector literal in header")
    return re.findall(r'"([^"]+)"', m.group(1))


def iter_written_entries(data: bytes):
    """Yield (ns_idx, key, dtype, span, raw_entry) for every WRITTEN entry.

    raw_entry is the full 32-byte slice so callers can read namespace-decl
    values from offset [24]. Skips UNINIT pages entirely.
    """
    for p in range(len(data) // PAGE_SIZE):
        page = data[p * PAGE_SIZE:(p + 1) * PAGE_SIZE]
        (state,) = struct.unpack("<I", page[:4])
        if state == 0xFFFFFFFF:
            continue  # UNINIT
        entries = page[64:]
        bitmap = page[32:64]
        states: list[int] = []
        for byte in bitmap:
            for shift in (0, 2, 4, 6):
                states.append((byte >> shift) & 0b11)
        states = states[:126]
        i = 0
        while i < 126:
            if states[i] != ENTRY_STATE_WRITTEN:
                i += 1
                continue
            raw = entries[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            ns_idx, dtype, span = raw[0], raw[1], raw[2]
            span = max(span, 1)
            key = raw[8:24].split(b"\x00", 1)[0].decode("latin-1", errors="replace")
            yield (ns_idx, key, dtype, span, raw)
            i += span


def main() -> int:
    if not FIXTURE.exists():
        print(
            "SKIP: no NVS fixture present "
            "(set QC_NVS_FIXTURE=path to a hx_list NVS partition dump "
            "to enable this check)"
        )
        return 0
    if not HEADER.exists():
        print(f"FAIL: header not found at {HEADER}")
        return 1

    declared = parse_declared_keys(HEADER.read_text())
    data = FIXTURE.read_bytes()

    # First pass: discover namespaces. ns-decl entries have ns_idx=0,
    # dtype=0x01, and the assigned index is the byte at raw[24].
    ns_map: dict[int, str] = {}
    for ns_idx, key, dtype, _, raw in iter_written_entries(data):
        if ns_idx == 0 and dtype == 0x01:
            ns_map[raw[24]] = key

    if "hx_list" not in ns_map.values():
        print("FAIL: hx_list namespace not present in fixture")
        return 1
    hx_idx = next(idx for idx, nm in ns_map.items() if nm == "hx_list")

    # Second pass: collect Group_* entries in hx_list, summing total bytes per
    # key (a single logical blob has BLOB_DATA + BLOB_IDX entries, same key).
    by_key: dict[str, int] = defaultdict(int)
    for ns_idx, key, _, span, _ in iter_written_entries(data):
        if ns_idx == hx_idx and key.startswith("Group_"):
            by_key[key] += span * ENTRY_SIZE

    actual = set(by_key)
    expected = set(declared)
    missing = actual - expected
    extra = expected - actual

    print("group_history_keys() declares:", declared)
    print("hx_list/Group_* found in dump:")
    for k in sorted(by_key):
        print(f"  {k:<14} {by_key[k]:>6} bytes")
    total = sum(by_key.values())
    print(f"  {'TOTAL':<14} {total:>6} bytes (~{total / 1024:.1f} KB)")

    if missing:
        print(f"\nFAIL: declared list MISSING keys present in dump: {sorted(missing)}")
        print("  Action: add them to group_history_keys() in oem_nvs_reader_logic.h")
        return 1
    if extra:
        print(f"\nFAIL: declared list has EXTRA keys not in dump: {sorted(extra)}")
        print("  Action: either remove from group_history_keys(), or re-dump NVS")
        return 1

    print(f"\nPASS  group_history_keys() matches the NVS dump exactly "
          f"({len(actual)} keys, {total} bytes reclaimable)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
