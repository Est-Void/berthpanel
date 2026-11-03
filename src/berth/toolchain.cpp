#include "toolchain.h"

#include <utility>

#include "../core/atomic.h"
#include "../util/fs.h"
#include "../util/strutil.h"
#include "toolbox.h"

namespace berth {

std::optional<ToolchainPin> pinned_toolchain(const std::string_view name) {
  if (name != "pnpm") {
    return std::nullopt;
  }
#if defined(__x86_64__)
  return ToolchainPin{
      .name = "pnpm",
      .version = "11.7.0",
      .url =
          "https://github.com/pnpm/pnpm/releases/download/v11.7.0/"
          "pnpm-linux-x64.tar.gz",
      .sha256 =
          "752e31654c6f24bc945db784d12831b80b29f533b43105575ce30dd3c8658609",
      .inner_path = "pnpm",
  };
#elif defined(__aarch64__)
  // No verified sha256 for the arm64 asset yet: pinning a guess is worse than
  // blocking honestly.
  return std::nullopt;
#else
  return std::nullopt;
#endif
}

std::filesystem::path toolbox_bin_dir(const Paths& paths) {
  return paths.state_dir / "toolbox" / "bin";
}

std::string path_with_toolbox(const Paths& paths,
                              const std::string_view base_path) {
  return toolbox_bin_dir(paths).string() + ":" + std::string(base_path);
}

bool pin_is_current(const std::filesystem::path& version_marker,
                    const std::string_view version) {
  const Result<std::string> marker = read_file(version_marker);
  if (marker.is_err()) {
    return false;
  }
  return trim(marker.value()) == version;
}

Result<std::filesystem::path> ensure_toolchain(const std::string_view name,
                                               const Paths& paths,
                                               std::atomic<bool>* cancel) {
  const std::optional<ToolchainPin> pin = pinned_toolchain(name);
  if (!pin.has_value()) {
    return Error("toolchain: no pinned build for '" + std::string(name) +
                 "' on this host");
  }
  const std::filesystem::path bin_dir = toolbox_bin_dir(paths);
  const std::filesystem::path binary = bin_dir / pin->name;
  const std::filesystem::path marker = bin_dir / (pin->name + ".version");
  if (pin_is_current(marker, pin->version) && is_executable(binary)) {
    return binary;
  }

  const PinnedTool tool{.name = pin->name,
                        .url = pin->url,
                        .sha256 = pin->sha256,
                        .inner_path = pin->inner_path,
                        .install_dir = bin_dir,
                        .executable = true,
                        .archive = ToolArchive::kTarGz,
                        .extract_archive = true};
  Result<std::filesystem::path> installed =
      install_pinned_tool(tool, paths, cancel);
  if (installed.is_err()) {
    return with_context(std::move(installed).error(),
                        "toolchain: installing " + pin->name + " " +
                            pin->version);
  }
  Result<void> wrote =
      atomic_write(marker, pin->version + "\n", kPermShared);
  if (wrote.is_err()) {
    return with_context(std::move(wrote).error(),
                        "toolchain: recording " + marker.string());
  }
  return installed.value();
}

Result<std::vector<ToolStatus>> resolve_tools(
    const std::vector<std::string>& names, const Paths& paths,
    std::atomic<bool>* cancel) {
  Result<std::vector<ToolStatus>> tools = check_tools(names, cancel);
  if (tools.is_err()) {
    return std::move(tools).error();
  }
  std::vector<ToolStatus> statuses = std::move(tools).value();
  for (ToolStatus& status : statuses) {
    if (status.present) {
      continue;
    }
    const std::optional<ToolchainPin> pin = pinned_toolchain(status.name);
    if (!pin.has_value()) {
      continue;  // preflight blocks with the missing-tool detail
    }
    Result<std::filesystem::path> ensured =
        ensure_toolchain(status.name, paths, cancel);
    if (ensured.is_err()) {
      return with_context(std::move(ensured).error(),
                          "toolchain: resolving " + status.name);
    }
    status.present = true;
    status.path = ensured.value().string();
    status.version = pin->version;
  }
  return statuses;
}

}
