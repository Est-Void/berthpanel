// Layering guard (dev.md §5): the engine must never depend on the panel.
// `berth/`, `core/` and `analyzer/` are linked into the CLI and the test binary
// without FTXUI, so a single `#include "tui/..."` or an FTXUI header there
// breaks the boundary the whole split exists for. The old engine called
// `tui::workers::append_deploy_log` directly; this test makes that class of
// regression fail the build instead of a code review.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runner.h"

namespace {

namespace fs = std::filesystem;

bool is_forbidden_include(const std::string& line) {
  const std::size_t hash = line.find('#');
  if (hash == std::string::npos) {
    return false;
  }
  const std::size_t inc = line.find("include", hash);
  if (inc == std::string::npos) {
    return false;
  }
  // Only the include specifier after `include` counts; a comment that merely
  // mentions tui/ is prose, not a dependency.
  return line.find("tui/", inc) != std::string::npos ||
         line.find("ftxui", inc) != std::string::npos;
}

bool is_engine_source(const fs::path& p) {
  const std::string ext = p.extension().string();
  return ext == ".cpp" || ext == ".h" || ext == ".hpp";
}

}  // namespace

TEST(engine_sources_never_include_the_tui_or_ftxui) {
  // Resolve the repo root from this file's path first (the Makefile compiles
  // with a repo-relative path), then from the working directory (`make test`
  // runs at the root). If neither locates src/berth, FAIL rather than pass: a
  // guard that cannot fail is not a guard (§11).
  std::vector<fs::path> bases{
      fs::path(__FILE__).parent_path().parent_path(),
      fs::current_path(),
  };
  fs::path root;
  for (fs::path base : bases) {
    if (base.empty()) {
      base = fs::current_path();
    }
    if (fs::is_directory(base / "src" / "berth")) {
      root = base;
      break;
    }
  }
  CHECK(!root.empty());

  std::vector<std::string> offenders;
  for (const char* top : {"berth", "core", "analyzer"}) {
    const fs::path dir = root / "src" / top;
    if (!fs::is_directory(dir)) {
      continue;
    }
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(dir)) {
      if (!entry.is_regular_file() || !is_engine_source(entry.path())) {
        continue;
      }
      std::ifstream in(entry.path());
      if (!in) {
        offenders.push_back(entry.path().string() + ": cannot open");
        continue;
      }
      std::string line;
      std::size_t number = 0;
      while (std::getline(in, line)) {
        ++number;
        if (is_forbidden_include(line)) {
          offenders.push_back(entry.path().string() + ":" +
                              std::to_string(number) + ": " + line);
        }
      }
    }
  }

  if (!offenders.empty()) {
    std::string message = "layering violated: engine includes tui/ftxui:\n";
    for (const std::string& offender : offenders) {
      message += "  " + offender + "\n";
    }
    throw std::runtime_error(message);
  }
}
