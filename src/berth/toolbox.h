#pragma once

// src/berth/toolbox.h — pinned build-tool downloads with SHA-256
// verification. The download is verified BEFORE it is installed, into a
// staging file, and only a verified artifact is renamed into place — a
// truncated file must never be mistaken for a complete one (dev.md §11).
// Host allowlisting is enforced by httpclient; a non-allowlisted URL
// fails before any socket opens.

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "../paths.h"
#include "../util/result.h"

namespace berth {

enum class ToolArchive {
  kRaw,     // the body IS the file
  kTarGz,   // a .tar.gz; `inner_path` is the member to extract
};

struct PinnedTool {
  std::string name;
  std::string url;
  std::string sha256;
  std::string inner_path;  // member inside the archive, e.g. "go/bin/go"
  std::filesystem::path install_dir;  // where the tool lands
  bool executable = true;
  ToolArchive archive = ToolArchive::kTarGz;
};

[[nodiscard]] Result<void> verify_sha256_file(
    const std::filesystem::path& path, const std::string& expected_sha256);

// Downloads into staging_dir/download.tmp, verifies, and returns the path
// of the verified file. A hash mismatch removes the staged file.
[[nodiscard]] Result<std::filesystem::path> download_verified(
    const std::string& url, const std::string& expected_sha256,
    const std::filesystem::path& staging_dir, const Paths& paths,
    std::atomic<bool>* cancel);

// Full install: download → verify → place at install_dir/inner_path,
// atomically (staged then renamed). Never installs unverified bytes.
[[nodiscard]] Result<std::filesystem::path> install_pinned_tool(
    const PinnedTool& tool, const Paths& paths, std::atomic<bool>* cancel);

}  // namespace berth
