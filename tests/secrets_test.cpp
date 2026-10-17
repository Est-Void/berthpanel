// Tests for secrets.h (stage 4.7): EnvironmentFile rendering under
// systemd's rules, and a 0600 file in a 0700 directory.

#include "berth/secrets.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/fs.h"

using berth::list_secret_keys;
using berth::render_environment_file;
using berth::SecretEntry;
using berth::set_environment_value;
using berth::write_secrets;

namespace {

berth::Paths test_paths(const std::filesystem::path& root) {
  berth::Paths p;
  p.state_dir = root / "state" / "berthpanel";
  p.secrets_dir = p.state_dir / "secrets";
  return p;
}

}
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

TEST(secrets_set_environment_value_replaces_and_keeps_other_values) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(write_secrets(p, "berthpanel-memos", {{"A", "one"}, {"B", "two"}})
            .is_ok());
  CHECK(set_environment_value(p, "berthpanel-memos", "B", "new value").is_ok());

  const auto body =
      berth::read_file(p.secrets_dir / "berthpanel-memos.env");
  CHECK(body.is_ok());
  CHECK(body.value() == "A=\"one\"\nB=\"new value\"\n");
  CHECK(std::filesystem::status(p.secrets_dir / "berthpanel-memos.env")
            .permissions() == berth::kPermPrivate);
}

TEST(secrets_set_environment_value_appends_a_new_key) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(write_secrets(p, "berthpanel-memos", {{"A", "one"}}).is_ok());
  CHECK(set_environment_value(p, "berthpanel-memos", "C", "three").is_ok());
  const auto body =
      berth::read_file(p.secrets_dir / "berthpanel-memos.env");
  CHECK(body.is_ok());
  CHECK(body.value() == "A=\"one\"\nC=\"three\"\n");
}

TEST(secrets_set_environment_value_preserves_comments_and_other_lines) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.secrets_dir).is_ok());
  const std::filesystem::path target =
      p.secrets_dir / "berthpanel-memos.env";
  CHECK(berth::atomic_write(target, "# keep me\nA = \"1\"\nOTHER=\"x\"\n",
                            berth::kPermPrivate)
            .is_ok());
  CHECK(set_environment_value(p, "berthpanel-memos", "A", "2").is_ok());
  const auto body = berth::read_file(target);
  CHECK(body.is_ok());
  CHECK(body.value() == "# keep me\nA=\"2\"\nOTHER=\"x\"\n");
}

TEST(secrets_set_environment_value_refuses_bad_inputs_without_writing) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(set_environment_value(p, "memos", "K", "v").is_err());
  CHECK(set_environment_value(p, "berthpanel-memos", "1BAD", "v").is_err());
  CHECK(set_environment_value(p, "berthpanel-memos", "OK", "a\nb").is_err());
  CHECK(!std::filesystem::exists(p.secrets_dir / "berthpanel-memos.env"));
}

TEST(secrets_set_environment_value_refuses_duplicate_key_lines) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(berth::ensure_dir(p.secrets_dir).is_ok());
  const std::filesystem::path target =
      p.secrets_dir / "berthpanel-memos.env";
  CHECK(berth::atomic_write(target, "A=\"1\"\nA=\"2\"\n", berth::kPermPrivate)
            .is_ok());
  CHECK(set_environment_value(p, "berthpanel-memos", "A", "3").is_err());
  const auto body = berth::read_file(target);
  CHECK(body.is_ok());
  CHECK(body.value() == "A=\"1\"\nA=\"2\"\n");
}

TEST(secrets_list_keys_returns_keys_only) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  CHECK(write_secrets(p, "berthpanel-memos",
                      {{"B", "secret-two"}, {"A", "secret-one"}})
            .is_ok());
  const auto keys = list_secret_keys(p, "berthpanel-memos");
  CHECK(keys.is_ok());
  CHECK(keys.value().size() == 2);
  CHECK(keys.value()[0] == "A");
  CHECK(keys.value()[1] == "B");
  std::string joined;
  for (const std::string& key : keys.value())
    joined += key;
  CHECK(joined.find("secret") == std::string::npos);
}

TEST(secrets_list_keys_of_a_missing_file_is_empty) {
  const TempDir tmp;
  const berth::Paths p = test_paths(tmp.path());
  const auto keys = list_secret_keys(p, "berthpanel-memos");
  CHECK(keys.is_ok());
  CHECK(keys.value().empty());
}
