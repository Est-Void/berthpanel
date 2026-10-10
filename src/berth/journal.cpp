// src/berth/journal.cpp — see journal.h for the contract.

#include "journal.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/docstore.h"
#include "core/names.h"

namespace berth {

namespace {

constexpr int kJournalVersion = 1;

constexpr const char* kKnownStages[] = {"cloning", "building", "registering"};

[[noreturn]] void schema_fail(const std::string& why) {
  throw nlohmann::json::out_of_range::create(404, why, nullptr);
}

bool stage_is_known(const std::string& stage) {
  for (const char* known : kKnownStages) {
    if (stage == known)
      return true;
  }
  return false;
}

nlohmann::json entry_to_json(const JournalEntry& e) {
  return nlohmann::json{{"service_name", e.service_name},
                        {"work_dir", e.work_dir},
                        {"repo", e.repo},
                        {"commit", e.commit},
                        {"stage", e.stage},
                        {"started_at_unix", e.started_at_unix}};
}

// Every value that later reaches a path or a systemctl argument is validated
// HERE, at load: a hand-edited journal must not be able to steer a delete
// outside the services layout (§11).
JournalEntry entry_from_json(const nlohmann::json& j) {
  JournalEntry e;
  e.service_name = j.at("service_name").get<std::string>();
  e.work_dir = j.at("work_dir").get<std::string>();
  e.repo = j.at("repo").get<std::string>();
  e.commit = j.at("commit").get<std::string>();
  e.stage = j.at("stage").get<std::string>();
  e.started_at_unix = j.at("started_at_unix").get<std::int64_t>();
  if (!validate_unit_name(e.service_name)) {
    schema_fail("invalid service_name in journal: " + e.service_name);
  }
  if (e.work_dir.empty() || e.work_dir.front() != '/') {
    schema_fail("work_dir must be absolute: " + e.work_dir);
  }
  if (!stage_is_known(e.stage)) {
    schema_fail("unknown journal stage: " + e.stage);
  }
  if (e.started_at_unix < 0) {
    schema_fail("negative started_at_unix");
  }
  return e;
}

std::string entry_key(const JournalEntry& e) {
  return e.service_name;
}

DocStore<JournalEntry> store_for(const Paths& paths) {
  return DocStore<JournalEntry>(paths.deploy_journal, paths.state_dir,
                                kJournalVersion, "entries", entry_to_json,
                                entry_from_json, entry_key);
}

}  // namespace

Result<std::vector<JournalEntry>> journal_load(const Paths& paths) {
  Result<std::vector<JournalEntry>> loaded = store_for(paths).load();
  if (loaded.is_err()) {
    return with_context(std::move(loaded).error(), "journal_load");
  }
  return loaded;
}

Result<void> journal_upsert(const Paths& paths, const JournalEntry& entry) {
  if (!validate_unit_name(entry.service_name)) {
    return Error("journal_upsert: invalid service name: '" +
                 entry.service_name + "'");
  }
  const Result<void> written = store_for(paths).upsert(entry);
  if (written.is_err())
    return written;
  return {};
}

Result<void> journal_remove(const Paths& paths,
                            const std::string_view service_name) {
  const Result<void> erased = store_for(paths).remove(service_name);
  if (erased.is_err())
    return erased;
  return {};
}

}  // namespace berth
