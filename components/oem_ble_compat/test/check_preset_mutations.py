"""Prove migration guards reject deliberate defects, using temporary copies only.

Run: uv run --no-project components/oem_ble_compat/test/check_preset_mutations.py
Requires a C++17 g++; --compiler can name a toolchain outside PATH.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FRESH_SCHEMA = """  if (!has_saved && !state.schema_current) {
    if (!store.write_u8(PRESET_BANK_SCHEMA_KEY, 1) || !store.commit())
      return PresetImportResult::Error;
    state.schema_current = true;
  }
"""
BANK_HEADER = """    if (!store.write_u8(keys->count_key, presets.count) ||
        !store.write_u8(keys->tag_key, 0x66)) return false;
"""

# Expected failure names prevent compiler errors or unrelated failures from
# counting as a killed mutation.
MUTATIONS = [
    ("reversed-bank", "oem_ble_compat_logic.h",
     [("case 2: return &high;", "case 2: return &low;")],
     "distinct OEM banks and stale caches"),
    ("stale-cache", "oem_preset_storage.h",
     [("if (source == PresetImportResult::Absent && has_saved) staged = saved;",
       "if (has_saved) staged = saved;")],
     "distinct OEM banks and stale caches"),
    ("early-marker", "oem_preset_storage.h",
     [("  if (!store.commit()) return false;",
       "  if (!state.schema_current && !store.write_u8(PRESET_BANK_SCHEMA_KEY, 1)) return false;\n"
       "  if (!store.commit()) return false;")],
     "power loss before and after every"),
    ("ignored-write-error", "oem_preset_storage.h",
     [("!store.write_u8(keys->count_key, presets.count)",
       "(store.write_u8(keys->count_key, presets.count), false)")],
     "every destination write or commit failure"),
    ("lost-dirty-flag", "oem_preset_storage.h",
     [("  if (!state.ready || !other_writes_ok || presets.count > 4) return false;",
       "  state.dirty = false;\n"
       "  if (!state.ready || !other_writes_ok || presets.count > 4) return false;")],
     "every destination write or commit failure"),
    ("cache-before-fresh-marker", "oem_preset_storage.h",
     [(FRESH_SCHEMA, ""), ("  out = staged;", FRESH_SCHEMA + "  out = staged;")],
     "power loss before and after every"),
    ("ignored-existing-marker", "oem_preset_storage.h",
     [("preset_import_dip(dip, has_saved, state.schema_current)",
       "preset_import_dip(dip, has_saved, false)")],
     "marker makes the corrected bank authoritative"),
    ("published-bank-before-values", "oem_preset_storage.h",
     [(BANK_HEADER, ""), ("  if (keys) {\n", "  if (keys) {\n" + BANK_HEADER)],
     "power loss before and after every"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler and os.name == "nt" and args.compiler == "g++":
        compiler = next((str(p) for p in (
            Path("C:/msys64/mingw64/bin/g++.exe"),
            Path("C:/tools/msys64/mingw64/bin/g++.exe"),
        ) if p.is_file()), None)
    if not compiler:
        raise SystemExit("C++17 compiler not found; pass --compiler")
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
    component = Path(__file__).resolve().parent.parent
    originals = {name: (component / name).read_text(encoding="utf-8")
                 for name in ("oem_ble_compat_logic.h", "oem_preset_storage.h")}
    with tempfile.TemporaryDirectory(prefix="qc-preset-mutations-") as tmp:
        root = Path(tmp)
        (root / "test").mkdir()
        for name in ("test_utils.h", "test_oem_preset_storage.cpp"):
            shutil.copy2(component / "test" / name, root / "test" / name)
        exe = root / ("suite.exe" if os.name == "nt" else "suite")

        def run(edits=None):
            for name, text in originals.items():
                (root / name).write_text(text, encoding="utf-8")
            if edits:
                name, replacements = edits
                text = originals[name]
                for old, new in replacements:
                    if text.count(old) != 1:
                        raise RuntimeError(f"Mutation anchor drift in {name}: {old!r}")
                    text = text.replace(old, new, 1)
                (root / name).write_text(text, encoding="utf-8")
            compile_result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
                 str(root / "test/test_oem_preset_storage.cpp"), "-o", str(exe)],
                env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            if compile_result.returncode:
                raise RuntimeError("Compilation failure is not a killed mutation:\n"
                                   + compile_result.stderr)
            return subprocess.run([str(exe)], env=env, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace")

        baseline = run()
        if baseline.returncode:
            raise RuntimeError("Baseline failed:\n" + baseline.stdout + baseline.stderr)
        print("PASS baseline", flush=True)
        for label, header, replacements, expected in MUTATIONS:
            result = run((header, replacements))
            if result.returncode == 0 or f"FAIL  migration: {expected}" not in result.stdout:
                raise RuntimeError(f"Mutation survived or failed unexpectedly: {label}\n"
                                   + result.stdout + result.stderr)
            print(f"KILLED {label}", flush=True)
        restored = run()
        if restored.returncode:
            raise RuntimeError("Restored baseline failed:\n" + restored.stdout + restored.stderr)
        print(f"PASS restored baseline; {len(MUTATIONS)} mutations killed", flush=True)


if __name__ == "__main__":
    main()
