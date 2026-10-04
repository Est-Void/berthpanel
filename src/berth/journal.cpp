// src/berth/journal.cpp — see journal.h for the contract. Schema mapping
// is explicit (no nlohmann ADL): validation stays visible, §11.

#include "journal.h"

#include <algorithm>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../atomic.h"
#include "../json.h"
#include "../state.h"

namespace berth {

namespace {
constexpr int kJournalVersion = 1;

[[noreturn]] void schema_fail(const std::string& why) {
  throw nlohmann::json::out_of_range::create(404, why, nullptr);
}

nlohmann::json entry_to_json(const JournalEntry& e) {
  return nlohmann::json{{"service_name", e.service_name},
                        {"work_dir", e.work_dir},
                        {"repo", e.repo},
                        {"commit", e.commit},
                        {"stage", e.stage},
                        {"started_at_unix", e.started_at_unix}};
}

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
  if (e.stage != "cloning" && e.stage != "building" &&
      e.stage != "registering") {
    schema_fail("unknown journal stage: " + e.stage);
  }
  if (e.started_at_unix < 0) {
    schema_fail("negative started_at_unix");
  }
  return e;
}

nlohmann::json entries_to_json(const std::vector<JournalEntry>& entries) {
  nlohmann::json array = nlohmann::json::array();
  for (const JournalEntry& e : entries) {
    array.push_back(entry_to_json(e));
  }
  return nlohmann::json{{"version", kJournalVersion}, {"entries", array}};
}

std::vector<JournalEntry> entries_from_json(const nlohmann::json& doc) {
  const nlohmann::json& entries = doc.at("entries");
  if (!entries.is_array()) {
    throw nlohmann::json::type_error::create(302, "entries is not an array",
                                             nullptr);
  }
  std::vector<JournalEntry> out;
  out.reserve(entries.size());
  for (const nlohmann::json& item : entries) {
    out.push_back(entry_from_json(item));
  }
  return out;
}

Result<void> journal_store(const Paths& paths,
                           const std::vector<JournalEntry>& entries) {
  Result<void> dir = ensure_dir(paths.state_dir);
  if (dir.is_err()) return dir;
  return json_store(paths.deploy_journal,
                    entries_to_json(entries));  // atomic 0600, pretty
}

}  // namespace

Result<std::vector<JournalEntry>> journal_load(const Paths& paths) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(paths.deploy_journal, ec);
  if (ec) {
    return Error("journal_load: stat " + paths.deploy_journal.string() + ": " +
                 ec.message());
  }
  if (!exists) return std::vector<JournalEntry>{};

  Result<nlohmann::json> loaded = json_load(paths.deploy_journal);
  if (loaded.is_err()) return std::move(loaded).error();  // quarantined

  const auto schema_error = [&](const std::string& why) -> Result<std::vector<JournalEntry>> {
    Result<std::filesystem::path> backup =
        quarantine_corrupt(paths.deploy_journal);
    std::string msg = "journal_load: " + why + " in " +
                      paths.deploy_journal.string();
    if (backup.is_ok()) {
      msg += "; backed up to " + backup.value().string();
    } else {
      msg += "; AND the backup failed: " + backup.error().ctx();
    }
    return Error(std::move(msg));
  };

  const nlohmann::json& doc = loaded.value();
  if (!doc.is_object() || !doc.contains("version") ||
      !doc["version"].is_number_integer()) {
    return schema_error("not a versioned journal object");
  }
  if (doc["version"].get<int>() != kJournalVersion) {
    return schema_error("unsupported journal version " + doc["version"].dump());
  }
  try {
    std::vector<JournalEntry> entries = entries_from_json(doc);
    std::sort(entries.begin(), entries.end(),
              [](const JournalEntry& a, const JournalEntry& b) {
                return a.service_name < b.service_name;
              });
    return entries;
  } catch (const nlohmann::json::exception& e) {
    return schema_error(e.what());
  }
}

Result<void> journal_upsert(const Paths& paths, const JournalEntry& entry) {
  if (!validate_unit_name(entry.service_name)) {
    return Error("journal_upsert: invalid service name: '" +
                 entry.service_name + "'");
  }
  Result<std::vector<JournalEntry>> loaded = journal_load(paths);
  if (loaded.is_err()) return std::move(loaded).error();
  std::vector<JournalEntry> entries = std::move(loaded).value();
  bool replaced = false;
  for (JournalEntry& existing : entries) {
    if (existing.service_name == entry.service_name) {
      existing = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) entries.push_back(entry);
  return journal_store(paths, entries);
}

Result<void> journal_remove(const Paths& paths,
                            const std::string_view service_name) {
  Result<std::vector<JournalEntry>> loaded = journal_load(paths);
  if (loaded.is_err()) return std::move(loaded).error();
  std::vector<JournalEntry> entries = std::move(loaded).value();
  const auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const JournalEntry& e) {
                                 return e.service_name == service_name;
                               });
  if (it == entries.end()) return {};  // idempotent by contract
  entries.erase(it);
  return journal_store(paths, entries);
}

}  // namespace berth
