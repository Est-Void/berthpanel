#pragma once

// A download is verified BEFORE it is installed and only a verified artifact
// is renamed into place (§11). Allowlisting is enforced by httpclient.

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "../core/paths.h"
#include "../util/result.h"

namespace berth {

enum class ToolArchive {
  kRaw,    // the body IS the file
  kTarGz,  // a .tar.gz; `inner_path` is the member to extract
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

[[nodiscard]] Result<std::filesystem::path> download_verified(
    const std::string& url, const std::string& expected_sha256,
    const std::filesystem::path& staging_dir, const Paths& paths,
    std::atomic<bool>* cancel);

[[nodiscard]] Result<std::filesystem::path> install_pinned_tool(
    const PinnedTool& tool, const Paths& paths, std::atomic<bool>* cancel);

}
