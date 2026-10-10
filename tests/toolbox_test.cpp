#include "berth/toolbox.h"

#include <string>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"
#include "util/sha256.h"

using berth::install_pinned_tool;
using berth::verify_sha256_file;

TEST(toolbox_verify_accepts_matching_hash) {
  const TempDir tmp;
  const auto file = tmp.path() / "tool.bin";
  const std::string payload = "pretend go toolchain";
  CHECK(berth::atomic_write(file, payload).is_ok());
  const std::string sha = berth::sha256_hex(payload);
  CHECK(verify_sha256_file(file, sha).is_ok());
  CHECK(verify_sha256_file(file, "  " + sha + " ").is_ok());  // trimmed
}

TEST(toolbox_verify_rejects_mismatch_and_bad_pins) {
  const TempDir tmp;
  const auto file = tmp.path() / "tool.bin";
  CHECK(berth::atomic_write(file, "real bytes").is_ok());

  const berth::Result<void> mismatch =
      verify_sha256_file(file, berth::sha256_hex("other bytes"));
  CHECK(mismatch.is_err());
  CHECK(mismatch.error().ctx().find("mismatch") != std::string::npos);

  CHECK(verify_sha256_file(file, "deadbeef").is_err());
  CHECK(verify_sha256_file(tmp.path() / "absent", berth::sha256_hex("x"))
            .is_err());
}

TEST(toolbox_install_refuses_without_pin) {
  const TempDir tmp;
  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  const berth::Result<std::filesystem::path> installed = install_pinned_tool(
      {.name = "go",
       .url = "https://dl.google.com/go/go1.24.0.linux-amd64.tar.gz",
       .sha256 = "",
       .inner_path = "go/bin/go",
       .install_dir = tmp.path() / "tools"},
      paths, nullptr);
  CHECK(installed.is_err());
  CHECK(installed.error().ctx().find("sha256") != std::string::npos);
}

TEST(toolbox_install_refuses_a_traversing_member_before_any_download) {
  // `member` becomes install_dir/member, so a user recipe must not be able to
  // aim it outside install_dir. The refusal happens before the network call,
  // so this is offline.
  const TempDir tmp;
  berth::Paths paths;
  paths.state_dir = tmp.path() / "state";
  const berth::Result<std::filesystem::path> installed =
      install_pinned_tool({.name = "evil",
                           .url = "https://dl.google.com/evil.tar.gz",
                           .sha256 = std::string(64, 'a'),
                           .inner_path = "../../escape.service",
                           .install_dir = tmp.path() / "tools"},
                          paths, nullptr);
  CHECK(installed.is_err());
  CHECK(installed.error().ctx().find("member") != std::string::npos);
  CHECK(!std::filesystem::exists(tmp.path() / "escape.service"));
}
