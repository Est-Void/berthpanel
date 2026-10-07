// Tests for the FTXUI-free half of tui/ (stage 6.1, 6.2, 6.4): the chrome
// data and the two view models. These modules exist precisely so the strings
// and the layout rules are assertable without a terminal — the renderer in
// tui/app.cpp is the only part that needs FTXUI, and it is not in this binary.

#include "tui/chrome.h"
#include "tui/view_catalog.h"
#include "tui/view_services.h"

#include <cstddef>
#include <string>
#include <vector>

#include "berth/service_ops.h"
#include "runner.h"

namespace {

// Doubled visual centre (first + last) of the non-space part of a line, so
// no integer division has to be argued about.
std::size_t doubled_centre(const std::string& line) {
  const std::size_t first = line.find_first_not_of(' ');
  const std::size_t last = line.find_last_not_of(' ');
  return first + last;
}

bool contains(const std::vector<std::string>& haystack,
              const std::string_view needle) {
  for (const std::string& line : haystack) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

berth::Recipe sample_recipe() {
  berth::Recipe r;
  r.owner = "usememos";
  r.name = "memos";
  r.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";
  r.language = "go";
  r.service_name = "berthpanel-memos";
  r.display_name = "Memos";
  r.description = "self-hosted note taking";
  r.build_steps = {{"go", "build", "-o", "memos", "./cmd/memos"}};
  r.prefetch_steps = {{"git", "fetch", "--depth", "1"}};
  r.tools = {"go", "git"};
  r.res = {"512M", 300};
  r.binary_name = "memos";
  r.start_cmd = "{self} --addr 127.0.0.1 --port {port} --data {project}/data";
  r.port = 0;
  return r;
}

}  // namespace

// --- chrome ---------------------------------------------------------------

TEST(chrome_tabs_are_in_the_agreed_order_with_their_glyphs) {
  const std::span<const tui::TabSpec> specs = tui::tabs();
  CHECK(specs.size() == tui::kTabCount);
  CHECK(tui::tab_index(tui::Tab::kCatalog) == 0);
  CHECK(tui::tab_index(tui::Tab::kDeploy) == 1);
  CHECK(tui::tab_index(tui::Tab::kServices) == 2);
  CHECK(tui::tab_index(tui::Tab::kLogs) == 3);
  CHECK(specs[0].label == "System Scan");
  CHECK(specs[1].label == "Deploy");
  CHECK(specs[2].label == "My Services");
  CHECK(specs[3].label == "Logs");
  // Each tab has its own accent; a missing one would fall back to nothing.
  CHECK(specs[0].accent == "cyan");
  CHECK(specs[1].accent == "magenta");
  CHECK(specs[2].accent == "green");
  CHECK(specs[3].accent == "yellow");
}

TEST(chrome_sign_lines_share_one_visual_centre) {
  // The brand mark is load-bearing and an off-by-one is invisible in review.
  const std::vector<std::string>& sign = tui::sign_lines();
  CHECK(sign.size() == 3);
  for (const std::string& line : sign) {
    const std::string_view trimmed = std::string_view(line);
    CHECK(trimmed == "  o" || trimmed == " -+-" || trimmed == "\\_|_/");
  }
  CHECK(doubled_centre(sign[0]) == doubled_centre(sign[1]));
  CHECK(doubled_centre(sign[1]) == doubled_centre(sign[2]));
}

TEST(chrome_footer_names_the_keys_and_the_words) {
  const std::span<const tui::FooterHint> hints = tui::footer_hints();
  CHECK(hints.size() == 3);
  CHECK(hints[0].keys == "\xe2\x86\x91\xe2\x86\x93");  // ↑↓
  CHECK(hints[0].words == "select");
  CHECK(hints[1].keys == "Enter");
  CHECK(hints[1].words == "choose");
  CHECK(hints[2].keys == "q");
  CHECK(hints[2].words == "quit");
  for (const tui::FooterHint& hint : hints) {
    CHECK(!hint.accent.empty());
  }
}

TEST(chrome_pending_tabs_say_what_is_missing_and_how_to_work_around_it) {
  // An honest sentence with a working command in it, not an empty box (§11:
  // silence on a missing feature reads as a broken feature).
  const std::string_view deploy = tui::pending_notice(tui::Tab::kDeploy);
  CHECK(deploy.find("next stage") != std::string_view::npos);
  CHECK(deploy.find("berthpanel __deploy") != std::string_view::npos);
  const std::string_view logs = tui::pending_notice(tui::Tab::kLogs);
  CHECK(logs.find("net.log") != std::string_view::npos);
  CHECK(logs.find("--show-endpoints") != std::string_view::npos);
}

// --- catalog view model ----------------------------------------------------

TEST(catalog_list_lines_have_a_header_and_fixed_columns) {
  tui::Catalog catalog;
  catalog.recipes.push_back(sample_recipe());
  catalog.sources.push_back(tui::RecipeSource::kBuiltin);

  const std::vector<std::string> lines = tui::catalog_list_lines(catalog);
  CHECK(lines.size() == 2);
  CHECK(lines[0].find("SERVICE") == 0);
  CHECK(lines[1].find("Memos") == 0);
  CHECK(lines[1].find("go") != std::string::npos);
  CHECK(lines[1].find("2b2192d") != std::string::npos);
  CHECK(lines[1].find("512M/300") != std::string::npos);
}

TEST(catalog_detail_lines_show_the_pin_the_tools_the_steps_and_the_command) {
  tui::Catalog catalog;
  catalog.recipes.push_back(sample_recipe());
  catalog.sources.push_back(tui::RecipeSource::kBuiltin);

  const std::vector<std::string> lines = tui::catalog_detail_lines(catalog, 0);
  CHECK(contains(lines, "2b2192d4e153bd04f1d325b60fd880cf00d68b01"));
  CHECK(contains(lines, "usememos/memos"));
  CHECK(contains(lines, "go, git"));  // tools
  CHECK(contains(lines, "go build -o memos ./cmd/memos"));
  CHECK(contains(lines, "git fetch --depth 1"));
  CHECK(contains(lines, "berthpanel __deploy berthpanel-memos"));
  CHECK(contains(lines, "self-hosted note taking"));
  // The limits are mandatory (§8); the pane must show them, not hide them.
  CHECK(contains(lines, "512M / 300"));
}

TEST(catalog_detail_marks_a_user_recipe_as_such) {
  tui::Catalog catalog;
  catalog.recipes.push_back(sample_recipe());
  catalog.sources.push_back(tui::RecipeSource::kUser);
  CHECK(tui::recipe_source_label(tui::RecipeSource::kUser) == "user");
  CHECK(tui::recipe_source_label(tui::RecipeSource::kBuiltin) == "builtin");
  CHECK(contains(tui::catalog_detail_lines(catalog, 0), "source: user"));
}

TEST(catalog_render_step_keeps_arguments_together) {
  // A path with a space must survive as ONE argument in the display text —
  // nothing parses this back, but a display that lies about the argv is
  // still a lie about the deploy.
  CHECK(tui::render_step({"sh", "-c", "echo hello world"}) ==
        "sh -c echo hello world");
  CHECK(tui::render_step({"cp", "/tmp/with space/bin", "/tmp/out"}) ==
        "cp /tmp/with space/bin /tmp/out");
  CHECK(tui::render_step({"true"}) == "true");
}

// --- services view model ---------------------------------------------------

TEST(services_table_names_the_unit_the_state_the_port_and_the_pin) {
  std::vector<berth::ServiceRow> rows;
  berth::ServiceRow row;
  row.service_name = "berthpanel-memos";
  row.state = berth::ServiceState::kActive;
  row.port = 8234;
  row.commit_short = "2b2192d";
  rows.push_back(row);
  berth::ServiceRow unknown;
  unknown.service_name = "berthpanel-thing";
  unknown.state = berth::ServiceState::kUnknown;
  rows.push_back(unknown);

  const std::vector<std::string> lines = tui::services_table_lines(rows);
  CHECK(lines.size() == 3);
  CHECK(lines[0].find("SERVICE") == 0);
  CHECK(lines[1].find("berthpanel-memos") == 0);
  CHECK(contains({lines[1]}, "active"));
  CHECK(contains({lines[1]}, "8234"));
  CHECK(contains({lines[1]}, "2b2192d"));
  // A row with no port and no pin shows placeholders, not empty columns.
  CHECK(contains({lines[2]}, "unknown"));
  CHECK(contains({lines[2]}, "-"));
}

TEST(services_uninstall_confirmation_says_the_data_stays) {
  // §11: a silent data delete is forbidden, and so is hiding it behind a
  // dialog. The confirmation MUST name the work dir and say it remains.
  berth::ServiceRow row;
  row.service_name = "berthpanel-memos";
  const std::vector<std::string> lines = tui::confirm_uninstall_lines(row);
  CHECK(contains(lines, "uninstall berthpanel-memos?"));
  CHECK(contains(lines, "STAY"));
  CHECK(contains(lines, "never deletes your data silently"));
  CHECK(contains(lines, "y  yes, uninstall"));
  CHECK(contains(lines, "n  no, keep it"));
}

TEST(services_empty_registry_points_at_the_way_forward) {
  const std::vector<std::string> lines = tui::services_empty_lines();
  CHECK(contains(lines, "no services registered"));
  CHECK(contains(lines, "berthpanel __deploy"));
}

TEST(services_key_hints_name_every_action) {
  const std::vector<std::string> hints = tui::services_key_hints();
  CHECK(contains(hints, "s start"));
  CHECK(contains(hints, "x stop"));
  CHECK(contains(hints, "u uninstall"));
  CHECK(contains(hints, "r refresh"));
}