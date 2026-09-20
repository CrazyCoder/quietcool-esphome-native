#pragma once

#include "oem_ble_compat_logic.h"

namespace qc {

struct Preset {
  char name[51] = "";
  int16_t values[6] = {};  // speed, high/medium/low temperature, high/low humidity
} __attribute__((packed));

struct PresetStorage {
  uint8_t count = 0;
  Preset presets[4] = {};
} __attribute__((packed));

enum class PresetRead { Ok, Missing, Error };

struct PresetPersistenceState {
  bool ready = false;
  bool schema_current = false;
  bool dirty = false;
};

// Store operations are individual NVS operations, not a transaction. In
// particular, writes may survive a reboot even when commit() has not succeeded.
template<typename Store>
PresetImportResult read_preset_bank(Store &store, uint8_t dip, PresetStorage &out) {
  const auto *keys = nvs_preset_keys_for_dip(dip);
  if (!keys) return PresetImportResult::Absent;
  uint8_t tag = 0;
  const auto tag_result = store.read_u8(keys->tag_key, tag);
  if (tag_result == PresetRead::Missing ||
      (tag_result == PresetRead::Ok && tag != 0x66))
    return PresetImportResult::Absent;
  if (tag_result != PresetRead::Ok) return PresetImportResult::Error;
  uint8_t count = 0;
  if (store.read_u8(keys->count_key, count) != PresetRead::Ok)
    return PresetImportResult::Error;
  const bool ok = read_preset_entries(out, count,
      [&](int slot, char *name, size_t capacity) {
        char key[20];
        keys->format_name_key(slot, key, sizeof(key));
        return store.read_string(key, name, capacity) == PresetRead::Ok;
      },
      [&](int slot, int index, int16_t &value) {
        char key[12];
        keys->format_value_key(slot, index, key, sizeof(key));
        return store.read_i16(key, value) == PresetRead::Ok;
      });
  return ok ? PresetImportResult::Loaded : PresetImportResult::Error;
}

template<typename Store, typename SaveCache>
PresetImportResult load_presets(Store &store, uint8_t dip, bool has_saved,
                               const PresetStorage &saved, PresetStorage &out,
                               PresetPersistenceState &state, SaveCache save_cache) {
  state.ready = false;
  state.schema_current = false;
  has_saved = has_saved && saved.count <= 4;
  uint8_t schema = 0;
  const auto schema_result = store.read_u8(PRESET_BANK_SCHEMA_KEY, schema);
  if (schema_result == PresetRead::Error ||
      (schema_result == PresetRead::Ok && schema != 1))
    return PresetImportResult::Error;
  state.schema_current = schema_result == PresetRead::Ok;
  // Certify fresh layout before publishing a cache that could otherwise be
  // mistaken for a legacy installation after a power interruption.
  if (!has_saved && !state.schema_current) {
    if (!store.write_u8(PRESET_BANK_SCHEMA_KEY, 1) || !store.commit())
      return PresetImportResult::Error;
    state.schema_current = true;
  }
  PresetStorage staged{};
  const auto source = read_preset_bank(
      store, preset_import_dip(dip, has_saved, state.schema_current), staged);
  if (source == PresetImportResult::Error) return source;
  if (source == PresetImportResult::Absent && has_saved) staged = saved;
  if (source == PresetImportResult::Loaded) save_cache(staged);
  out = staged;
  state.ready = true;
  if (!state.schema_current && nvs_preset_keys_for_dip(dip)) state.dirty = true;
  return source;
}

template<typename Store>
bool flush_presets(Store &store, uint8_t dip, const PresetStorage &presets,
                   PresetPersistenceState &state, bool other_writes_ok = true) {
  if (!state.ready || !other_writes_ok || presets.count > 4) return false;
  const auto *keys = nvs_preset_keys_for_dip(dip);
  if (keys) {
    for (int slot = 0; slot < presets.count; ++slot) {
      // OEM readers use the primary name; omit redundant triplicated copies
      // to conserve entries in the 16 KB NVS partition.
      char key[20];
      keys->format_name_key(slot, key, sizeof(key));
      if (!store.write_string(key, presets.presets[slot].name)) return false;
      for (int j = 0; j < 6; ++j) {
        keys->format_value_key(slot, j, key, sizeof(key));
        if (!store.write_i16(key, presets.presets[slot].values[j])) return false;
      }
    }
    // Publish an absent bank only after its contents. Otherwise a reboot can
    // prefer partially restored data over the cache, especially when source
    // and destination are the same two-speed bank.
    if (!store.write_u8(keys->count_key, presets.count) ||
        !store.write_u8(keys->tag_key, 0x66)) return false;
  }
  // Never certify migration before every destination write and its commit.
  // Retain the source bank and dirty flag on failure so a retry can finish.
  if (!store.commit()) return false;
  if (keys && !state.schema_current) {
    if (!store.write_u8(PRESET_BANK_SCHEMA_KEY, 1) || !store.commit()) return false;
    state.schema_current = true;
  }
  state.dirty = false;
  return true;
}

}  // namespace qc
