#pragma once

// An entry lives while a deploy does real work; cleanup reads "present AND lock
// free" as interrupted. A corrupt journal is quarantined, never read as empty.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

struct JournalEntry {
  std::string service_name;
  std::string work_dir;
  std::string repo;
  std::string commit;
  std::string stage;
  std::int64_t started_at_unix = 0;
  // Non-empty for a stack member: the group stem. The key stays service_name,
  // so each member is its own entry; the tag groups them for cleanup.
  std::string stack;
};

[[nodiscard]] Result<std::vector<JournalEntry>> journal_load(
    const Paths& paths);

// Upsert by service_name (a redeploy replaces its own entry).
[[nodiscard]] Result<void> journal_upsert(const Paths& paths,
                                          const JournalEntry& entry);

// One store for the whole group: a stack writes all member entries before the
// first member runs, so an interruption reconciles every member.
[[nodiscard]] Result<void> journal_upsert_many(
    const Paths& paths, const std::vector<JournalEntry>& entries);

// Idempotent: removing an unknown entry is Ok — cleanup may run twice.
[[nodiscard]] Result<void> journal_remove(const Paths& paths,
                                          std::string_view service_name);

}
