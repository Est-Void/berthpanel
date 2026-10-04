// Tests for secrets.h (stage 4.7): EnvironmentFile rendering under
// systemd's rules, and a 0600 file in a 0700 directory.

#include "berth/secrets.h"

#include <filesystem>
#include <string>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"

using berth::render_environment_file;
using berth::SecretEntry;
using berth::write_secrets;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.secrets_dir = p.state_dir / "secrets";
  return p;
}

}  // namespace

TEST(secrets_render_quotes_values) {
  const auto text = render_environment_file({
      {"ADMIN_PASSWORD", "hunter2"},
      {"DSN", "user=me pass=word"},
  });
  CHECK(text.is_ok());
  CHECK(text.value() ==
        "ADMIN_PASSWORD=\"hunter2\"\nDSN=\"user=me pass=word\"\n");
}

TEST(secrets_render_escapes_shell_metacharacters) {
  const auto text = render_environment_file({
      {"TOKEN", "a\"b\\c$d`e"},
  });
  CHECK(text.is_ok());
  // Inside double quotes systemd EnvironmentFile honours backslash escapes
  // for '"', '\', '$' and '`'.
  CHECK(text.value() == "TOKEN=\"a\\\"b\\\\c\\$d\\`e\"\n");
}

TEST(secrets_render_rejects_newlines_and_bad_keys) {
  CHECK(render_environment_file({{"OK", "a\nb"}}).is_err());
  CHECK(render_environment_file({{"OK", std::string("a\0b", 3)}}).is_err());
  CHECK(render_environment_file({{"1BAD", "x"}}).is_err());
  CHECK(render_environment_file({{"has space", "x"}}).is_err());
  CHECK(render_environment_file({{"", "x"}}).is_err());
}

TEST(secrets_render_rejects_duplicate_keys) {
  CHECK(render_environment_file({{"K", "one"}, {"K", "two"}}).is_err());
}

TEST(secrets_write_is_private_and_atomic) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto written =
      write_secrets(p, "berthpanel-memos", {{"ADMIN_PASSWORD", "s3cret"}});
  CHECK(written.is_ok());
  CHECK(written.value().filename() == "berthpanel-memos.env");
  CHECK(std::filesystem::status(written.value()).permissions() ==
        berth::kPermPrivate);
  CHECK(std::filesystem::status(p.secrets_dir).permissions() ==
        berth::kPermPrivateDir);

  const auto body = berth::read_file(written.value());
  CHECK(body.is_ok());
  CHECK(body.value() == "ADMIN_PASSWORD=\"s3cret\"\n");

  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(p.secrets_dir)) {
    (void)entry;
    ++files;
  }
  CHECK(files == 1);  // no temp leftovers
}

TEST(secrets_write_refuses_invalid_service_name) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(write_secrets(p, "memos", {{"K", "v"}}).is_err());
}
