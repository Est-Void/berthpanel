// Tests for units.h (stage 4.6): rendering, per-argument quoting, the
// mandatory resource limits, and the two §11 traps — a $HOME with a space
// and list directives that must never be space-joined.

#include "berth/units.h"

#include <filesystem>
#include <string>
#include <vector>

#include "atomic.h"
#include "runner.h"
#include "state.h"
#include "test_env.h"

using berth::quote_exec_word;
using berth::quote_unit_value;
using berth::raw_systemd_path;
using berth::render_exec_start;
using berth::render_unit;
using berth::substitute_placeholders;
using berth::UnitLimits;
using berth::UnitSpec;

namespace {

UnitSpec base_spec() {
  UnitSpec spec;
  spec.service_name = "berthpanel-memos";
  spec.description = "Memos";
  spec.working_dir =
      "/home/u/.local/share/berthpanel/services/berthpanel-memos";
  spec.secrets_file =
      "/home/u/.local/state/berthpanel/secrets/berthpanel-memos.env";
  spec.exec_binary = spec.working_dir.string() + "/memos";
  spec.exec_args = {"--port", "8234", "--mode", "prod"};
  spec.limits = UnitLimits{.memory_max = "384M", .tasks_max = 300};
  spec.read_write_paths = {spec.working_dir,
                           "/home/u/.local/state/berthpanel/secrets"};
  return spec;
}

}  // namespace

TEST(units_exec_word_quoting_is_per_argument) {
  const auto plain = quote_exec_word("memos");
  CHECK(plain.is_ok());
  CHECK(plain.value() == "\"memos\"");

  const auto empty = quote_exec_word("");
  CHECK(empty.is_ok());
  CHECK(empty.value() == "\"\"");  // an empty argument must survive

  const auto spaced = quote_exec_word("a b");
  CHECK(spaced.is_ok());
  CHECK(spaced.value() == "\"a b\"");

  const auto quoted = quote_exec_word("say \"hi\"");
  CHECK(quoted.is_ok());
  CHECK(quoted.value() == "\"say \\\"hi\\\"\"");

  const auto backslash = quote_exec_word("a\\b");
  CHECK(backslash.value() == "\"a\\\\b\"");

  // '%' is a systemd specifier and must be doubled.
  CHECK(quote_exec_word("%i").value() == "\"%%i\"");

  // Newlines cannot be represented in a unit file.
  CHECK(quote_exec_word("a\nb").is_err());
}

TEST(units_exec_start_keeps_argument_boundaries) {
  const auto rendered =
      render_exec_start("/home/u/My Apps/memos",
                        {"--dir", "/home/u/My Apps/data", "--name", "a b"});
  CHECK(rendered.is_ok());
  CHECK(rendered.value() ==
        "\"/home/u/My Apps/memos\" \"--dir\" \"/home/u/My Apps/data\" "
        "\"--name\" \"a b\"");
}

TEST(units_unit_value_quoting_handles_percent_and_quotes) {
  CHECK(quote_unit_value("/home/u/100%").value() == "\"/home/u/100%%\"");
  CHECK(quote_unit_value("say \"x\"").value() == "\"say \\\"x\\\"\"");
  CHECK(quote_unit_value("a\\b").value() == "\"a\\\\b\"");
  CHECK(quote_unit_value("a\nb").is_err());
}

TEST(units_renders_hardening_and_limits) {
  const auto unit = render_unit(base_spec());
  CHECK(unit.is_ok());
  const std::string& text = unit.value();
  CHECK(text.find("MemoryMax=384M") != std::string::npos);
  CHECK(text.find("TasksMax=300") != std::string::npos);
  CHECK(text.find("NoNewPrivileges=yes") != std::string::npos);
  CHECK(text.find("ProtectSystem=strict") != std::string::npos);
  CHECK(text.find("PrivateTmp=yes") != std::string::npos);
  CHECK(text.find("SystemCallFilter=@system-service") != std::string::npos);
  CHECK(text.find("CapabilityBoundingSet=") != std::string::npos);
  CHECK(text.find("[Install]") != std::string::npos);
  // The honest framing travels with the hardening.
  CHECK(text.find("not a security boundary") != std::string::npos);
}

TEST(units_refuses_without_resource_limits) {
  UnitSpec no_memory = base_spec();
  no_memory.limits = UnitLimits{.memory_max = "", .tasks_max = 300};
  CHECK(render_unit(no_memory).is_err());

  UnitSpec no_tasks = base_spec();
  no_tasks.limits = UnitLimits{.memory_max = "384M", .tasks_max = 0};
  CHECK(render_unit(no_tasks).is_err());
}

TEST(units_refuses_invalid_service_name) {
  UnitSpec bad = base_spec();
  bad.service_name = "memos";  // no berthpanel- prefix
  CHECK(render_unit(bad).is_err());
  bad.service_name = "berthpanel-a/b";
  CHECK(render_unit(bad).is_err());
}

TEST(units_read_write_paths_are_one_directive_each) {
  UnitSpec spec = base_spec();
  spec.read_write_paths = {"/home/u/My Apps/data", "/home/u/state"};
  const auto unit = render_unit(spec);
  CHECK(unit.is_ok());
  // Exactly two ReadWritePaths lines: a joined list would be one line with
  // a space, and the namespace setup would fail (§11).
  const std::string needle = "ReadWritePaths=";
  std::size_t count = 0;
  for (std::size_t pos = unit.value().find(needle); pos != std::string::npos;
       pos = unit.value().find(needle, pos + 1)) {
    ++count;
  }
  CHECK(count == 2);
  // Path directives use systemd-escape --path semantics, not quotes.
  CHECK(unit.value().find("ReadWritePaths=/home/u/My Apps/data") !=
        std::string::npos);
}

TEST(units_workspace_with_spaces_survives_rendering) {
  UnitSpec spec = base_spec();
  spec.working_dir = "/home/u/My Apps/berthpanel-memos";
  spec.secrets_file = "/home/u/My Apps/secrets/berthpanel-memos.env";
  spec.exec_binary = spec.working_dir.string() + "/memos";
  const auto unit = render_unit(spec);
  CHECK(unit.is_ok());
  CHECK(
      unit.value().find("WorkingDirectory=/home/u/My Apps/berthpanel-memos") !=
      std::string::npos);
  CHECK(unit.value().find(
            "ExecStart=\"/home/u/My Apps/berthpanel-memos/memos\"") !=
        std::string::npos);
}

TEST(units_path_directives_take_the_raw_path) {
  // systemd parses path directives verbatim: quotes would become part of
  // the path, but spaces are fine (verified against a live user manager).
  CHECK(raw_systemd_path("/home/u/data").value() == "/home/u/data");
  CHECK(raw_systemd_path("/home/u/My Apps").value() == "/home/u/My Apps");
  CHECK(raw_systemd_path("/a/../b").value() == "/a/../b");
  CHECK(raw_systemd_path("").is_err());
  CHECK(raw_systemd_path("relative/path").is_err());
  CHECK(raw_systemd_path("/home/u/line\nbreak").is_err());
}

TEST(units_placeholders_substitute_or_fail_loudly) {
  const std::vector<std::pair<std::string, std::string>> values = {
      {"bin", "/opt/memos"}, {"port", "8234"}, {"self", "/usr/bin/memos"}};
  CHECK(substitute_placeholders("{bin} --port {port}", values).value() ==
        "/opt/memos --port 8234");
  CHECK(substitute_placeholders("literal only", values).value() ==
        "literal only");
  CHECK(substitute_placeholders("{bin} --self {self}", values).value() ==
        "/opt/memos --self /usr/bin/memos");
  CHECK(substitute_placeholders("{nope}", values).is_err());
  CHECK(substitute_placeholders("{unclosed", values).is_err());
}

TEST(units_write_unit_is_atomic_and_0644) {
  const TempDir tmp;
  berth::Paths paths;
  paths.units_dir = tmp.path() / "config" / "systemd" / "user";
  const auto written = berth::write_unit(paths, base_spec());
  CHECK(written.is_ok());
  CHECK(written.value().filename() == "berthpanel-memos.service");
  // Unit files are not secrets: 0644 by design, unlike state/secrets.
  const std::filesystem::perms expected =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::group_read | std::filesystem::perms::others_read;
  CHECK(std::filesystem::status(written.value()).permissions() == expected);
  const auto body = berth::read_file(written.value());
  CHECK(body.is_ok());
  CHECK(body.value().find("ExecStart=") != std::string::npos);

  // No temp leftovers; exactly one file.
  std::size_t files = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(paths.units_dir)) {
    (void)entry;
    ++files;
  }
  CHECK(files == 1);
}
