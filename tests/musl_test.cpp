// A floating tag in a signed-artifact pipeline is the whole attack, so
// musl-build.sh refuses every image without a digest (§3, §11). The stub
// runtime on PATH is run()'s per-call extra_env: the env stays untouched.

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runner.h"
#include "test_env.h"
#include "util/subprocess.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kDigest =
    "ghcr.io/archlinux/archlinux@sha256:"
    "67f575e6ae9319883231be5e32468bb88d4afcfab281b5ac014bd03f0571f3bc";

fs::path repo_root() {
  std::vector<fs::path> bases{
      fs::path(__FILE__).parent_path().parent_path(),
      fs::current_path(),
  };
  for (fs::path base : bases) {
    if (base.empty()) {
      base = fs::current_path();
    }
    if (fs::is_directory(base / "scripts")) {
      // The script's argv[0] must be absolute (util/subprocess.h).
      return fs::absolute(base);
    }
  }
  return {};
}

void write_executable(const fs::path& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
  out.close();
  ::chmod(path.c_str(), 0755);
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

berth::Result<berth::RunOutput> run_script(
    const fs::path& root, const std::vector<std::string>& extra_env) {
  berth::RunLimits limits;
  limits.timeout = std::chrono::milliseconds(30000);
  const std::vector<std::string> argv{root / "scripts" / "musl-build.sh"};
  limits.extra_env = &extra_env;
  return berth::run(argv, limits);
}

}
TEST(musl_build_refuses_a_missing_image) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const std::vector<std::string> env{"CONTAINER=", "CONTAINER_RUNTIME="};
  const berth::Result<berth::RunOutput> result = run_script(root, env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code != 0);
  CHECK(result.value().err.find("CONTAINER is not set") != std::string::npos);
}

TEST(musl_build_refuses_a_floating_tag) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const std::vector<std::string> env{
      "CONTAINER=ghcr.io/archlinux/archlinux:latest",
      "CONTAINER_RUNTIME=/nonexistent/runtime"};
  const berth::Result<berth::RunOutput> result = run_script(root, env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code != 0);
  CHECK(result.value().err.find("never by tag") != std::string::npos);
}

TEST(musl_build_accepts_a_digest_and_streams_a_static_artifact) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path out = tmp.path() / "out";
  fs::create_directories(bin);
  fs::create_directories(out);
  const fs::path log = tmp.path() / "runtime.log";

  // The stub runtime records its argv and emits the artifact on stdout, as
  // the real build writes only the binary there.
  write_executable(bin / "stub-runtime",
                   "#!/bin/sh\n"
                   "printf '%s\\n' \"$*\" >> \"" +
                       log.string() +
                       "\"\n"
                       "printf 'artifact-bytes'\n");

  // `file` must call our stub static; a real `file` would print ASCII text.
  write_executable(
      bin / "file",
      "#!/bin/sh\n"
      "printf '%s\\n' 'ELF 64-bit LSB executable, statically linked'\n");

  const std::vector<std::string> env{
      std::string("CONTAINER=") + kDigest,
      std::string("CONTAINER_RUNTIME=") + (bin / "stub-runtime").string(),
      std::string("MUSL_OUT=") + out.string(),
      "SOURCE_DATE_EPOCH=1234567890",
      std::string("PATH=") + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result = run_script(root, env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(fs::exists(out / "berthpanel"));
  CHECK(read_file(out / "berthpanel") == "artifact-bytes");

  const std::string recorded = read_file(log);
  CHECK(recorded.find(kDigest) != std::string::npos);
  CHECK(recorded.find("/src:ro") != std::string::npos);
  CHECK(recorded.find("SOURCE_DATE_EPOCH=1234567890") != std::string::npos);
  // argv, not a shell string: the runtime is invoked with `run` and `--rm`.
  CHECK(recorded.find("run --rm") != std::string::npos);
}

TEST(musl_build_rejects_a_dynamic_artifact) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path out = tmp.path() / "out";
  fs::create_directories(bin);
  fs::create_directories(out);

  write_executable(bin / "stub-runtime",
                   "#!/bin/sh\n"
                   "printf 'dynamic'\n");
  // A dynamic artifact: `file` says so, and the script must refuse it.
  write_executable(
      bin / "file",
      "#!/bin/sh\n"
      "printf '%s\\n' 'ELF 64-bit LSB pie executable, dynamically linked'\n");

  const std::vector<std::string> env{
      std::string("CONTAINER=") + kDigest,
      std::string("CONTAINER_RUNTIME=") + (bin / "stub-runtime").string(),
      std::string("MUSL_OUT=") + out.string(),
      std::string("PATH=") + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result = run_script(root, env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code != 0);
  CHECK(result.value().err.find("not statically linked") != std::string::npos);
  CHECK(!fs::exists(out / "berthpanel"));
}
