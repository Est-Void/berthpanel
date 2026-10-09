// Tests for install.sh (stage 8.4): the installer/uninstaller's safety
// properties, asserted offline with a stub minisign and a stub systemctl —
// the shim pattern cancel_test established. No real key, no live user
// manager, and no process-wide environment is mutated (PATH is per-call via
// run()'s extra_env).
//
// Covered:
//   * verify-first, fail-closed: a bad signature or a checksum mismatch
//     installs nothing;
//   * --allow-checksum-only is the explicit opt-out of the signature step;
//   * a good signature installs the binary into <prefix>/bin;
//   * the uninstaller stops/removes only `berthpanel-` units and unit files,
//     never anything else, and is idempotent.

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runner.h"
#include "test_env.h"
#include "util/sha256.h"
#include "util/subprocess.h"

namespace {

namespace fs = std::filesystem;

fs::path repo_root() {
  std::vector<fs::path> bases{
      fs::path(__FILE__).parent_path().parent_path(),
      fs::current_path(),
  };
  for (fs::path base : bases) {
    if (base.empty()) {
      base = fs::current_path();
    }
    if (fs::is_regular_file(base / "install.sh")) {
      return fs::absolute(base);
    }
  }
  return {};
}

void write_text(const fs::path& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

void write_executable(const fs::path& path, const std::string& body) {
  write_text(path, body);
  ::chmod(path.c_str(), 0755);
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

int mode_of(const fs::path& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return -1;
  }
  return static_cast<int>(st.st_mode & 0777);
}

berth::Result<berth::RunOutput> run_cmd(
    const std::vector<std::string>& argv,
    const std::vector<std::string>& extra_env) {
  berth::RunLimits limits;
  limits.timeout = std::chrono::milliseconds(30000);
  limits.extra_env = &extra_env;
  return berth::run(argv, limits);
}

// A source directory with a well-formed release layout. `correct_checksum`
// controls whether SHA256SUMS matches the artifact.
fs::path make_source(const fs::path& dir, const std::string& bytes,
                     bool correct_checksum) {
  fs::create_directories(dir);
  write_text(dir / "berthpanel", bytes);
  const std::string hash =
      correct_checksum ? berth::sha256_hex(bytes) : std::string(64, '0');
  write_text(dir / "SHA256SUMS", hash + "  berthpanel\n");
  write_text(dir / "SHA256SUMS.minisig",
             "untrusted comment: signature\nfake\n");
  write_text(dir / "release.pub",
             "untrusted comment: minisign public key: X\nfake\n");
  return dir;
}

}  // namespace

TEST(installer_refuses_a_bad_signature_and_installs_nothing) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  fs::create_directories(bin);
  // A minisign that rejects every signature.
  write_executable(bin / "minisign", "#!/bin/sh\nexit 1\n");

  const std::vector<std::string> env{
      "BERTHPANEL_MINISIGN=" + (bin / "minisign").string(),
      "PATH=" + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "install.sh", "--source", src.string(), "--prefix",
               prefix.string(), "--pub", (src / "release.pub").string()},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code != 0);
  CHECK(result.value().err.find("signature verification FAILED") !=
        std::string::npos);
  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));
}

TEST(installer_installs_after_verification) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  fs::create_directories(bin);
  write_executable(bin / "minisign", "#!/bin/sh\nexit 0\n");

  const std::vector<std::string> env{
      "BERTHPANEL_MINISIGN=" + (bin / "minisign").string(),
      "PATH=" + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "install.sh", "--source", src.string(), "--prefix",
               prefix.string(), "--pub", (src / "release.pub").string()},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(read_file(prefix / "bin" / "berthpanel") == "BIN");
  CHECK(mode_of(prefix / "bin" / "berthpanel") == 0755);
}

TEST(installer_rejects_a_checksum_mismatch) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path src = make_source(tmp.path() / "src", "BIN", false);
  const fs::path prefix = tmp.path() / "prefix";

  const std::vector<std::string> env{"PATH=/usr/bin:/bin"};
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "install.sh", "--source", src.string(), "--prefix",
               prefix.string(), "--allow-checksum-only"},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code != 0);
  CHECK(result.value().err.find("checksum verification FAILED") !=
        std::string::npos);
  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));
}

TEST(installer_allow_checksum_only_skips_the_signature) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  // Even with no minisign anywhere, the explicit opt-out installs.
  fs::remove(src / "SHA256SUMS.minisig");

  const std::vector<std::string> env{"PATH=/usr/bin:/bin"};
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "install.sh", "--source", src.string(), "--prefix",
               prefix.string(), "--allow-checksum-only"},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(read_file(prefix / "bin" / "berthpanel") == "BIN");
}

TEST(uninstaller_only_touches_the_prefix) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path prefix = tmp.path() / "prefix";
  const fs::path cfg = tmp.path() / "cfg";
  fs::create_directories(bin);
  fs::create_directories(prefix / "bin");
  fs::create_directories(cfg / "systemd" / "user");
  const fs::path log = tmp.path() / "systemctl.log";

  write_text(prefix / "bin" / "berthpanel", "BIN");
  // Unit files on disk, only some of them ours.
  write_text(cfg / "systemd" / "user" / "berthpanel-memos.service", "u");
  write_text(cfg / "systemd" / "user" / "other.service", "u");

  write_executable(
      bin / "systemctl",
      "#!/bin/sh\n"
      "printf '%s\\n' \"$*\" >> \"" +
          log.string() +
          "\"\n"
          "cmd=$1\n"
          "case \"$cmd\" in --*) shift; cmd=$1 ;; esac\n"
          "shift\n"
          "case \"$cmd\" in\n"
          "  list-units)\n"
          "    printf '%s\\n' \\\n"
          "      'berthpanel-memos.service loaded active running memos' \\\n"
          "      'berthpanel-cleanup.timer loaded active waiting x' \\\n"
          "      'other.service loaded active running y' \\\n"
          "      'demo-x.service loaded active running z' ;;\n"
          "  list-unit-files)\n"
          "    printf '%s\\n' 'berthpanel-memos.service enabled' "
          "'other.service enabled' ;;\n"
          "esac\n"
          "exit 0\n");

  const std::vector<std::string> env{
      "PATH=" + bin.string() + ":/usr/bin:/bin",
      "XDG_CONFIG_HOME=" + cfg.string(),
  };
  const berth::Result<berth::RunOutput> result = run_cmd(
      {root / "install.sh", "--uninstall", "--prefix", prefix.string()}, env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);

  const std::string recorded = read_file(log);
  CHECK(recorded.find("berthpanel-memos.service") != std::string::npos);
  CHECK(recorded.find("berthpanel-cleanup.timer") != std::string::npos);
  // The foreign units are never disabled.
  CHECK(recorded.find("disable --now other.service") == std::string::npos);
  CHECK(recorded.find("demo-x.service") == std::string::npos);

  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));
  CHECK(!fs::exists(cfg / "systemd" / "user" / "berthpanel-memos.service"));
  CHECK(fs::exists(cfg / "systemd" / "user" / "other.service"));
}

TEST(uninstaller_is_idempotent) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path prefix = tmp.path() / "prefix";
  fs::create_directories(bin);
  fs::create_directories(prefix / "bin");
  write_text(prefix / "bin" / "berthpanel", "BIN");
  write_executable(
      bin / "systemctl",
      "#!/bin/sh\ncmd=$1\ncase \"$cmd\" in --*) shift; esac\nexit 0\n");

  const std::vector<std::string> env{
      "PATH=" + bin.string() + ":/usr/bin:/bin",
      "XDG_CONFIG_HOME=" + (tmp.path() / "cfg").string(),
  };
  const std::vector<std::string> argv{root / "install.sh", "--uninstall",
                                      "--prefix", prefix.string()};
  const berth::Result<berth::RunOutput> first = run_cmd(argv, env);
  CHECK(first.is_ok());
  CHECK(first.value().exit_code == 0);
  const berth::Result<berth::RunOutput> second = run_cmd(argv, env);
  CHECK(second.is_ok());
  CHECK(second.value().exit_code == 0);
  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));
}
