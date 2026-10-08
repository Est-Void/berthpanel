#pragma once

// src/tui/view_deploy.h — the Deploy view model (plan stage 6.3) as pure
// functions over data. No FTXUI, no threads, no engine calls: the deploy runs
// on a worker (screens.h) and everything the screen draws is assembled here,
// so the strings and the stage parsing are assertable without a terminal.
//
// The stage parser is the interesting part. deploy.log is written by the
// engine, not by the panel (dev.md §5 layering), so the screen learns where a
// deploy is by READING the log it already produces — one parse, one source of
// truth, and a deploy run from the CLI shows up in this screen's history
// exactly as a deploy run from here does.
//
// A REGISTRY WRITE FAILED line is its own stage and gets its own colour: the
// unit is live but not registered, which is the one outcome a user must not
// read as success (§11 — the most consequential bug of the old implementation).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../berth/deploy.h"
#include "../util/result.h"

namespace tui {

enum class DeployStage {
  kOther = 0,
  kFetch,
  kBuild,
  kStart,
  kDone,
  kRegistryFailed,  // live unit, failed registry write: rerun the deploy
};

[[nodiscard]] std::string_view deploy_stage_label(DeployStage stage);

struct DeployLogLine {
  std::string timestamp;  // empty when the line carried none
  std::string text;       // one line, newlines folded away
  std::string service;    // the service token, empty when the line names none
  DeployStage stage = DeployStage::kOther;
};

// One line of deploy.log. Tolerant by design: a line this parser cannot read
// comes back as kOther with its text intact, because a log line the panel
// refuses to display is worse than an unrecognised one.
[[nodiscard]] DeployLogLine parse_deploy_log_line(std::string_view raw);

// The same, for the case where only the stage matters.
[[nodiscard]] DeployStage parse_deploy_stage(std::string_view raw);

// The stage checklist: every stage with its state, so the screen shows where
// the deploy is AND what it has not reached yet. `current` is the newest stage
// seen; earlier stages read as done and later ones as pending.
[[nodiscard]] std::vector<std::string> deploy_stage_lines(
    DeployStage current, bool finished, const std::string& failure);

// The port field. Empty (or whitespace only) means "ask the recipe, then pick
// a free port" — the recipe's own default is not a hardcoded 8080. A port that
// is not a number in 1..65535 is an Err naming what was typed: silently
// coercing it to 0 would deploy somewhere the user did not ask for.
[[nodiscard]] berth::Result<std::uint16_t> parse_port_field(
    std::string_view field);

// The port field owns digits and nothing else. A field that swallowed every
// printable key would take 'q' (quit), 'e' and 'r' hostage: the panel must
// always be quittable and the screen's own keys must always work. Return,
// Backspace and Delete are routed to the field by the app, which knows the
// key names; this predicate is the printable-character half of that rule, and
// it is here so the rule is testable without a terminal.
[[nodiscard]] bool port_field_accepts_digit(const std::string& key);

// What the screen shows before anything was deployed, and the keys that act.
[[nodiscard]] std::vector<std::string> deploy_idle_lines(
    const berth::Recipe& recipe, std::uint16_t chosen_port, bool have_choice);
[[nodiscard]] std::vector<std::string> deploy_key_hints(bool running);

// The finished deploy: what is now true, and where the files are.
[[nodiscard]] std::vector<std::string> deploy_outcome_lines(
    const berth::DeployOutcome& outcome);

// The cancellation status. It MUST say that the journal entry stays and that
// __cleanup will pick it up (§6.7): a cancelled deploy that looks like a
// rolled-back one leaves the user hunting for a half-written unit.
[[nodiscard]] std::vector<std::string> deploy_cancelled_lines(
    const std::string& service);

// The failure text, wrapped by the caller. Empty when nothing failed.
[[nodiscard]] std::vector<std::string> deploy_error_lines(
    const std::string& error);

}  // namespace tui