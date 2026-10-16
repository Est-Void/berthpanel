#pragma once

// The stage parser reads deploy.log — written by the engine, not the panel
// (dev.md §5) — so a CLI deploy shows up in this screen's history too.
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
  std::string timestamp;
  std::string text;
  std::string service;
  DeployStage stage = DeployStage::kOther;
};

// A line this parser cannot read comes back as kOther with its text intact: a
// log line the panel refuses to display is worse than an unrecognised one.
[[nodiscard]] DeployLogLine parse_deploy_log_line(std::string_view raw);

[[nodiscard]] DeployStage parse_deploy_stage(std::string_view raw);

[[nodiscard]] std::vector<std::string> deploy_stage_lines(
    DeployStage current, bool finished, const std::string& failure);

// Empty (or whitespace only) means "ask the recipe, then pick a free port",
// never a hardcoded default. Anything outside 1..65535 is an Err naming what
// was typed; coercing it to 0 would deploy somewhere the user did not ask.
[[nodiscard]] berth::Result<std::uint16_t> parse_port_field(
    std::string_view field);

// A form and not a repair screen: the user must not store the detector's guess
// before fixing it. What a human cannot type still comes from detection, and
// the review shows it all (§8: validate_recipe needs a build or a prebuilt).
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

inline constexpr std::size_t kCreateFieldCount = 11;

[[nodiscard]] std::string_view create_field_label(CreateField field);
// Tab walks the whole form and wraps; kNone enters at the url.
[[nodiscard]] CreateField next_create_field(CreateField field);
[[nodiscard]] CreateField create_field_at(std::size_t index);

// `creating` is true while the worker runs, which locks the fields.
[[nodiscard]] std::vector<KeyHint> create_key_hints(CreateField focus,
                                                    bool creating);

// A draft is heuristics: the warnings print verbatim, never counted (§11).
[[nodiscard]] std::vector<std::string> draft_review_lines(
    const berth::DraftResult& draft);

[[nodiscard]] std::vector<std::string> draft_clash_notice_lines(
    const berth::DraftClash& clash);

// MUST carry the pin: "overwrite 2b2192d with 91ab2c4?" is a decision,
// "overwrite?" is not.
[[nodiscard]] std::vector<std::string> draft_clash_lines(
    const berth::DraftClash& clash, std::string_view new_commit);

[[nodiscard]] std::vector<std::string> draft_stored_lines(
    const std::filesystem::path& recipe_path);

[[nodiscard]] std::vector<KeyHint> deploy_key_hints(bool running);
[[nodiscard]] std::vector<KeyHint> draft_key_hints(bool running, bool ready);

[[nodiscard]] std::vector<std::string> deploy_idle_lines(
    const berth::Recipe& recipe, std::uint16_t chosen_port, bool have_choice);

[[nodiscard]] std::vector<std::string> deploy_outcome_lines(
    const berth::DeployOutcome& outcome);

// MUST say the journal entry stays for __cleanup (§6.7): a cancelled deploy
// that looks rolled back leaves the user hunting for a half-written unit.
[[nodiscard]] std::vector<std::string> deploy_cancelled_lines(
    const std::string& service);

// Wrapped by the caller; empty when nothing failed.
[[nodiscard]] std::vector<std::string> deploy_error_lines(
    const std::string& error);

// A user cancellation is NOT a failure even though the engine reports an
// Error: red next to the yellow cancellation text tells two stories at once.
struct DeployVerdict {
  bool success = false;
  bool cancelled = false;
  bool failed = false;
};
[[nodiscard]] DeployVerdict deploy_verdict(bool finished, bool cancelled,
                                           const std::string& error);

}
