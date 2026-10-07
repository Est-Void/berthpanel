#pragma once

// src/berth/journal.h — deploy-journal.json, the interrupted-deploy
// record (dev.md §6.7). An entry is written when a deploy starts real
// work and removed when the deploy reaches a terminal outcome; cleanup
// treats "entry present AND deploy.lock free" as interrupted. Storage
// goes through json.h: a corrupt journal is quarantined loudly, never
// read as empty (§11). Missing file = no journal = nothing interrupted.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

struct JournalEntry {
  std::string service_name;  // unit stem, validated on load
  std::string work_dir;      // services/<name> dir cleanup removes
  std::string repo;          // owner/repo as deployed
  std::string commit;        // target SHA
  std::string stage;         // cloning | building | registering
  std::int64_t started_at_unix = 0;
};

[[nodiscard]] Result<std::vector<JournalEntry>> journal_load(
    const Paths& paths);

// Upsert by service_name (a redeploy replaces its own entry).
[[nodiscard]] Result<void> journal_upsert(const Paths& paths,
                                          const JournalEntry& entry);

// Idempotent: removing an unknown entry is Ok — cleanup may run twice.
[[nodiscard]] Result<void> journal_remove(const Paths& paths,
                                          std::string_view service_name);

}  // namespace berth
