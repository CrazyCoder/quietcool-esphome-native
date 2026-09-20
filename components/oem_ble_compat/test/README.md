# Preset migration verification

The firmware and host suites use the same `load_presets` and `flush_presets`
functions in `../oem_preset_storage.h`, including the packed preference layout.
The ESP-IDF adapter in `oem_ble_compat.cpp` supplies typed NVS operations; the
host suite supplies a fault-injectable store.

## Run

From the repository root, on Windows:

```powershell
components/oem_ble_compat/test/run_tests.bat
```

On Linux or macOS, to run the migration suite:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic \
  components/oem_ble_compat/test/test_oem_preset_storage.cpp -o /tmp/qc-presets
/tmp/qc-presets
```

The deployment workflow compiles and runs every component's host suites before
building or publishing firmware.

Prove the guards reject deliberate regressions:

```sh
uv run --no-project components/oem_ble_compat/test/check_preset_mutations.py
```

The mutation runner needs a C++17 `g++`; use `--compiler <path>` if necessary.
It modifies temporary copies, never the checkout. It requires each mutant to
compile and fail the expected test, then verifies the restored baseline.

## Coverage

- Distinct Low, Med, and High fixtures, with an independent bank-selection
  oracle, for every wiring mode.
- Fresh, legacy, and marked installations; latest NVS data versus stale cache;
  absent, empty, incomplete, wrongly typed, and invalid banks.
- Source retention, destination contents, repeated boots, and migration-marker
  sequencing.
- Every startup storage operation and destination write/commit failure, before
  or after effects become persistent. Failed setup blocks preset flushing;
  failed flushes retain dirty state and allow retry.
- Power interruption before and after every operation in startup and flush,
  including cache publication. Cached preferences can persist immediately or be
  lost before their deferred write reaches storage.
- A bounded-key store that cannot allocate the marker, plus recovery when
  capacity becomes available.
- Mutations that reverse bank selection, prefer stale cache, publish a marker or
  bank tag too early, ignore write failures or an existing marker, lose dirty
  state, or create a fresh cache before its marker.

The suite prints its simulated-interruption count. An absent bank's tag is
written after its values and count: otherwise a reboot can accept stale or
partially restored values instead of the cache.

## Limits and hardware-test policy

Physical one-/three-speed migration testing is optional for this storage-only
change. Its automated release evidence consists of these suites, mutation
checks, configuration validation, a real ESPHome factory build, and the
documented OEM bank mapping. Report physical coverage separately: the owner
hub's completed candidate test covers two-speed wiring only.

The mock persists individual writes before commit; it does **not** provide
multi-key transaction rollback. It does not emulate flash wear, NVS page
compaction, electrical failure during a flash write, GPIO wiring, or relay
behavior. These tests do not establish atomicity for arbitrary concurrent preset
edits, recovery from corrupt hardware, or a round trip to OEM V4.4.
