#include "berth/toolcheck.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"

using berth::check_tools;
using berth::render_tool_report;

namespace {

void put_tool(const std::filesystem::path& dir, const std::string& name,
              const std::string& version_output) {
  CHECK(berth::ensure_dir(dir).is_ok());
  const std::filesystem::path file = dir / name;
  const std::string script =
      "#!/bin/sh\n" +
      (version_output.empty() ? std::string("echo \"\"")
                              : "echo '" + version_output + "'") +
      "\n";
  FILE* f = std::fopen(file.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs(script.c_str(), f);
  std::fclose(f);
  CHECK(::chmod(file.c_str(), 0755) == 0);
}

}  // namespace

TEST(toolcheck_reports_present_and_missing) {
  const TempDir tmp;
  put_tool(tmp.path() / "bin", "bun", "1.1.20");
  put_tool(tmp.path() / "bin", "go", "go version go1.24.0 linux/amd64");

  std::lock_guard lock(berth::env_test_lock());
  ScopedEnv path("PATH");
  path.set((tmp.path() / "bin").string());

  const auto tools = check_tools({"bun", "go", "pnpm"}, nullptr);
  CHECK(tools.is_ok());
  CHECK(tools.value().size() == 3);

  CHECK(tools.value()[0].present);
  CHECK(tools.value()[0].version == "1.1.20");
  CHECK(tools.value()[1].present);
  CHECK(tools.value()[1].version.find("go1.24.0") != std::string::npos);
  CHECK(!tools.value()[2].present);
}

TEST(toolcheck_catalog_tools_are_the_four_build_tools) {
  const std::vector<std::string> tools = berth::catalog_tools();
  CHECK(tools.size() == 4);
  CHECK(std::find(tools.begin(), tools.end(), "bun") != tools.end());
  CHECK(std::find(tools.begin(), tools.end(), "pnpm") != tools.end());
  CHECK(std::find(tools.begin(), tools.end(), "go") != tools.end());
  CHECK(std::find(tools.begin(), tools.end(), "python3") != tools.end());
}

TEST(toolcheck_report_marks_missing) {
  const std::vector<berth::ToolStatus> tools = {
      {.name = "go",
       .present = true,
       .path = "/usr/bin/go",
       .version = "go1.24"},
      {.name = "bun", .present = false, .path = "", .version = ""},
  };
  const std::string report = render_tool_report(tools);
  CHECK(report.find("ok   go") != std::string::npos);
  CHECK(report.find("MISS bun") != std::string::npos);
}
