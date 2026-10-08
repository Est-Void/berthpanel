#include "prefetch.h"

#include <string>

#include "../util/argv.h"
#include "../util/subprocess.h"
#include "../util/which.h"
#include "validate.h"

namespace berth {

Result<void> validate_prefetch_step(const std::vector<std::string>& argv) {
  if (argv.empty())
    return Error("prefetch: empty step");

  const std::vector<std::string> dangerous = matched_dangerous_patterns(argv);
  if (!dangerous.empty()) {
    return Error("prefetch: refused dangerous step (rule " + dangerous.front() +
                 ")");
  }

  const std::string program = program_name(argv);
  const auto has = [&argv](const std::string& needle) {
    return std::find(argv.begin() + 1, argv.end(), needle) != argv.end();
  };

  if (program == "go") {
    if (!(has("mod") && has("download"))) {
      return Error(
          "prefetch: go steps must be 'go mod download' (never build "
          "or run)");
    }
    return {};
  }
  if (program == "python3" || program == "python") {
    if (has("install")) {
      return Error(
          "prefetch: refusing 'pip install' on the host; use "
          "'pip download'");
    }
    if (!has("download")) {
      return Error("prefetch: python steps must download wheels");
    }
    return {};
  }
  if (program == "bun" || program == "npm" || program == "pnpm") {
    if (!has_arg(argv, "--ignore-scripts")) {
      return Error("prefetch: " + program +
                   " must run with --ignore-scripts so no install script "
                   "executes on the host");
    }
    if (program == "pnpm" && !has("fetch") && !has("install")) {
      return Error("prefetch: pnpm step must be fetch or install");
    }
    return {};
  }
  return Error("prefetch: program not on the download-only allowlist: " +
               program);
}

Result<std::size_t> run_prefetch(
    const std::vector<std::vector<std::string>>& steps,
    const std::filesystem::path& work_dir, std::atomic<bool>* cancel,
    const std::chrono::milliseconds per_step_timeout) {
  std::size_t done = 0;
  for (const std::vector<std::string>& step : steps) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      return Error("prefetch: cancelled");
    }
    Result<void> allowed = validate_prefetch_step(step);
    if (allowed.is_err())
      return std::move(allowed).error();

    Result<std::string> binary = resolve_executable(program_name(step));
    if (binary.is_err())
      return std::move(binary).error();

    std::vector<std::string> argv{std::move(binary).value()};
    argv.insert(argv.end(), step.begin() + 1, step.end());

    RunLimits limits;
    limits.timeout = per_step_timeout;
    limits.cancel = cancel;
    // The checkout, so a manifest-reading step reads the USER's manifest.
    limits.cwd = &work_dir;
    Result<RunOutput> out = run(argv, limits);
    if (out.is_err())
      return std::move(out).error();
    if (out.value().exit_code != 0) {
      return Error("prefetch: " + program_name(step) + " failed with exit " +
                   std::to_string(out.value().exit_code) + ": " +
                   out.value().err);
    }
    ++done;
  }
  return done;
}

std::string render_prefetch_report(
    const std::vector<std::vector<std::string>>& steps,
    const std::vector<std::string>& failures) {
  std::string out =
      "prefetch: " + std::to_string(failures.empty() ? steps.size() : 0) + "/" +
      std::to_string(steps.size()) + " steps ran\n";
  for (const std::string& failure : failures) {
    out += "  failed: " + failure + "\n";
  }
  return out;
}

}  // namespace berth