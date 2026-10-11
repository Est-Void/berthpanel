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
#include "../berth/wizard.h"
#include "../util/result.h"
#include "chrome.h"  // KeyHint: the shared keys+words structured hint

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

// --- The creation form ('i') -----------------------------------------------
//
// ONE form for a new service: where it comes from (url, ref), which port to
// deploy it on, and the eight recipe values. It is a form and not a repair
// screen on purpose — the earlier shape made the user store the detector's
// guess first and only then fix it, so a wrong start_cmd or a missing limit was
// always written before anyone could see it.
//
// The eight values are the ones a user can reasonably type. What a recipe
// needs and a human cannot (language, tools, build steps, a prebuilt pin) still
// comes from detection, and the review shows all of it before anything is
// stored — a form that hid those would promise a recipe the validator rejects
// (validate_recipe requires a build or a prebuilt, §8).
enum class CreateField : std::size_t {
  kNone = 0,
  kUrl,
  kRef,
  kDeployPort,
  kStartCmd,
  kBind,
  kHealthPath,
  kDisplayName,
  kDescription,
  kAppPort,
  kMemoryMax,
  kTasksMax,
};

// Every fillable slot, in focus order. The count is the contract the renderer
// and the focus cycle both walk.
inline constexpr std::size_t kCreateFieldCount = 11;

[[nodiscard]] std::string_view create_field_label(CreateField field);
// Tab walks the whole form and wraps; kNone enters at the url.
[[nodiscard]] CreateField next_create_field(CreateField field);
[[nodiscard]] CreateField create_field_at(std::size_t index);

// The keys the creation form acts on. `creating` is true while the worker runs
// (fields locked). Pure so the hints are assertable without a terminal.
[[nodiscard]] std::vector<KeyHint> create_key_hints(CreateField focus,
                                                    bool creating);

// The reviewed draft: what was detected, what the tools say, and every warning
// the detector produced. A draft is heuristics — the screen has to say so, and
// it has to print the warnings verbatim rather than count them (§11).
[[nodiscard]] std::vector<std::string> draft_review_lines(
    const berth::DraftResult& draft);

// One line saying the store will ask first, and what it would replace. The
// question itself is a modal (draft_clash_lines), not a block of text the user
// has to read past.
[[nodiscard]] std::vector<std::string> draft_clash_notice_lines(
    const berth::DraftClash& clash);

// The clash question. It MUST carry the pin being replaced: "overwrite?" is not
// a decision a user can make, "overwrite 2b2192d with 91ab2c4?" is.
[[nodiscard]] std::vector<std::string> draft_clash_lines(
    const berth::DraftClash& clash, std::string_view new_commit);

// Shown after the store: where the draft now lives, and that it is on disk
// before anything deploys (reproducibility).
[[nodiscard]] std::vector<std::string> draft_stored_lines(
    const std::filesystem::path& recipe_path);

// The keys this screen acts on right now. One line per group; the footer keeps
// the global hints, this list is the screen's own. Structured as keys+words so
// the renderer colours only the keys.
[[nodiscard]] std::vector<KeyHint> deploy_key_hints(bool running);
[[nodiscard]] std::vector<KeyHint> draft_key_hints(bool running, bool ready);

// What the screen shows before anything was deployed: which recipe, and which
// port the deploy would use.
[[nodiscard]] std::vector<std::string> deploy_idle_lines(
    const berth::Recipe& recipe, std::uint16_t chosen_port, bool have_choice);

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

// The final verdict of a deploy, from the three facts the screen holds. A user
// cancellation is NOT a failure even though the engine reports it as an Error
// (the cancel path returns one to unwind): painting that red — "deploy failed:
// cancel: cancelled while stopping ..." — next to the yellow cancellation text
// tells the user two different stories about one event.
struct DeployVerdict {
  bool success = false;
  bool cancelled = false;
  bool failed = false;
};
[[nodiscard]] DeployVerdict deploy_verdict(bool finished, bool cancelled,
                                           const std::string& error);

}  // namespace tui