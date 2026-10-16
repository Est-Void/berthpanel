// The real signing key never exists in a test: a stub minisign on PATH is
// run()'s per-call extra_env, so no process-wide environment is touched.

#include <sys/stat.h>

#include <chrono>
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
    if (fs::is_directory(base / "scripts")) {
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

void write_text(const fs::path& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
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

// A minisign shim: `-G` writes a keypair, `-S` writes a detached signature,
// both at the paths given after -p/-s/-x.
void write_minisign_shim(const fs::path& path, const std::string& fingerprint) {
  write_executable(path,
                   "#!/bin/sh\n"
                   "mode=''\n"
                   "key=''\n"
                   "pub=''\n"
                   "sig=''\n"
                   "while [ \"$#\" -gt 0 ]; do\n"
                   "  case \"$1\" in\n"
                   "    -G) mode=gen ;;\n"
                   "    -S) mode=sign ;;\n"
                   "    -p) pub=$2; shift ;;\n"
                   "    -s) key=$2; shift ;;\n"
                   "    -x) sig=$2; shift ;;\n"
                   "    -t) shift ;;\n"
                   "  esac\n"
                   "  shift\n"
                   "done\n"
                   "if [ \"$mode\" = gen ]; then\n"
                   "  printf 'SECRET-KEY' > \"$key\"\n"
                   "  printf 'untrusted comment: minisign public key: " +
                       fingerprint +
                       "\\nRWQfakekeymaterial\\n' > \"$pub\"\n"
                       "  exit 0\n"
                       "fi\n"
                       "if [ \"$mode\" = sign ]; then\n"
                       "  printf 'fake-signature\\n' > \"$sig\"\n"
                       "  exit 0\n"
                       "fi\n"
                       "exit 1\n");
}

berth::Result<berth::RunOutput> run_cmd(
    const std::vector<std::string>& argv,
    const std::vector<std::string>& extra_env) {
  berth::RunLimits limits;
  limits.timeout = std::chrono::milliseconds(30000);
  limits.extra_env = &extra_env;
  return berth::run(argv, limits);
}

}
TEST(keygen_creates_a_keypair_and_prints_the_fingerprint) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path key_dir = tmp.path() / "cfg";
  fs::create_directories(bin);

  const std::string fp = "0123456789ABCDEF";
  write_minisign_shim(bin / "minisign", fp);

  const std::vector<std::string> env{
      "MINISIGN=" + (bin / "minisign").string(),
      "BERTHPANEL_RELEASE_KEY_DIR=" + key_dir.string(),
      "PATH=" + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "scripts" / "keygen-release.sh"}, env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(result.value().out.find("fingerprint: " + fp) != std::string::npos);
  CHECK(mode_of(key_dir / "release.key") == 0600);
  CHECK(mode_of(key_dir / "release.pub") == 0644);
}

TEST(keygen_refuses_to_overwrite_without_force) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path key_dir = tmp.path() / "cfg";
  fs::create_directories(bin);
  fs::create_directories(key_dir);
  write_text(key_dir / "release.key", "PRE-EXISTING");
  write_minisign_shim(bin / "minisign", "AABBCCDD");

  const std::vector<std::string> env{
      "MINISIGN=" + (bin / "minisign").string(),
      "BERTHPANEL_RELEASE_KEY_DIR=" + key_dir.string(),
      "PATH=" + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> refused =
      run_cmd({root / "scripts" / "keygen-release.sh"}, env);
  CHECK(refused.is_ok());
  CHECK(refused.value().exit_code != 0);
  CHECK(refused.value().err.find("refusing to overwrite") != std::string::npos);
  CHECK(read_file(key_dir / "release.key") == "PRE-EXISTING");

  const berth::Result<berth::RunOutput> forced =
      run_cmd({root / "scripts" / "keygen-release.sh", "--force"}, env);
  CHECK(forced.is_ok());
  CHECK(forced.value().exit_code == 0);
  CHECK(read_file(key_dir / "release.key") == "SECRET-KEY");
}

TEST(release_local_stages_deterministic_checksums) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path artifact = tmp.path() / "prebuilt";
  const fs::path release_dir = tmp.path() / "release";
  const std::string bytes = "BINARY-CONTENT";
  write_text(artifact, bytes);

  const std::vector<std::string> env{
      "RELEASE_ARTIFACT=" + artifact.string(),
      "RELEASE_DIR=" + release_dir.string(),
      "PATH=/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result = run_cmd(
      {root / "scripts" / "release-local.sh", "--fast", "--version", "v9.9.9"},
      env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(result.value().out.find("version v9.9.9") != std::string::npos);

  CHECK(read_file(release_dir / "berthpanel") == bytes);
  const std::string sums = read_file(release_dir / "SHA256SUMS");
  const std::string expected = berth::sha256_hex(bytes) + "  berthpanel";
  CHECK(sums.find(expected) != std::string::npos);
  // The Corresponding Source tarball is staged and covered by the checksums.
  const std::string src_name = "berthpanel-v9.9.9-src.tar.gz";
  CHECK(fs::exists(release_dir / src_name));
  CHECK(sums.find(src_name) != std::string::npos);
  const std::string src = read_file(release_dir / src_name);
  CHECK(src.size() > 2);
  CHECK(static_cast<unsigned char>(src[0]) == 0x1f);
  CHECK(static_cast<unsigned char>(src[1]) == 0x8b);  // gzip magic
  // install.sh is included only when the tracked installer is present.
  CHECK(!fs::exists(release_dir / "SHA256SUMS.minisig"));
}

TEST(release_local_can_skip_the_source_tarball) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path artifact = tmp.path() / "prebuilt";
  const fs::path release_dir = tmp.path() / "release";
  write_text(artifact, "BINARY-CONTENT");

  const std::vector<std::string> env{
      "RELEASE_ARTIFACT=" + artifact.string(),
      "RELEASE_DIR=" + release_dir.string(),
      "PATH=/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "scripts" / "release-local.sh", "--fast", "--no-source",
               "--version", "v9.9.9"},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(!fs::exists(release_dir / "berthpanel-v9.9.9-src.tar.gz"));
  CHECK(read_file(release_dir / "SHA256SUMS").find("src.tar.gz") ==
        std::string::npos);
}

TEST(release_local_signs_only_when_asked) {
  const fs::path root = repo_root();
  CHECK(!root.empty());

  const TempDir tmp;
  const fs::path bin = tmp.path() / "bin";
  const fs::path artifact = tmp.path() / "prebuilt";
  const fs::path release_dir = tmp.path() / "release";
  const fs::path key_dir = tmp.path() / "cfg";
  fs::create_directories(bin);
  fs::create_directories(key_dir);
  write_text(artifact, "BINARY-CONTENT");
  write_text(key_dir / "release.key", "SECRET-KEY");
  write_minisign_shim(bin / "minisign", "AABBCCDD");

  const std::vector<std::string> env{
      "RELEASE_ARTIFACT=" + artifact.string(),
      "RELEASE_DIR=" + release_dir.string(),
      "MINISIGN=" + (bin / "minisign").string(),
      "BERTHPANEL_RELEASE_KEY_DIR=" + key_dir.string(),
      "PATH=" + bin.string() + ":/usr/bin:/bin",
  };
  const berth::Result<berth::RunOutput> result =
      run_cmd({root / "scripts" / "release-local.sh", "--fast", "--version",
               "v9.9.9", "--sign"},
              env);
  CHECK(result.is_ok());
  CHECK(result.value().exit_code == 0);
  CHECK(read_file(release_dir / "SHA256SUMS.minisig") == "fake-signature\n");
}
