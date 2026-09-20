#include "test_utils.h"
#include "../oem_preset_storage.h"

#include <limits>
#include <map>
#include <variant>

using namespace qc;
using Cell = std::variant<uint8_t, int16_t, std::string>;
using Cells = std::map<std::string, Cell>;

static_assert(sizeof(Preset) == 63 && sizeof(PresetStorage) == 253,
              "The existing ESPHome preference layout must not change");
static_assert(alignof(Preset) == 1 && alignof(PresetStorage) == 1);

struct Disk {
  Cells cells;
  PresetStorage cache{};
  bool has_cache = false;
};

struct PowerLoss {};

// Typed NVS model: each successful write persists immediately, even if commit
// later fails. No transaction rollback. This does not emulate flash compaction.
struct Nvs {
  Disk &disk;
  explicit Nvs(Disk &storage) : disk(storage) {}
  size_t fail_at = 0;
  size_t cut_before = 0;
  size_t cut_after = 0;
  bool fail_after_effect = false;
  bool persist_cache = true;
  size_t capacity = std::numeric_limits<size_t>::max();
  std::vector<std::string> trace;
  std::vector<Cells> commits;

  template<typename Effect>
  bool step(const std::string &label, Effect effect) {
    trace.push_back(label);
    const auto position = trace.size();
    if (cut_before == position) throw PowerLoss{};
    if (fail_at == position && !fail_after_effect) return false;
    const bool ok = effect();
    if (cut_after == position) throw PowerLoss{};
    return ok && fail_at != position;
  }
  template<typename T>
  PresetRead read(const char *key, T &value) {
    PresetRead result = PresetRead::Error;
    const bool ok = step(std::string("read:") + key, [&] {
      auto it = disk.cells.find(key);
      if (it == disk.cells.end()) result = PresetRead::Missing;
      else if (auto *found = std::get_if<T>(&it->second)) {
        value = *found;
        result = PresetRead::Ok;
      }
      return true;
    });
    return ok ? result : PresetRead::Error;
  }
  PresetRead read_u8(const char *key, uint8_t &value) { return read(key, value); }
  PresetRead read_i16(const char *key, int16_t &value) { return read(key, value); }
  PresetRead read_string(const char *key, char *out, size_t capacity_bytes) {
    std::string value;
    const auto result = read(key, value);
    if (result != PresetRead::Ok) return result;
    if (value.size() + 1 > capacity_bytes) return PresetRead::Error;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return PresetRead::Ok;
  }
  template<typename T>
  bool write(const char *key, T value) {
    return step(std::string("write:") + key, [&] {
      if (!disk.cells.count(key) && disk.cells.size() >= capacity) return false;
      disk.cells[key] = value;
      return true;
    });
  }
  bool write_u8(const char *key, uint8_t value) { return write(key, value); }
  bool write_i16(const char *key, int16_t value) { return write(key, value); }
  bool write_string(const char *key, const char *value) { return write(key, std::string(value)); }
  bool commit() {
    return step("commit", [&] {
      // A reported commit failure is not evidence of a successful checkpoint.
      if (fail_at == trace.size()) return false;
      commits.push_back(disk.cells);
      return true;
    });
  }
  void save_cache(const PresetStorage &value) {
    step("cache", [&] {
      if (persist_cache) {
        disk.cache = value;
        disk.has_cache = true;
      }
      return true;
    });
  }
};

// Independent oracle: do NOT seed fixtures using the mapping under test.
const NvsPresetKeys BANKS[] = {
    {"Med", 'm', "medsize1", "PresetsMed"},
    {"High", 'h', "highsize1", "PresetsHigh"},
    {"Low", 'l', "lowsize1", "PresetsLow"},
};

PresetStorage values(const std::string &name, int base, uint8_t count = 4) {
  PresetStorage out{};
  out.count = count;
  for (int slot = 0; slot < count; ++slot) {
    std::snprintf(out.presets[slot].name, sizeof(out.presets[slot].name),
                  "%s%d", name.c_str(), slot);
    for (int j = 0; j < 6; ++j) out.presets[slot].values[j] = base + slot * 10 + j;
  }
  return out;
}

void equal(const PresetStorage &actual, const PresetStorage &expected) {
  REQUIRE_EQ(actual.count, expected.count);
  // Include unused slots: zero initialization is part of the cache contract.
  for (int slot = 0; slot < 4; ++slot) {
    REQUIRE_EQ(std::string(actual.presets[slot].name), std::string(expected.presets[slot].name));
    for (int j = 0; j < 6; ++j)
      REQUIRE_EQ(actual.presets[slot].values[j], expected.presets[slot].values[j]);
  }
}

void seed_bank(Cells &cells, int bank, const PresetStorage &data) {
  const auto &keys = BANKS[bank - 1];
  cells[keys.tag_key] = uint8_t(0x66);
  cells[keys.count_key] = data.count;
  for (int slot = 0; slot < data.count; ++slot) {
    char key[20];
    keys.format_name_key(slot, key, sizeof(key));
    cells[key] = std::string(data.presets[slot].name);
    for (int j = 0; j < 6; ++j) {
      keys.format_value_key(slot, j, key, sizeof(key));
      cells[key] = int16_t(data.presets[slot].values[j]);
    }
  }
}

PresetStorage bank_values(int bank) { return values(BANKS[bank - 1].value_prefix, bank * 60); }
int legacy_bank(int dip) { return dip == 2 ? 3 : dip == 3 ? 2 : 1; }

Disk fixture(bool legacy) {
  Disk disk;
  for (int bank = 1; bank <= 3; ++bank) seed_bank(disk.cells, bank, bank_values(bank));
  disk.cells["Phone1"] = std::string("fixture-pair-id");
  disk.cells["flag_PhoneID"] = uint8_t(0x41);
  disk.cells["pair_num"] = uint8_t(1);
  disk.cache = values("stale-cache", 500, 2);
  disk.has_cache = legacy;
  return disk;
}

struct Session {
  PresetStorage presets{};
  PresetPersistenceState state;
  PresetImportResult boot(Nvs &nvs, uint8_t dip) {
    return load_presets(nvs, dip, nvs.disk.has_cache, nvs.disk.cache, presets, state,
                        [&](const PresetStorage &p) { nvs.save_cache(p); });
  }
  bool flush(Nvs &nvs, uint8_t dip, bool other_writes_ok = true) {
    return flush_presets(nvs, dip, presets, state, other_writes_ok);
  }
};

void contains_bank(const Cells &cells, int bank, const PresetStorage &expected) {
  Cells required;
  seed_bank(required, bank, expected);
  for (const auto &[key, value] : required) {
    REQUIRE(cells.count(key) == 1);
    REQUIRE(cells.at(key) == value);
  }
}

void unaffected(const Disk &disk, const Disk &before, int target) {
  Cells allowed;
  seed_bank(allowed, target, bank_values(target));
  for (const auto &[key, value] : before.cells)
    if (!allowed.count(key) && key != PRESET_BANK_SCHEMA_KEY) {
      REQUIRE(disk.cells.count(key) == 1);
      REQUIRE(disk.cells.at(key) == value);
    }
}

void recover(Disk &disk, int dip, const PresetStorage &expected) {
  Nvs nvs{disk};
  Session reboot;
  REQUIRE(reboot.boot(nvs, dip) != PresetImportResult::Error);
  equal(reboot.presets, expected);
  reboot.state.dirty = true;
  REQUIRE(reboot.flush(nvs, dip));
  REQUIRE(!reboot.state.dirty);
  REQUIRE(reboot.state.schema_current);
  contains_bank(disk.cells, dip, expected);
  Nvs again{disk};
  Session stable;
  REQUIRE(stable.boot(again, dip) == PresetImportResult::Loaded);
  equal(stable.presets, expected);
  REQUIRE(!stable.state.dirty);
  REQUIRE(stable.state.schema_current);
}

TEST("migration: distinct OEM banks and stale caches select the correct source") {
  for (int dip = 1; dip <= 3; ++dip)
    for (bool legacy : {false, true}) {
      auto disk = fixture(legacy);
      const auto before = disk;
      Nvs nvs{disk};
      Session session;
      REQUIRE(session.boot(nvs, dip) == PresetImportResult::Loaded);
      const auto expected = bank_values(legacy ? legacy_bank(dip) : dip);
      equal(session.presets, expected);
      REQUIRE_EQ(session.state.schema_current, !legacy);
      REQUIRE_EQ(session.state.dirty, legacy);
      session.state.dirty = true;
      REQUIRE(session.flush(nvs, dip));
      unaffected(disk, before, dip);
      contains_bank(disk.cells, legacy ? legacy_bank(dip) : dip, expected);
      recover(disk, dip, expected);
    }
}

TEST("migration: marker makes the corrected bank authoritative on reboot") {
  for (int dip = 1; dip <= 3; ++dip) {
    auto disk = fixture(true);
    disk.cells[PRESET_BANK_SCHEMA_KEY] = uint8_t(1);
    recover(disk, dip, bank_values(dip));
  }
}

TEST("migration: empty storage and invalid caches are fresh installations") {
  for (int dip = 1; dip <= 3; ++dip) {
    Disk empty;
    recover(empty, dip, PresetStorage{});
    auto invalid = fixture(true);
    invalid.cache.count = 5;
    recover(invalid, dip, bank_values(dip));
  }
}

TEST("migration: uninitialized bank tags allow cache fallback") {
  for (int dip = 1; dip <= 3; ++dip)
    for (uint8_t tag : {uint8_t(0), uint8_t(1), uint8_t(255)}) {
      auto disk = fixture(true);
      disk.cells[BANKS[legacy_bank(dip) - 1].tag_key] = tag;
      const auto expected = disk.cache;
      recover(disk, dip, expected);
    }
}

TEST("migration: full-length names and signed values survive the packed layout") {
  for (int dip = 1; dip <= 3; ++dip) {
    auto disk = fixture(true);
    auto expected = bank_values(legacy_bank(dip));
    std::memset(expected.presets[0].name, 'x', 50);
    expected.presets[0].name[50] = '\0';
    expected.presets[0].values[0] = std::numeric_limits<int16_t>::min();
    expected.presets[0].values[5] = std::numeric_limits<int16_t>::max();
    seed_bank(disk.cells, legacy_bank(dip), expected);
    recover(disk, dip, expected);
  }
}

TEST("migration: absent banks fall back to cache but explicit empty banks do not") {
  for (int dip = 1; dip <= 3; ++dip) {
    auto disk = fixture(true);
    disk.cells.erase(BANKS[legacy_bank(dip) - 1].tag_key);
    const auto cached = disk.cache;
    recover(disk, dip, cached);
    disk = fixture(true);
    disk.cells[BANKS[legacy_bank(dip) - 1].count_key] = uint8_t(0);
    recover(disk, dip, PresetStorage{});
  }
}

TEST("migration: incomplete or wrongly typed banks never fall back or certify") {
  for (int dip = 1; dip <= 3; ++dip) {
    const auto &keys = BANKS[legacy_bank(dip) - 1];
    Cells fields;
    seed_bank(fields, legacy_bank(dip), bank_values(legacy_bank(dip)));
    for (const auto &[key, unused] : fields)
      for (bool wrong_type : {false, true}) {
        // An absent tag means no bank; a present but wrong-typed tag is an error.
        if (key == keys.tag_key && !wrong_type) continue;
        auto disk = fixture(true);
        if (wrong_type) disk.cells[key] = int16_t(-1); // strings/u8 mismatch; i16 tested below
        else disk.cells.erase(key);
        if (wrong_type && std::holds_alternative<int16_t>(unused))
          disk.cells[key] = std::string("not-an-integer");
        const auto before = disk;
        Nvs nvs{disk};
        Session session;
        REQUIRE(session.boot(nvs, dip) == PresetImportResult::Error);
        REQUIRE(!session.state.ready);
        REQUIRE(!session.flush(nvs, dip));
        REQUIRE(disk.cells == before.cells);
        equal(disk.cache, before.cache);
      }
  }
}

TEST("migration: invalid metadata and oversized names fail closed") {
  for (int dip = 1; dip <= 3; ++dip)
    for (int fault = 0; fault < 7; ++fault) {
      auto disk = fixture(true);
      const auto &keys = BANKS[legacy_bank(dip) - 1];
      if (fault < 3) disk.cells[PRESET_BANK_SCHEMA_KEY] = uint8_t(fault == 0 ? 0 : fault == 1 ? 2 : 255);
      if (fault == 3) disk.cells[PRESET_BANK_SCHEMA_KEY] = std::string("1");
      if (fault == 4 || fault == 5) disk.cells[keys.count_key] = uint8_t(fault == 4 ? 5 : 255);
      if (fault == 6) {
        char key[20];
        keys.format_name_key(0, key, sizeof(key));
        disk.cells[key] = std::string(51, 'x');
      }
      const auto before = disk;
      Nvs nvs{disk};
      Session session;
      REQUIRE(session.boot(nvs, dip) == PresetImportResult::Error);
      REQUIRE(!session.flush(nvs, dip));
      REQUIRE(disk.cells == before.cells);
      equal(disk.cache, before.cache);
    }
}

TEST("migration: every startup storage failure preserves data and blocks flushing") {
  for (int dip = 1; dip <= 3; ++dip)
    for (bool legacy : {false, true}) {
      auto baseline = fixture(legacy);
      Nvs successful{baseline};
      Session initial;
      REQUIRE(initial.boot(successful, dip) == PresetImportResult::Loaded);
      for (size_t point = 1; point <= successful.trace.size(); ++point)
        for (bool after : {false, true}) {
          if (successful.trace[point - 1] == "cache") continue; // optional, tested separately
          auto disk = fixture(legacy);
          const auto before = disk;
          Nvs nvs{disk};
          nvs.fail_at = point;
          nvs.fail_after_effect = after;
          Session session;
          REQUIRE(session.boot(nvs, dip) == PresetImportResult::Error);
          REQUIRE(!session.state.ready);
          const auto operations = nvs.trace.size();
          REQUIRE(!session.flush(nvs, dip));
          REQUIRE_EQ(nvs.trace.size(), operations);
          // Fresh startup may have persisted only its schema marker.
          auto actual = disk.cells;
          actual.erase(PRESET_BANK_SCHEMA_KEY);
          REQUIRE(actual == before.cells);
          equal(disk.cache, before.cache);
          recover(disk, dip, bank_values(legacy ? legacy_bank(dip) : dip));
        }
    }
}

TEST("migration: every destination write or commit failure retains dirty state and retries") {
  for (int dip = 1; dip <= 3; ++dip) {
    auto baseline = fixture(true);
    Nvs startup{baseline};
    Session initial;
    initial.boot(startup, dip);
    Nvs successful{baseline};
    REQUIRE(initial.flush(successful, dip));
    for (size_t point = 1; point <= successful.trace.size(); ++point)
      for (bool after : {false, true}) {
        auto disk = fixture(true);
        const auto before = disk;
        Nvs boot{disk};
        Session session;
        session.boot(boot, dip);
        Nvs nvs{disk};
        nvs.fail_at = point;
        nvs.fail_after_effect = after;
        REQUIRE(!session.flush(nvs, dip));
        REQUIRE(session.state.dirty);
        REQUIRE(!session.state.schema_current);
        const auto expected = bank_values(legacy_bank(dip));
        contains_bank(disk.cells, legacy_bank(dip), expected);
        unaffected(disk, before, dip);
        if (disk.cells.count(PRESET_BANK_SCHEMA_KEY)) {
          REQUIRE(!nvs.commits.empty());
          contains_bank(nvs.commits.front(), dip, expected);
          contains_bank(disk.cells, dip, expected);
        }
        // Retry in the same process, including marker-write success/commit failure.
        nvs.fail_at = 0;
        REQUIRE(session.flush(nvs, dip));
        REQUIRE(!session.state.dirty);
        recover(disk, dip, expected);
      }
  }
}

TEST("migration: power loss before and after every startup and flush operation recovers") {
  size_t interruptions = 0;
  for (int dip = 1; dip <= 3; ++dip)
    for (bool legacy : {false, true})
      for (bool persist_cache : {false, true})
      for (int shape = 0; shape < 3; ++shape) {
        // Exercise populated, absent, and explicitly empty source banks.
        const int source_bank = legacy ? legacy_bank(dip) : dip;
        auto make_disk = [&] {
          auto disk = fixture(legacy);
          if (shape == 1) disk.cells.erase(BANKS[source_bank - 1].tag_key);
          if (shape == 2) disk.cells[BANKS[source_bank - 1].count_key] = uint8_t(0);
          return disk;
        };
        auto baseline = make_disk();
        const auto expected = shape == 0 ? bank_values(source_bank) :
            shape == 1 && legacy ? baseline.cache : PresetStorage{};
        Nvs successful{baseline};
        successful.persist_cache = persist_cache;
        Session initial;
        initial.boot(successful, dip);
        initial.state.dirty = true;
        REQUIRE(initial.flush(successful, dip));
        for (size_t point = 1; point <= successful.trace.size(); ++point)
          for (bool after : {false, true}) {
            auto disk = make_disk();
            const auto before = disk;
            Nvs nvs{disk};
            nvs.persist_cache = persist_cache;
            if (after) nvs.cut_after = point;
            else nvs.cut_before = point;
            Session session;
            bool interrupted = false;
            try {
              session.boot(nvs, dip);
              session.state.dirty = true;
              session.flush(nvs, dip);
            } catch (const PowerLoss &) { interrupted = true; }
            REQUIRE(interrupted);
            ++interruptions;
            unaffected(disk, before, dip);
            if (shape != 1) contains_bank(disk.cells, source_bank, expected);
            if (legacy && disk.cells.count(PRESET_BANK_SCHEMA_KEY)) {
              REQUIRE(!nvs.commits.empty());
              contains_bank(nvs.commits.front(), dip, expected);
              contains_bank(disk.cells, dip, expected);
            }
            recover(disk, dip, expected);
          }
      }
  std::cout << "    simulated power interruptions: " << interruptions << "\n";
}

TEST("migration: finite key capacity prevents certification and recovery succeeds after space is freed") {
  for (int dip = 1; dip <= 3; ++dip) {
    auto disk = fixture(true);
    Nvs nvs{disk};
    Session session;
    session.boot(nvs, dip);
    nvs.capacity = disk.cells.size(); // no space for the migration marker
    REQUIRE(!session.flush(nvs, dip));
    REQUIRE(session.state.dirty);
    REQUIRE(!disk.cells.count(PRESET_BANK_SCHEMA_KEY));
    nvs.capacity++;
    REQUIRE(session.flush(nvs, dip));
    recover(disk, dip, bank_values(legacy_bank(dip)));
  }
}

TEST("migration: other section failures cannot certify or clear pending writes") {
  auto disk = fixture(true);
  Nvs nvs{disk};
  Session session;
  session.boot(nvs, 2);
  const auto before = disk.cells;
  REQUIRE(!session.flush(nvs, 2, false));
  REQUIRE(session.state.dirty);
  REQUIRE(disk.cells == before);
  REQUIRE(session.flush(nvs, 2));
}

TEST("migration: invalid DIP never selects a bank or certifies a legacy layout") {
  for (uint8_t dip : {uint8_t(0), uint8_t(4), uint8_t(255)}) {
    auto disk = fixture(true);
    const auto before = disk;
    Nvs nvs{disk};
    Session session;
    REQUIRE(session.boot(nvs, dip) == PresetImportResult::Absent);
    equal(session.presets, before.cache);
    session.state.dirty = true;
    REQUIRE(session.flush(nvs, dip));
    REQUIRE(!session.state.schema_current);
    REQUIRE(disk.cells == before.cells);
  }
}

TEST("migration: cache write failure does not replace authoritative NVS with defaults") {
  for (int dip = 1; dip <= 3; ++dip)
    for (bool legacy : {false, true}) {
      auto baseline = fixture(legacy);
      Nvs successful{baseline};
      Session initial;
      initial.boot(successful, dip);
      REQUIRE_EQ(successful.trace.back(), std::string("cache"));
      auto disk = fixture(legacy);
      Nvs nvs{disk};
      nvs.fail_at = successful.trace.size();
      Session session;
      REQUIRE(session.boot(nvs, dip) == PresetImportResult::Loaded);
      equal(session.presets, bank_values(legacy ? legacy_bank(dip) : dip));
      recover(disk, dip, session.presets);
    }
}

int main() { return tu::run_all(); }
