#include "berth/toolcheck.h"

#include <chrono>
#include <string>

#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/which.h"

namespace berth {

namespace {

constexpr std::chrono::milliseconds kVersionTimeout{5000};

std::string first_version_line(const std::string& text) {
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t eol = text.find('\n', start);
    const std::string line = text.substr(
        start, eol == std::string::npos ? std::string::npos : eol - start);
    if (line.find_first_of("0123456789") != std::string::npos) {
      return std::string(trim(line));
    }
    if (eol == std::string::npos)
      break;
    start = eol + 1;
  }
  return {};
}

}  // namespace

Result<std::vector<ToolStatus>> check_tools(
    const std::vector<std::string>& names, std::atomic<bool>* cancel) {
  std::vector<ToolStatus> out;
  for (const std::string& name : names) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error("toolcheck: cancelled");
    }
    ToolStatus status;
    status.name = name;
    const Result<std::string> path = resolve_executable(name);
    if (path.is_ok()) {
      status.present = true;
      status.path = std::move(path).value();
      RunLimits limits;
      limits.timeout = kVersionTimeout;
      limits.cancel = cancel;
      const Result<RunOutput> version = run({status.path, "--version"}, limits);
      if (version.is_ok() && version.value().exit_code == 0) {
        status.version =
            first_version_line(version.value().out + version.value().err);
      }
    }
    out.push_back(std::move(status));
  }
  return out;
}

std::vector<std::string> catalog_tools() {
  return {"bun", "pnpm", "go", "python3"};
}

std::string render_tool_report(const std::vector<ToolStatus>& tools) {
  std::string out;
  for (const ToolStatus& tool : tools) {
    out += tool.present ? "  ok   " : "  MISS ";
    out += tool.name;
    if (!tool.version.empty()) {
      out += "  ";
      out += tool.version;
    }
    out += "\n";
  }
  return out;
}

}  // namespace berth
