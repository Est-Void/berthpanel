// Offline: stub minisign and stub systemctl, so no real key or user manager
// is touched and PATH stays per-call (run()'s extra_env, never process-wide).

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

// `correct_checksum` decides whether SHA256SUMS matches the artifact.
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

}
TEST(installer_refuses_a_bad_signature_and_installs_nothing) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  fs::create_directories(bin);
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

TEST(installer_verifies_only_the_artifacts_it_fetches) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  // A real SHA256SUMS also lists install.sh and the source tarball; neither is
  // fetched by a binary install, so neither may be required.
  {
    std::ofstream out(src / "SHA256SUMS", std::ios::app);
    out << std::string(64, 'a') << "  install.sh\n"
        << std::string(64, 'b') << "  berthpanel-v0.1.0-alpha-src.tar.gz\n";
  }

  const std::vector<std::string> env{"PATH=/usr/bin:/bin"};
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "install.sh", "--source", src.string(), "--prefix",
               prefix.string(), "--allow-checksum-only", "--dry-run"},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(result.value().out.find("would install") != std::string::npos);
  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));
}

TEST(installer_rejects_a_missing_binary_checksum_line) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  // Lists the installer but not the artifact being installed: fail closed.
  write_text(src / "SHA256SUMS", std::string(64, 'a') + "  install.sh\n");

  const std::vector<std::string> env{"PATH=/usr/bin:/bin"};
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "install.sh", "--source", src.string(), "--prefix",
               prefix.string(), "--allow-checksum-only"},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code != 0);
  CHECK(result.value().err.find("no berthpanel entry in SHA256SUMS") !=
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

TEST(installer_pipe_safe_from_stdin) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path src = make_source(tmp.path() / "src", "BIN", true);
  const fs::path prefix = tmp.path() / "prefix";
  const std::string script = (root / "install.sh").string();
  // stdin is /dev/null, so `cat` reads the file and the inner `sh -s` takes the
  // script from the pipe: the `curl | sh` shape, no script file on disk.
  const std::vector<std::string> env{"PATH=/usr/bin:/bin"};

  const std::string help_cmd = "cat '" + script + "' | sh -s -- --prefix '" +
                               prefix.string() + "' --help";
  const berth::Result<berth::RunOutput> help =
      run_cmd({"/bin/sh", "-c", help_cmd}, env);
  CHECK(help.is_ok());
  CHECK(help.value().exit_code == 0);
  CHECK(help.value().out.find("Usage:") != std::string::npos);
  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));

  // Dry run through the pipe installs nothing and never touches the network.
  const std::string dry_cmd = "cat '" + script + "' | sh -s -- --source '" +
                              src.string() + "' --prefix '" + prefix.string() +
                              "' --allow-checksum-only --dry-run";
  const berth::Result<berth::RunOutput> dry =
      run_cmd({"/bin/sh", "-c", dry_cmd}, env);
  CHECK(dry.is_ok());
  CHECK(dry.value().exit_code == 0);
  CHECK(dry.value().out.find("would install") != std::string::npos);
  CHECK(!fs::exists(prefix / "bin" / "berthpanel"));
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
