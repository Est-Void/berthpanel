// Tests for the Deploy view model (stage 6.3). The parser is the load-bearing
// piece: deploy.log is written by the ENGINE, so the screen learns where a
// deploy is by reading the log — and "a log line the panel refuses to show" is
// a worse failure than "a stage the panel does not recognise".

#include "tui/view_deploy.h"

#include <cstdint>
#include <string>
#include <vector>

#include "runner.h"
#include "tui/ring.h"

namespace {

// One line exactly as deploylog_append writes it: ISO-8601 stamp, space, text.
std::string stamped(const std::string& text) {
  return "2026-10-08T12:34:56.789Z " + text;
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

}  // namespace

TEST(deploy_stage_parser_names_every_stage_the_engine_logs) {
  // The fixtures below are the log lines run_deploy actually writes, so a
  // change to the engine's wording breaks this test instead of the screen.
  CHECK(tui::parse_deploy_stage(stamped(
            "fetch berthpanel-memos@2b2192d4e153bd04f1d325b60fd880cf00d68b01 "
            "port 8234")) == tui::DeployStage::kFetch);
  CHECK(tui::parse_deploy_stage(stamped("build berthpanel-memos port 8234")) ==
        tui::DeployStage::kBuild);
  CHECK(tui::parse_deploy_stage(stamped("start berthpanel-memos")) ==
        tui::DeployStage::kStart);
  CHECK(tui::parse_deploy_stage(stamped("done berthpanel-memos port 8234")) ==
        tui::DeployStage::kDone);
}

TEST(deploy_stage_parser_surfaces_a_registry_write_failure) {
  // The one outcome that must never read as a success (§11): the unit is live
  // and the panel does not know about it.
  const tui::DeployLogLine line = tui::parse_deploy_log_line(stamped(
      "REGISTRY WRITE FAILED berthpanel-memos: state.json is a directory"));
  CHECK(line.stage == tui::DeployStage::kRegistryFailed);
  CHECK(line.service == "berthpanel-memos");
  CHECK(tui::deploy_stage_label(tui::DeployStage::kRegistryFailed) ==
        "REGISTRY WRITE FAILED");
}

TEST(deploy_stage_parser_separates_the_timestamp_and_the_service) {
  const tui::DeployLogLine line = tui::parse_deploy_log_line(
      stamped("fetch berthpanel-memos@2b2192d4e153bd04f1d325b60fd880cf00d68b01 "
              "port 8234"));
  CHECK(line.timestamp == "2026-10-08T12:34:56.789Z");
  CHECK(line.stage == tui::DeployStage::kFetch);
  // The pin is glued to the name in this line; the service is the unit stem.
  CHECK(line.service == "berthpanel-memos");
  CHECK(line.text.find("port 8234") != std::string::npos);
}

TEST(deploy_stage_parser_keeps_a_line_it_cannot_classify) {
  // A sweep line, and a hand-written line with no timestamp at all. Both must
  // survive to the screen: an unreadable log line that vanishes is silence
  // where the user is debugging (§11).
  const tui::DeployLogLine sweep = tui::parse_deploy_log_line(
      stamped("cleanup failed for berthpanel-other: unit still active"));
  CHECK(sweep.stage == tui::DeployStage::kOther);
  CHECK(sweep.timestamp == "2026-10-08T12:34:56.789Z");
  CHECK(sweep.text.find("cleanup failed") != std::string::npos);

  const tui::DeployLogLine bare =
      tui::parse_deploy_log_line("done berthpanel-x");
  CHECK(bare.stage == tui::DeployStage::kDone);
  CHECK(bare.timestamp.empty());
  CHECK(bare.service == "berthpanel-x");

  const tui::DeployLogLine empty = tui::parse_deploy_log_line("");
  CHECK(empty.stage == tui::DeployStage::kOther);
  CHECK(empty.text.empty());
}

TEST(deploy_stage_parser_never_splits_one_record_into_two) {
  // A newline inside a line would render as two records on the tail pane.
  const tui::DeployLogLine line =
      tui::parse_deploy_log_line("done berthpanel-memos\nport 8234");
  CHECK(line.text.find('\n') == std::string::npos);
  CHECK(line.stage == tui::DeployStage::kDone);
}

TEST(deploy_stage_lines_show_reached_pending_and_the_failure) {
  const std::vector<std::string> fetching =
      tui::deploy_stage_lines(tui::DeployStage::kFetch, false, "");
  CHECK(contains(fetching, "ok"));
  CHECK(contains(fetching, "fetch"));
  CHECK(contains(fetching, "..."));
  // Nothing past the current stage claims to be done.
  CHECK(!contains(fetching, "done  (last stage reached)"));

  const std::vector<std::string> finished =
      tui::deploy_stage_lines(tui::DeployStage::kDone, true, "");
  CHECK(contains(finished, "(last stage reached)"));

  // A failure line is carried verbatim: the screen's own error box may wrap it,
  // but the ladder must not swallow it.
  const std::vector<std::string> failed = tui::deploy_stage_lines(
      tui::DeployStage::kBuild, false, "build failed: exit 1");
  CHECK(contains(failed, "build failed: exit 1"));

  // Before anything is logged, the ladder says so rather than showing nothing.
  CHECK(contains(tui::deploy_stage_lines(tui::DeployStage::kOther, false, ""),
                 "no stage logged yet"));
}

TEST(deploy_port_field_treats_empty_as_auto_and_refuses_nonsense) {
  // Empty is the recipe's own default or a free port — never a hardcoded one.
  CHECK(tui::parse_port_field("").value() == 0);
  CHECK(tui::parse_port_field("   ").value() == 0);
  CHECK(tui::parse_port_field("8234").value() == 8234);
  CHECK(tui::parse_port_field(" 8234 ").value() == 8234);
  CHECK(tui::parse_port_field("65535").value() == 65535);

  // A typo is a loud refusal, not a silent 0 that deploys somewhere else.
  for (const std::string_view bad :
       {"0", "65536", "-1", "80O", "8080a", "8 080", "0x1f90"}) {
    const berth::Result<std::uint16_t> parsed = tui::parse_port_field(bad);
    CHECK(parsed.is_err());
    CHECK(!parsed.error().ctx().empty());
  }
}

TEST(deploy_port_field_takes_digits_and_nothing_else) {
  // The reason the field is not allowed to eat letters: 'q' quits, 'e' and 'r'
  // act on the screen. A panel you cannot quit from one tab is a trap.
  for (const char* digit : {"0", "1", "5", "9"}) {
    CHECK(tui::port_field_accepts_digit(digit));
  }
  for (const char* not_a_digit : {"", "12", "a", "q", "c", " ", "-", "8\n"}) {
    CHECK(!tui::port_field_accepts_digit(not_a_digit));
  }
}

TEST(deploy_cancellation_says_the_journal_entry_stays) {
  // §6.7: a cancelled deploy keeps its journal entry and __cleanup sweeps it.
  // A status line that made a cancellation look like a rollback would send the
  // user hunting for a half-written unit.
  const std::vector<std::string> lines =
      tui::deploy_cancelled_lines("berthpanel-memos");
  CHECK(contains(lines, "cancelled berthpanel-memos"));
  CHECK(contains(lines, "journal entry stays"));
  CHECK(contains(lines, "__cleanup"));
  CHECK(contains(lines, "nothing was deleted"));
}

TEST(deploy_hints_name_the_launch_key_and_the_cancel_key) {
  const std::vector<std::string> idle = tui::deploy_key_hints(false);
  CHECK(contains(idle, "Enter"));
  CHECK(contains(idle, "e  recipe details"));
  // While idle, 'c' would be a letter in the port field, so it must not be
  // advertised as a cancel key there.
  CHECK(!contains(idle, "cancel"));

  const std::vector<std::string> running = tui::deploy_key_hints(true);
  CHECK(contains(running, "c  cancel"));
  CHECK(contains(running, "q  quit"));
}

TEST(deploy_outcome_lines_show_the_port_the_unit_and_the_data) {
  berth::DeployOutcome outcome;
  outcome.service_name = "berthpanel-memos";
  outcome.port = 8234;
  outcome.work_dir =
      "/home/u/.local/share/berthpanel/services/berthpanel-memos";
  outcome.unit_path = "/home/u/.config/systemd/user/berthpanel-memos.service";

  const std::vector<std::string> lines = tui::deploy_outcome_lines(outcome);
  CHECK(contains(lines, "deployed berthpanel-memos"));
  CHECK(contains(lines, "port: 8234"));
  CHECK(contains(lines, "berthpanel-memos.service"));
  // Uninstall keeps the data dir on purpose; the screen must not imply the
  // deploy owned it (§12.12).
  CHECK(contains(lines, "leaves the data dir in place"));
}

TEST(deploy_error_lines_keep_the_whole_error_text) {
  // §11: the full error, not a summary. deploy.cpp adds context at every
  // boundary and the screen must not truncate it away.
  const std::vector<std::string> lines = tui::deploy_error_lines(
      "deploy: build failed: exit 1\n  (journal entry kept for cleanup)");
  CHECK(lines.size() == 3);
  CHECK(lines[0] == "deploy failed:");
  CHECK(contains(lines, "journal entry kept for cleanup"));
  CHECK(tui::deploy_error_lines("").empty());
}

TEST(deploy_idle_lines_say_which_port_will_be_used) {
  berth::Recipe recipe;
  recipe.service_name = "berthpanel-memos";
  recipe.owner = "usememos";
  recipe.name = "memos";
  recipe.commit = "2b2192d4e153bd04f1d325b60fd880cf00d68b01";

  CHECK(contains(tui::deploy_idle_lines(recipe, 0, false), "port: free"));
  CHECK(contains(tui::deploy_idle_lines(recipe, 9090, true), "port: 9090"));

  recipe.port = 8312;
  const std::vector<std::string> defaulted =
      tui::deploy_idle_lines(recipe, 0, false);
  CHECK(contains(defaulted, "port: 8312"));
  CHECK(contains(defaulted, "the recipe's own default"));
}

TEST(deploy_tail_integration_stays_bounded_and_oldest_first) {
  // The screen's buffer is a ring (dev.md §5: never a vector drained from the
  // front). This is the integration the deploy screen depends on: a long build
  // streams thousands of lines and the pane must neither grow nor reorder.
  // `text` keeps the stamp, because on the tail pane the time is information.
  tui::RingBuffer<std::string> tail(8);
  for (int i = 0; i < 40; ++i) {
    tail.push_back(
        tui::parse_deploy_log_line(
            stamped("build berthpanel-memos line " + std::to_string(i)))
            .text);
  }
  CHECK(tail.size() == 8);
  CHECK(tail.dropped() == 32);
  CHECK(tail.at(0).find("line 32") != std::string::npos);
  CHECK(tail.at(7).find("line 39") != std::string::npos);

  // Every retained line parses back to the build stage: the reader and the
  // parser agree on what the engine writes.
  for (std::size_t i = 0; i < tail.size(); ++i) {
    CHECK(tui::parse_deploy_stage(tail.at(i)) == tui::DeployStage::kBuild);
  }
}