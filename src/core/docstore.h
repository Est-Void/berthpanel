#pragma once

// src/core/docstore.h — the shape every versioned on-disk document has: a
// JSON object {"version": N, "<array_field>": [ … ]} of records keyed by a
// string field. state.json and deploy-journal.json are the same machinery
// with different records, so load/quarantine/sort/upsert/remove exists once
// here instead of twice in near-identical files.
//
// Corrupt input is quarantined and reported, NEVER read as an empty document
// (§11): an empty registry would make the panel forget every service it
// manages. Header-only because it is a template: instantiating it in a .cpp
// would drag the record types into this module.

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/json.h"
#include "util/fs.h"
#include "util/result.h"

namespace berth {

namespace detail {

// A corrupt-document failure reports both what was wrong and what happened
// to the bad bytes. A failed backup is reported too: losing the diagnosis
// and the file would be worse than either.
[[nodiscard]] inline Error docstore_quarantine(
    const std::filesystem::path& file, const std::string& why) {
  const Result<std::filesystem::path> backup = quarantine_corrupt(file);
  std::string msg = "docstore: " + why + " in " + file.string();
  if (backup.is_ok()) {
    msg += "; backed up to " + backup.value().string();
  } else {
    msg += "; AND the backup failed: " + backup.error().ctx();
  }
  return Error(std::move(msg));
}

[[nodiscard]] inline bool docstore_version_is(const nlohmann::json& doc,
                                              const int version) {
  if (!doc.is_object() || !doc.contains("version"))
    return false;
  if (!doc["version"].is_number_integer())
    return false;
  return doc["version"].get<int>() == version;
}

[[nodiscard]] inline std::string docstore_version_problem(
    const nlohmann::json& doc) {
  if (doc.is_object() && doc.contains("version")) {
    return "unsupported document version " + doc["version"].dump();
  }
  return "not a versioned document";
}

}  // namespace detail

template <typename T>
class DocStore {
 public:
  using Encode = std::function<nlohmann::json(const T&)>;
  // Decode signals a schema violation by throwing a nlohmann exception; the
  // store turns that into a quarantine plus a loud error, never a default.
  using Decode = std::function<T(const nlohmann::json&)>;
  using KeyOf = std::function<std::string(const T&)>;

  // `dir` is created on the first store: a deploy into a fresh XDG state dir
  // must not need a separate ensure_dir call at every write site.
  DocStore(std::filesystem::path file, std::filesystem::path dir,
           const int version, std::string_view array_field, Encode encode,
           Decode decode, KeyOf key_of)
      : file_(std::move(file)),
        dir_(std::move(dir)),
        version_(version),
        array_field_(array_field),
        encode_(std::move(encode)),
        decode_(std::move(decode)),
        key_of_(std::move(key_of)) {}

  [[nodiscard]] const std::filesystem::path& file() const { return file_; }

  // Records sorted by key. A missing file is an empty list, not an error.
  [[nodiscard]] Result<std::vector<T>> load() const {
    std::error_code ec;
    const bool exists = std::filesystem::exists(file_, ec);
    if (ec) {
      return Error("docstore: stat " + file_.string() + ": " + ec.message());
    }
    if (!exists)
      return std::vector<T>{};

    Result<nlohmann::json> loaded = json_load(file_);
    if (loaded.is_err())
      return std::move(loaded).error();  // quarantined

    const nlohmann::json& doc = loaded.value();
    if (!detail::docstore_version_is(doc, version_)) {
      return detail::docstore_quarantine(file_,
                                         detail::docstore_version_problem(doc));
    }

    try {
      const nlohmann::json& array = doc.at(array_field_);
      if (!array.is_array()) {
        throw nlohmann::json::type_error::create(
            302, std::string(array_field_) + " is not an array", nullptr);
      }
      std::vector<T> records;
      records.reserve(array.size());
      for (const nlohmann::json& item : array) {
        records.push_back(decode_(item));
      }
      sort_by_key(&records);
      return records;
    } catch (const nlohmann::json::exception& e) {
      return detail::docstore_quarantine(file_, e.what());
    }
  }

  [[nodiscard]] Result<void> store(const std::vector<T>& records) const {
    const Result<void> dir = ensure_dir(dir_);
    if (dir.is_err())
      return dir;

    std::vector<T> sorted = records;
    sort_by_key(&sorted);
    nlohmann::json array = nlohmann::json::array();
    for (const T& record : sorted) {
      array.push_back(encode_(record));
    }
    const nlohmann::json doc{{"version", version_},
                             {std::string(array_field_), array}};
    return json_store(file_, doc);
  }

  // Replaces the record with a matching key, appends otherwise.
  [[nodiscard]] Result<void> upsert(const T& record) const {
    Result<std::vector<T>> loaded = load();
    if (loaded.is_err())
      return std::move(loaded).error();
    std::vector<T> records = std::move(loaded).value();
    replace_matching(&records, record);
    return store(records);
  }

  // Idempotent: removing an absent key succeeds, because cleanup and retry
  // paths call this more than once.
  [[nodiscard]] Result<void> remove(const std::string_view key) const {
    Result<std::vector<T>> loaded = load();
    if (loaded.is_err())
      return std::move(loaded).error();
    std::vector<T> records = std::move(loaded).value();
    erase_matching(&records, key);
    return store(records);
  }

 private:
  void sort_by_key(std::vector<T>* records) const {
    std::sort(records->begin(), records->end(), [this](const T& a, const T& b) {
      return key_of_(a) < key_of_(b);
    });
  }

  void replace_matching(std::vector<T>* records, const T& record) const {
    const std::string key = key_of_(record);
    for (T& existing : *records) {
      if (key_of_(existing) != key)
        continue;
      existing = record;
      return;
    }
    records->push_back(record);
  }

  void erase_matching(std::vector<T>* records,
                      const std::string_view key) const {
    const auto it =
        std::find_if(records->begin(), records->end(),
                     [this, key](const T& r) { return key_of_(r) == key; });
    if (it != records->end())
      records->erase(it);
  }

  std::filesystem::path file_;
  std::filesystem::path dir_;
  int version_;
  std::string array_field_;
  Encode encode_;
  Decode decode_;
  KeyOf key_of_;
};

}  // namespace berth
