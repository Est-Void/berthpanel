#include "screens.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <thread>

#include "../berth/deploy.h"
#include "../berth/recipe_edit.h"
#include "../berth/service_ops.h"
#include "../berth/userrecipes.h"
#include "../berth/wizard.h"
#include "../core/netstatus.h"
#include "../util/result.h"
#include "../util/strutil.h"
#include "../util/tail.h"
#include "view_edit.h"

namespace tui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

constexpr auto kDeployTailPoll = std::chrono::milliseconds(250);
constexpr std::chrono::milliseconds kTailDrainBudget{2000};
// The tail must know where this deploy's log starts before its first line.
constexpr std::chrono::milliseconds kTailReadyBudget{1000};

constexpr std::size_t kDeployListRows = 6;
constexpr std::size_t kDeployTailRows = 8;
// A draft takes the rows a vbox would DROP — silently eating the port field.
constexpr std::size_t kDraftListRows = 2;
constexpr std::size_t kDraftTailRows = 3;

}
namespace {

void absorb_deploy_lines(const std::shared_ptr<Panel>& panel,
                         const std::vector<std::string>& lines) {
  std::lock_guard lock(panel->mutex);
  for (const std::string& line : lines) {
    panel->deploy_log_lines.push_back(line);
    const DeployLogLine parsed = parse_deploy_log_line(line);
    if (parsed.stage != DeployStage::kOther) {
      panel->deploy_stage = parsed.stage;
    }
  }
}

// Shared: the tail worker outlives start_deploy(), so a reference dangles.
struct TailGate {
  std::atomic<bool> ready{false};
  std::atomic<bool> drained{false};
};

void follow_deploy_log(const std::shared_ptr<Panel>& panel,
                       const std::filesystem::path& log_path,
                       const std::shared_ptr<TailGate>& gate) {
  const std::shared_ptr<Panel> keep_alive = panel;
  std::thread tail([keep_alive, log_path, gate]() {
    // Start at the current end: this screen shows THIS deploy. A POSIX fd, not
    // stdio: read() after EOF sees later appends, a FILE* freezes on its EOF.
    int fd = ::open(log_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0 && ::lseek(fd, 0, SEEK_END) < 0) {
      ::close(fd);
      fd = -1;
    }

    std::string partial;  // a line split across two reads stays one line
    std::vector<std::string> fresh;
    const auto read_new = [&fd, &partial, &fresh]() {
      if (fd < 0) {
        return;
      }
      char buffer[8192];
      ssize_t got = 0;
      while ((got = ::read(fd, buffer, sizeof buffer)) > 0) {
        partial.append(buffer, static_cast<std::size_t>(got));
        std::size_t newline = 0;
        while ((newline = partial.find('\n')) != std::string::npos) {
          fresh.push_back(partial.substr(0, newline));
          partial.erase(0, newline + 1);
        }
      }
    };
    const auto flush = [&]() {
      if (fresh.empty()) {
        return;
      }
      absorb_deploy_lines(keep_alive, fresh);
      fresh.clear();
      request_render(*keep_alive);
    };

    gate->ready.store(true, std::memory_order_release);
    while (keep_alive->deploy_running.load(std::memory_order_relaxed) &&
           !keep_alive->shutting_down.load(std::memory_order_relaxed)) {
      // A missing file means 'no first line yet', not 'no progress' — keep
      // trying rather than lose the whole stage ladder (§11).
      if (fd < 0) {
        fd = ::open(log_path.c_str(), O_RDONLY | O_CLOEXEC);
      }
      read_new();
      flush();
      std::this_thread::sleep_for(kDeployTailPoll);
    }
    if (fd < 0) {
      fd = ::open(log_path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (fd >= 0) {
      read_new();
      flush();
      ::close(fd);
    }
    gate->drained.store(true, std::memory_order_release);
  });
  tail.detach();
}

}
void start_deploy(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const berth::Recipe& recipe,
                  const std::uint16_t forced_port, DeployPreStore pre_store) {
  {
    std::lock_guard lock(panel->mutex);
    // One deploy at a time in the UI: deploy.lock is only the second line.
    if (panel->deploy_in_flight) {
      return;
    }
    panel->deploy_in_flight = true;
    panel->deploy_running.store(true, std::memory_order_relaxed);
    panel->deploy_cancel_requested.store(false, std::memory_order_relaxed);
    panel->deploy_error.clear();
    panel->deploy_finished = false;
    panel->deploy_cancelled = false;
    panel->deploy_outcome = berth::DeployOutcome{};
    panel->deploy_stage = DeployStage::kOther;
    panel->deploy_service = recipe.service_name;
    panel->deploy_log_lines.clear();
    panel->deploy_status = "starting " + recipe.service_name;
  }

  const std::shared_ptr<TailGate> gate = std::make_shared<TailGate>();
  follow_deploy_log(panel, paths.deploy_log, gate);

  run_on_worker<berth::DeployOutcome>(
      panel, "deploy " + recipe.service_name,
      [paths, recipe, forced_port, gate, panel,
       pre_store](const std::shared_ptr<Op>& op) {
        {
          // Publish the op BEFORE the tail-ready wait: 'c' reaches the engine
          // only through panel->deploy_op.
          std::lock_guard lock(panel->mutex);
          panel->deploy_op = op;
        }
        static_cast<void>(wait_for_flag(gate->ready, kTailReadyBudget));
        if (pre_store) {
          const berth::Result<void> stored = pre_store();
          if (stored.is_err()) {
            return berth::Result<berth::DeployOutcome>(
                berth::Error("deploy: storing the recipe first failed: " +
                             stored.error().ctx()));
          }
        }
        berth::RealDeployOps ops;
        berth::DeployOptions options;
        options.cancel = &op->cancel;
        berth::Result<berth::DeployOutcome> outcome =
            run_deploy(paths, recipe, forced_port, ops, options);
        // Stop the tail, then WAIT (bounded) for the done/failed lines.
        panel->deploy_running.store(false, std::memory_order_release);
        static_cast<void>(wait_for_flag(gate->drained, kTailDrainBudget));
        return outcome;
      },
      [](Panel& state, berth::Result<berth::DeployOutcome>&& outcome) {
        state.deploy_finished = true;
        state.deploy_in_flight = false;
        state.deploy_op.reset();
        if (outcome.is_err()) {
          state.deploy_cancelled =
              state.deploy_cancel_requested.load(std::memory_order_relaxed);
          state.deploy_error = outcome.error().ctx();
          state.deploy_status =
              (state.deploy_cancelled ? "cancelled " : "failed ") +
              state.deploy_service;
          return;
        }
        state.deploy_outcome = std::move(outcome).value();
        state.deploy_stage = DeployStage::kDone;
        state.deploy_status = "deployed " + state.deploy_outcome.service_name;
      });
}

void cancel_deploy(const std::shared_ptr<Panel>& panel) {
  std::shared_ptr<Op> op;
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->deploy_running.load(std::memory_order_relaxed) ||
        panel->deploy_cancel_requested.load(std::memory_order_relaxed)) {
      return;
    }
    panel->deploy_cancel_requested.store(true, std::memory_order_relaxed);
    op = panel->deploy_op;
    panel->deploy_status = "cancelling " + panel->deploy_service + "...";
  }
  if (op != nullptr) {
    op->cancel.store(true, std::memory_order_relaxed);
  }
  request_render(*panel);
}

Element render_deploy_screen(const Panel& panel,
                             const ftxui::Elements& create_inputs) {
  if (panel.draft_clash_confirming) {
    return boxed_dialog(
        draft_clash_lines(panel.draft_clash, panel.draft.commit),
        Color::Yellow);
  }
  if (panel.confirming_recipe_remove) {
    // Derived like the engine's recipe_file(), never a hand-typed XDG path.
    const std::string recipe_path =
        (panel.paths.recipes_dir / (panel.recipe_remove_target + ".json"))
            .string();
    return boxed_dialog(
        recipe_remove_confirm_lines(panel.recipe_remove_target, recipe_path),
        Color::Red);
  }
  const bool drafting = panel.draft_running || panel.draft_ready;
  Elements out;
  out.push_back(ftxui::text("DEPLOY") | ftxui::bold |
                ftxui::color(Color::Magenta));

  Elements status;
  if (panel.deploy_running.load(std::memory_order_relaxed)) {
    const bool cancelling =
        panel.deploy_cancel_requested.load(std::memory_order_relaxed);
    status.push_back(ftxui::text(std::string("\xe2\x8b\xb0 ") +
                                 (cancelling ? "cancelling " : "deploying ") +
                                 panel.deploy_service + "...") |
                     ftxui::color(cancelling ? Color::Yellow : Color::Magenta));
  } else if (!panel.deploy_status.empty()) {
    const DeployVerdict verdict = deploy_verdict(
        panel.deploy_finished, panel.deploy_cancelled, panel.deploy_error);
    const Color colour = verdict.success     ? Color::Green
                         : verdict.cancelled ? Color::Yellow
                                             : Color::Red;
    status.push_back(ftxui::text(panel.deploy_status) | ftxui::color(colour));
  } else {
    status.push_back(ftxui::text("idle"));
  }
  out.push_back(ftxui::hbox(std::move(status)) | ftxui::dim);
  out.push_back(ftxui::separator());

  if (!panel.catalog_status.empty()) {
    const bool bad = panel.catalog_status.rfind("could not", 0) == 0;
    out.push_back(ftxui::text("  " + panel.catalog_status) |
                  ftxui::color(bad ? Color::Red : Color::Green));
  }

  // A refusal is a status line, not a modal: the screen is not lost.
  if (!panel.edit_error.empty()) {
    for (const std::string& part : berth::split(panel.edit_error, "\n")) {
      out.push_back(ftxui::paragraph("  " + part) | ftxui::color(Color::Red));
    }
  }

  if (!panel.catalog_error.empty()) {
    std::vector<std::string> lines{"the catalog could not be read:"};
    for (const std::string& part : berth::split(panel.catalog_error, "\n")) {
      lines.push_back("  " + part);
    }
    out.push_back(paragraph_lines(lines) | ftxui::color(Color::Red));
  } else if (panel.catalog.recipes.empty() && !panel.catalog_loading) {
    out.push_back(
        ftxui::paragraph("the catalog is empty: no builtin recipes compiled "
                         "into this binary and no user recipes under the "
                         "recipes dir") |
        ftxui::dim);
  } else {
    // Header row is NOT selectable: the cursor indexes catalog.recipes.
    const std::size_t rows = drafting ? kDraftListRows : kDeployListRows;
    out.push_back(ftxui::text(catalog_header_line()) | ftxui::dim);
    out.push_back(line_list(catalog_row_lines(panel.catalog),
                            panel.catalog_cursor, Color::Magenta, true,
                            rows > 0 ? rows - 1 : 0));
  }
  if (panel.catalog_details) {
    out.push_back(paragraph_lines(
        catalog_detail_lines(panel.catalog, panel.catalog_cursor)));
  }
  out.push_back(ftxui::separator());

  if (panel.create_active || panel.create_running) {
    out.push_back(ftxui::text("NEW SERVICE") | ftxui::bold |
                  ftxui::color(Color::Magenta));
    out.push_back(
        ftxui::text("  url and ref say where it comes from; the eight fields "
                    "below are yours — they override what detection guesses") |
        ftxui::dim);
    if (!panel.create_status.empty()) {
      out.push_back(ftxui::text("\xe2\x97\x8f " + panel.create_status) |
                    ftxui::dim);
    }
    if (!panel.create_error.empty()) {
      for (const std::string& part : berth::split(panel.create_error, "\n")) {
        out.push_back(ftxui::paragraph("  " + part) | ftxui::color(Color::Red));
      }
    }
    for (std::size_t i = 0; i < create_inputs.size() && i < kCreateFieldCount;
         ++i) {
      const CreateField field = create_field_at(i);
      const bool focused = panel.create_field == field;
      Elements row;
      row.push_back(ftxui::text(focused ? "\xe2\x96\xb8 " : "  ") |
                    ftxui::color(focused ? Color::Magenta : Color::White));
      std::string label(create_field_label(field));
      label.append(13 - std::min<std::size_t>(label.size(), 13), ' ');
      row.push_back(ftxui::text(label + "  ") | ftxui::dim);
      row.push_back(create_inputs[i] | ftxui::xflex);
      out.push_back(ftxui::hbox(std::move(row)) | ftxui::flex);
    }
    out.push_back(ftxui::separator());
    for (const KeyHint& hint :
         create_key_hints(panel.create_field, panel.create_running)) {
      out.push_back(key_hint(hint));
    }
  }
  out.push_back(ftxui::separator());

  if (!drafting) {
    out.push_back(paragraph_lines(deploy_stage_lines(
        panel.deploy_stage, panel.deploy_finished, panel.deploy_status)));
  }
  if (panel.deploy_cancelled) {
    for (const std::string& line :
         deploy_cancelled_lines(panel.deploy_service)) {
      out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Yellow));
    }
  }
  const DeployVerdict verdict = deploy_verdict(
      panel.deploy_finished, panel.deploy_cancelled, panel.deploy_error);
  if (verdict.success) {
    out.push_back(paragraph_lines(deploy_outcome_lines(panel.deploy_outcome)) |
                  ftxui::color(Color::Green));
  }
  if (verdict.failed) {
    for (const std::string& line : deploy_error_lines(panel.deploy_error)) {
      out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Red));
    }
  }
  if (!panel.draft_status.empty()) {
    out.push_back(ftxui::text("\xe2\x97\x8f " + panel.draft_status) |
                  ftxui::dim);
  }
  if (panel.draft_ready) {
    out.push_back(ftxui::text("DRAFT") | ftxui::bold |
                  ftxui::color(Color::Yellow));
    for (const std::string& line : draft_review_lines(panel.draft)) {
      out.push_back(ftxui::paragraph(line));
    }
    for (const std::string& line :
         draft_clash_notice_lines(panel.draft_clash)) {
      out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Yellow));
    }
    if (panel.draft_stored) {
      for (const std::string& line :
           draft_stored_lines(panel.draft_stored_path)) {
        out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Green));
      }
    }
  }
  if (!panel.draft_error.empty()) {
    for (const std::string& part : berth::split(panel.draft_error, "\n")) {
      out.push_back(ftxui::paragraph("  " + part) | ftxui::color(Color::Red));
    }
  }
  out.push_back(ftxui::separator());

  Elements tail_label;
  tail_label.push_back(ftxui::text("deploy.log") | ftxui::bold);
  if (panel.deploy_log_lines.dropped() > 0) {
    tail_label.push_back(
        ftxui::text("  (" + std::to_string(panel.deploy_log_lines.dropped()) +
                    " earlier lines dropped — the buffer is "
                    "bounded)") |
        ftxui::dim);
  }
  out.push_back(ftxui::hbox(std::move(tail_label)) | ftxui::dim);
  out.push_back(log_window(panel.deploy_log_lines, 0,
                           drafting ? kDraftTailRows : kDeployTailRows,
                           Color::Cyan));
  out.push_back(ftxui::separator());
  const std::vector<KeyHint> hints =
      panel.draft_running || panel.draft_ready
          ? draft_key_hints(panel.draft_running, panel.draft_ready)
          : deploy_key_hints(
                panel.deploy_running.load(std::memory_order_relaxed));
  for (const KeyHint& hint : hints) {
    out.push_back(key_hint(hint));
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

Element render_edit_screen(const Panel& panel,
                           const ftxui::Elements& edit_inputs) {
  Elements out;
  out.push_back(ftxui::text("EDIT " + panel.edit_service) | ftxui::bold |
                ftxui::color(Color::Magenta));
  out.push_back(ftxui::text("  a user recipe — identity, pin and build steps "
                            "are not editable here") |
                ftxui::dim);
  out.push_back(ftxui::separator());

  if (!panel.edit_status.empty()) {
    out.push_back(ftxui::text("\xe2\x97\x8f " + panel.edit_status) |
                  ftxui::dim);
  }
  const bool locked = panel.edit_running;
  for (std::size_t i = 0; i < edit_inputs.size() && i < kEditFieldCount; ++i) {
    const EditField field = edit_field_at(i);
    const bool focused = panel.edit_field == field;
    Elements row;
    row.push_back(ftxui::text(focused ? "\xe2\x96\xb8 " : "  ") |
                  ftxui::color(focused ? Color::Magenta : Color::White));
    std::string label(edit_field_label(field));
    if (label.size() < 13) {
      label.append(13 - label.size(), ' ');
    }
    row.push_back(ftxui::text(label + "  ") | ftxui::dim);
    row.push_back(edit_inputs[i] | ftxui::xflex);
    out.push_back(ftxui::hbox(std::move(row)) | ftxui::flex);
  }
  out.push_back(
      locked ? key_hint(KeyHint{"",
                                "fields are locked while the write "
                                "is in flight"})
             : key_hint_line(
                   {{"i", "edit"}, {"Tab", "next field"}, {"s", "save"}}));
  out.push_back(ftxui::separator());

  if (panel.edit_saved) {
    for (const std::string& line :
         edit_saved_lines(panel.edit_diff, panel.edit_stored_path)) {
      out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Green));
    }
  }
  if (!panel.edit_error.empty()) {
    for (const std::string& part : berth::split(panel.edit_error, "\n")) {
      out.push_back(ftxui::paragraph("  " + part) | ftxui::color(Color::Red));
    }
  }
  out.push_back(ftxui::separator());
  for (const KeyHint& hint :
       edit_key_hints(panel.edit_field, panel.edit_running)) {
    out.push_back(key_hint(hint));
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

void refresh_catalog(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths, const bool background) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->catalog_loading || panel->catalog_auto) {
      return;
    }
    if (background) {
      panel->catalog_auto = true;
    } else {
      panel->catalog_loading = true;
    }
    panel->catalog_error.clear();
  }
  run_on_worker<Catalog>(
      panel, "catalog load",
      [paths](const std::shared_ptr<Op>& op) -> berth::Result<Catalog> {
        std::atomic<bool>* cancel = &op->cancel;
        if (cancel->load()) {
          return berth::Error("cancelled before the catalog read");
        }
        return load_catalog(paths);
      },
      [](Panel& state, berth::Result<Catalog>&& loaded) {
        state.catalog_loading = false;
        state.catalog_auto = false;
        if (loaded.is_err()) {
          state.catalog_error = loaded.error().ctx();
          state.catalog = Catalog{};
          state.catalog_cursor = 0;
          return;
        }
        state.catalog = std::move(loaded).value();
        if (state.catalog_cursor >= state.catalog.recipes.size()) {
          state.catalog_cursor = 0;
        }
      });
}

void remove_user_recipe(const std::shared_ptr<Panel>& panel,
                        const berth::Paths& paths,
                        const std::string& service_name) {
  {
    std::lock_guard lock(panel->mutex);
    // A delete racing a reload is the §11 stale-row bug.
    if (panel->recipe_removing || panel->catalog_loading ||
        panel->catalog_auto) {
      return;
    }
    panel->recipe_removing = true;
    panel->catalog_status = "removing the recipe " + service_name + "...";
  }
  run_on_worker<Catalog>(
      panel, "recipe remove " + service_name,
      [paths,
       service_name](const std::shared_ptr<Op>& op) -> berth::Result<Catalog> {
        if (op->cancel.load(std::memory_order_relaxed)) {
          return berth::Error("recipe remove: cancelled before it ran");
        }
        const berth::Result<bool> removed =
            berth::user_recipe_remove(paths, service_name);
        if (removed.is_err()) {
          return std::move(removed).error();
        }
        berth::Result<Catalog> reloaded = load_catalog(paths);
        if (reloaded.is_err()) {
          return berth::with_context(std::move(reloaded).error(),
                                     "reloading the catalog after the removal");
        }
        return reloaded;
      },
      [service_name](Panel& state, berth::Result<Catalog>&& loaded) {
        state.recipe_removing = false;
        state.confirming_recipe_remove = false;
        if (loaded.is_err()) {
          state.catalog_status =
              "could not remove " + service_name + ": " + loaded.error().ctx();
          return;
        }
        state.catalog = std::move(loaded).value();
        if (state.catalog_cursor >= state.catalog.recipes.size()) {
          state.catalog_cursor = 0;
        }
        state.catalog_status = "removed the recipe " + service_name +
                               " (a deployed service, if any, is unchanged)";
      });
}

void run_system_scan(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->scan_running) {
      return;
    }
    panel->scan_running = true;
    panel->scan_ever_ran = true;
    panel->scan_error.clear();
  }
  run_on_worker<berth::ScanReport>(
      panel, "system scan",
      [paths](const std::shared_ptr<Op>& op) {
        return berth::run_system_scan(paths, &op->cancel);
      },
      [](Panel& state, berth::Result<berth::ScanReport>&& report) {
        state.scan_running = false;
        if (report.is_err()) {
          state.scan_error = report.error().ctx();
          return;
        }
        state.scan_error.clear();
        state.scan = std::move(report).value();
      });
}

namespace {

Element scan_section(const std::string& title,
                     const std::vector<std::string>& lines) {
  Elements out;
  out.push_back(ftxui::text(title) | ftxui::bold | ftxui::color(Color::Cyan));
  out.push_back(paragraph_lines(lines));
  out.push_back(ftxui::separator());
  return ftxui::vbox(std::move(out));
}

}
Element render_scan_screen(const Panel& panel) {
  Elements out;
  out.push_back(ftxui::text("SYSTEM SCAN") | ftxui::bold |
                ftxui::color(Color::Cyan));
  if (panel.scan_running) {
    out.push_back(ftxui::text("\xe2\x8b\xb0 running the scan (all four steps "
                              "are bounded)...") |
                  ftxui::dim);
  } else if (!panel.scan_ever_ran) {
    out.push_back(paragraph_lines(scan_idle_lines()) | ftxui::flex);
    return ftxui::vbox(std::move(out)) | ftxui::flex;
  } else {
    out.push_back(ftxui::text(scan_summary_line(panel.scan)) |
                  ftxui::color(Color::Green) | ftxui::bold);
  }
  out.push_back(ftxui::separator());

  if (!panel.scan_error.empty()) {
    out.push_back(paragraph_lines(std::vector<std::string>{
                      "the scan could not run:", "  " + panel.scan_error,
                      "  the report below is the "
                      "previous one"}) |
                  ftxui::color(Color::Red));
    out.push_back(ftxui::separator());
  }

  const std::string alert = scan_network_alert(panel.scan);
  if (!alert.empty()) {
    out.push_back(paragraph_lines(berth::split(alert, "\n")) |
                  ftxui::borderStyled(ftxui::ROUNDED, Color::Yellow));
    out.push_back(ftxui::separator());
  }

  if (panel.scan_running && !panel.scan_ever_ran) {
    out.push_back(paragraph_lines({"first run in progress..."}) | ftxui::dim);
    return ftxui::vbox(std::move(out)) | ftxui::flex;
  }

  out.push_back(scan_section("BUILD TOOLS THE CATALOG NEEDS",
                             scan_prereq_lines(panel.scan)));
  out.push_back(scan_section("PORTS IN USE", scan_port_lines(panel.scan)));
  out.push_back(scan_section("YOUR SERVICES", scan_service_lines(panel.scan)));
  out.push_back(scan_section("NETWORK", scan_network_lines(panel.scan)));
  out.push_back(key_hint(KeyHint{"r", "run the scan again"}));
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

namespace {

// The clash needs the detected name: file reads stay off the render thread.
struct DraftPrepared {
  berth::DraftResult draft;
  berth::DraftClash clash;
  std::string error;
};

}
namespace {

struct PreparedRecipe {
  berth::Recipe recipe;
  std::string owner_repo;
  std::string commit;
  std::vector<berth::ToolStatus> tools;
  std::vector<std::string> warnings;
  berth::DraftClash clash;
  std::string error;
};

}
void start_create_recipe(const std::shared_ptr<Panel>& panel,
                         const berth::Paths& paths,
                         const berth::DraftRequest& request,
                         const EditFields& fields) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->create_running || panel->deploy_in_flight) {
      return;
    }
    panel->create_running = true;
    panel->create_error.clear();
    panel->create_status = "working...";
  }
  const std::shared_ptr<Panel> keep_alive = panel;
  run_on_worker<PreparedRecipe>(
      panel, "create recipe",
      [paths, request, fields, keep_alive](
          const std::shared_ptr<Op>& op) -> berth::Result<PreparedRecipe> {
        {
          std::lock_guard lock(keep_alive->mutex);
          keep_alive->create_op = op;
        }
        const berth::DraftProgress progress =
            [keep_alive](const std::string& line) {
              std::lock_guard lock(keep_alive->mutex);
              keep_alive->create_status = line;
              request_render(*keep_alive);
            };
        berth::Result<berth::DraftResult> prepared =
            berth::prepare_draft(paths, request, &op->cancel, progress);
        PreparedRecipe out;
        if (prepared.is_err()) {
          out.error = prepared.error().ctx();
          return out;
        }
        berth::DraftResult detected = std::move(prepared).value();

        // The user's eight values WIN over the detector's guesses (§8).
        const berth::Result<berth::RecipeEdits> edits =
            assemble_recipe_edits(fields);
        if (edits.is_err()) {
          out.error = edits.error().ctx();
          return out;
        }
        const berth::Result<berth::Recipe> applied =
            berth::apply_recipe_edits(detected.draft, edits.value());
        if (applied.is_err()) {
          out.error = applied.error().ctx();
          return out;
        }
        detected.draft = std::move(applied).value();
        out.recipe = std::move(detected.draft);
        out.owner_repo = std::move(detected.owner_repo);
        out.commit = std::move(detected.commit);
        out.tools = std::move(detected.tools);
        out.warnings = std::move(detected.warnings);
        const berth::Result<berth::DraftClash> clash =
            berth::find_recipe_clash(paths, out.recipe.service_name);
        // An undeterminable clash IS a clash: a failed check must ask.
        if (clash.is_err()) {
          out.clash.exists = true;
          out.clash.service_name = out.recipe.service_name;
        } else {
          out.clash = clash.value();
        }
        return out;
      },
      [](Panel& state, berth::Result<PreparedRecipe>&& prepared) {
        state.create_running = false;
        state.create_op.reset();
        PreparedRecipe value = std::move(prepared).value();
        if (!value.error.empty()) {
          state.create_error = value.error;
          state.create_status = "the recipe could not be created";
          return;
        }
        state.create_error.clear();
        state.create_active = false;
        state.create_field = CreateField::kNone;
        // Handed to the EXISTING review + store path: two create paths drift.
        state.draft = berth::DraftResult{};
        state.draft.draft = std::move(value.recipe);
        state.draft.owner_repo = std::move(value.owner_repo);
        state.draft.commit = std::move(value.commit);
        state.draft.tools = std::move(value.tools);
        state.draft.warnings = std::move(value.warnings);
        state.draft_clash = std::move(value.clash);
        state.draft_ready = true;
        state.draft_stored = false;
        state.draft_error.clear();
        state.draft_status =
            "review it, then s to store or d to store and deploy";
      });
}

void accept_draft(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const bool deploy_after,
                  const bool force) {
  berth::Recipe recipe;
  std::uint16_t port = 0;
  bool clash_needs_an_answer = false;
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->draft_ready || panel->draft_stored || panel->draft_running ||
        panel->draft_storing || panel->deploy_in_flight) {
      return;
    }
    if (panel->draft_clash.exists && !force) {
      clash_needs_an_answer = true;
    } else {
      recipe = panel->draft.draft;
      const berth::Result<std::uint16_t> parsed =
          parse_port_field(panel->create_deploy_port);
      if (parsed.is_err()) {
        panel->draft_error = parsed.error().ctx();
        panel->draft_status = "nothing was stored";
        return;
      }
      port = std::move(parsed).value();
    }
  }
  if (clash_needs_an_answer) {
// Nothing is stored until the user says (§8).
    std::lock_guard lock(panel->mutex);
    panel->draft_clash_confirming = true;
    panel->draft_pending_deploy = deploy_after;
    return;
  }

  if (deploy_after) {
// Store and deploy are ONE worker: a refused store aborts the deploy.
    refresh_catalog(panel, paths);
    start_deploy(panel, paths, recipe, port,
                 [paths, recipe, force]() -> berth::Result<void> {
                   const berth::Result<std::filesystem::path> stored =
                       berth::store_draft(paths, recipe, force);
                   if (stored.is_err())
                     return std::move(stored).error();
                   return {};
                 });
    return;
  }

  {
    std::lock_guard lock(panel->mutex);
    if (panel->draft_storing) {
      return;
    }
    panel->draft_storing = true;
  }
  // The reload is in the SAME worker, so the next Enter deploys what was saved.
  struct StoredDraft {
    std::filesystem::path path;
    Catalog catalog;
  };
  const std::string service_name = recipe.service_name;
  run_on_worker<StoredDraft>(
      panel, "store draft",
      [paths, recipe,
       force](const std::shared_ptr<Op>& /*op*/) -> berth::Result<StoredDraft> {
        const berth::Result<std::filesystem::path> stored =
            berth::store_draft(paths, recipe, force);
        if (stored.is_err())
          return std::move(stored).error();
        berth::Result<Catalog> reloaded = load_catalog(paths);
        if (reloaded.is_err())
          return berth::with_context(
              std::move(reloaded).error(),
              "reloading the catalog after storing the draft");
        return StoredDraft{stored.value(), std::move(reloaded).value()};
      },
      [service_name](Panel& state, berth::Result<StoredDraft>&& stored) {
        state.draft_clash_confirming = false;
        state.draft_storing = false;
        if (stored.is_err()) {
          state.draft_error = stored.error().ctx();
          state.draft_status = "the draft was NOT stored";
          return;
        }
        StoredDraft value = std::move(stored).value();
        state.draft_stored = true;
        state.draft_stored_path = std::move(value.path);
        state.draft_error.clear();
        state.draft_status = "stored";
        state.catalog = std::move(value.catalog);
        for (std::size_t i = 0; i < state.catalog.recipes.size(); ++i) {
          if (state.catalog.recipes[i].service_name == service_name) {
            state.catalog_cursor = i;
            break;
          }
        }
      });
}

void discard_draft(const std::shared_ptr<Panel>& panel) {
  std::lock_guard lock(panel->mutex);
  panel->draft_ready = false;
  panel->draft_stored = false;
  panel->draft_stored_path.clear();
  panel->draft_clash = berth::DraftClash{};
  panel->draft_status.clear();
  panel->draft_error.clear();
}

namespace {

// One worker returns write + diff + reload, so the catalog cannot race the
// write; refresh_catalog() under the panel lock would deadlock.
struct EditSave {
  berth::RecipeEditResult edit;
  std::string diff;
  Catalog catalog;
};

}
void start_recipe_edit(const std::shared_ptr<Panel>& panel,
                       const berth::Paths& paths,
                       const berth::RecipeEdits& edits) {
  std::string service;
  {
    std::lock_guard lock(panel->mutex);
    if (panel->edit_running) {
      return;
    }
    panel->edit_running = true;
    panel->edit_saved = false;
    panel->edit_stored_path.clear();
    panel->edit_diff.clear();
    panel->edit_error.clear();
    service = panel->edit_service;
    panel->edit_status = "saving " + service;
  }
  run_on_worker<EditSave>(
      panel, "recipe edit " + service,
      [paths, service,
       edits](const std::shared_ptr<Op>& op) -> berth::Result<EditSave> {
        if (op->cancel.load(std::memory_order_relaxed)) {
          return berth::Error("recipe edit: cancelled before it was written");
        }
        berth::Result<berth::RecipeEditResult> updated =
            berth::user_recipe_update(paths, service, edits);
        if (updated.is_err()) {
          return std::move(updated).error();
        }
        berth::Result<Catalog> reloaded = load_catalog(paths);
        if (reloaded.is_err()) {
          return berth::with_context(
              std::move(reloaded).error(),
              "reloading the catalog after the edit was written");
        }
        EditSave save;
        save.edit = std::move(updated).value();
        save.diff =
            berth::render_recipe_edit_diff(save.edit.before, save.edit.after);
        save.catalog = std::move(reloaded).value();
        return save;
      },
      [](Panel& state, berth::Result<EditSave>&& outcome) {
        state.edit_running = false;
        if (outcome.is_err()) {
          state.edit_error = outcome.error().ctx();
          state.edit_status = "the edit was NOT saved";
          return;
        }
        EditSave save = std::move(outcome).value();
        state.edit_saved = true;
        state.edit_stored_path = save.edit.stored_path;
        state.edit_diff = std::move(save.diff);
        state.edit_status = "saved";
        state.edit_error.clear();
        state.catalog = std::move(save.catalog);
        if (state.catalog_cursor >= state.catalog.recipes.size()) {
          state.catalog_cursor = 0;
        }
      });
}

DraftKey handle_draft_clash_key(Panel& panel, const std::string& key) {
  if (key == "y" || key == "Y") {
    panel.draft_clash_confirming = false;
    return DraftKey::kConfirmed;
  }
  if (key == "n" || key == "N" || key == "\x1b") {
    panel.draft_clash_confirming = false;
    return DraftKey::kDismissed;
  }
  return DraftKey::kEaten;
}

namespace {

// Bounded (§5): drawing 20k lines takes a second to redraw.
constexpr std::size_t kLogViewLines = 1000;
constexpr std::size_t kLogVisibleRows = 14;

std::filesystem::path log_path_of(const berth::Paths& paths,
                                  const LogFile file) {
  switch (file) {
    case LogFile::kDeploy:
      return paths.deploy_log;
    case LogFile::kNet:
      return paths.net_log;
    case LogFile::kCrash:
      return paths.crash_log;
  }
  abort_bug("unknown LogFile");
}

}
void refresh_logs(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths) {
  LogFile file = LogFile::kDeploy;
  std::size_t generation = 0;
  {
    std::lock_guard lock(panel->mutex);
    // The generation makes a stale reply a no-op: a switch never loses a load.
    file = panel->log_selected;
    panel->log_loading = true;
    panel->log_error.clear();
    generation = ++panel->log_generation;
  }
  const std::filesystem::path path = log_path_of(paths, file);
  run_on_worker<berth::Tail>(
      panel, "log tail",
      [path](const std::shared_ptr<Op>& /*op*/) {
        return berth::tail_lines(path, kLogViewLines);
      },
      [file, generation](Panel& state, berth::Result<berth::Tail>&& tail) {
        if (generation != state.log_generation) {
          return;
        }
        state.log_loading = false;
        if (state.log_counts.size() != kLogFileCount) {
          state.log_counts.assign(kLogFileCount, std::nullopt);
        }
        const std::size_t slot = log_file_index(file);
        if (tail.is_err()) {
          // An unreadable log is NOT an empty one: the buffer is cleared.
          state.log_error = tail.error().ctx();
          state.logs_lines.clear();
          state.log_counts[slot] = std::nullopt;
          state.log_existed = false;
          state.log_truncated = false;
          return;
        }
        state.log_error.clear();
        state.log_existed = tail.value().existed;
        state.log_truncated = tail.value().truncated;
        state.logs_lines.clear();
        for (const std::string& line : tail.value().lines) {
          state.logs_lines.push_back(line);
        }
        state.log_counts[slot] = state.logs_lines.size();
      });
}

Element render_logs_screen(const Panel& panel) {
  Elements out;
  out.push_back(ftxui::text("LOGS") | ftxui::bold |
                ftxui::color(Color::Yellow));
  out.push_back(
      ftxui::text(log_tabs_line(panel.log_selected, panel.log_counts)) |
      ftxui::dim);
  out.push_back(ftxui::text(std::string("  ") + std::string(log_file_path_hint(
                                                    panel.log_selected))) |
                ftxui::dim);
  const berth::NetSnapshot net = berth::netstatus_snapshot();
  out.push_back(ftxui::text("  " + net_banner_note(net)) | ftxui::dim);
  out.push_back(ftxui::separator());

  if (panel.log_loading) {
    out.push_back(ftxui::text("\xe2\x8b\xb0 reading the tail...") | ftxui::dim);
    return ftxui::vbox(std::move(out)) | ftxui::flex;
  }
  if (!panel.log_error.empty()) {
    out.push_back(
        paragraph_lines(log_error_lines(panel.log_selected, panel.log_error)) |
        ftxui::color(Color::Red));
  } else if (panel.logs_lines.empty()) {
    out.push_back(paragraph_lines(log_empty_lines(panel.log_selected)));
  } else {
    Elements meta;
    meta.push_back(
        ftxui::text(std::to_string(panel.logs_lines.size()) + " lines" +
                    (panel.log_scroll == 0
                         ? ", pinned to the newest"
                         : ", scrolled up " + std::to_string(panel.log_scroll) +
                               " lines")) |
        ftxui::dim);
    if (panel.log_truncated) {
      meta.push_back(ftxui::text("  (the file is longer than the read window; "
                                 "this is its tail)") |
                     ftxui::dim);
    }
    if (panel.logs_lines.dropped() > 0) {
      meta.push_back(ftxui::text("  (" +
                                 std::to_string(panel.logs_lines.dropped()) +
                                 " older lines dropped)") |
                     ftxui::dim);
    }
    out.push_back(ftxui::hbox(std::move(meta)));
    out.push_back(log_window(panel.logs_lines, panel.log_scroll,
                             kLogVisibleRows, Color::Yellow));
  }
  out.push_back(ftxui::separator());
  out.push_back(key_hint_line({{"1 2 3", "switch log"},
                               {"r", "reload"},
                               {"\xe2\x86\x91\xe2\x86\x93", "scroll"},
                               {"End", "pinned to the newest"}}));
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

void refresh_services(const std::shared_ptr<Panel>& panel,
                      const berth::Paths& paths, const bool background) {
  {
    std::lock_guard lock(panel->mutex);
    // An action owns the rows until its re-list lands (§11 stale-row bug).
    if (panel->services_loading || panel->services_auto ||
        !panel->action.empty()) {
      return;
    }
    if (background) {
      // Silent: no status line and no loading flag, so a tick cannot clobber.
      panel->services_auto = true;
    } else {
      panel->services_loading = true;
      panel->services_status = "querying systemctl --user is-active...";
    }
  }
  run_on_worker<std::vector<berth::ServiceRow>>(
      panel, "service refresh",
      [paths](const std::shared_ptr<Op>& op) {
        return berth::service_rows(paths, &op->cancel);
      },
      [](Panel& state, berth::Result<std::vector<berth::ServiceRow>>&& rows) {
        state.services_loading = false;
        state.services_auto = false;
        if (rows.is_err()) {
          state.services_status = rows.error().ctx();
          return;
        }
        const bool dialog_open =
            state.confirming_uninstall || state.confirming_kill;
        state.services_cursor = cursor_after_rows_refresh(
            state.services_cursor, state.rows, rows.value());
        state.rows = std::move(rows).value();
        // A dialog owns the screen: the cursor is frozen while it is up.
        if (dialog_open && state.services_cursor >= state.rows.size()) {
          state.services_cursor = 0;
        }
        if (state.services_status.rfind("querying", 0) == 0) {
          state.services_status.clear();
        }
      });
}

namespace {

std::string row_is_frozen_after(const bool frozen_now, const bool freezing,
                                const std::string& service_name) {
  if (frozen_now) {
    return service_name +
           " is frozen: it holds its port and answers nothing until you "
           "unfreeze it";
  }
  if (freezing) {
    return service_name + " did not freeze (systemd says it is still " +
           "running) — nothing was changed";
  }
  return service_name + " is running again";
}

// Action and re-list on ONE worker; re-listing here avoids a self-deadlock.
void service_action(const std::shared_ptr<Panel>& panel,
                    const std::string& label,
                    const std::function<berth::Result<void>()>& op) {
  berth::Paths paths;
  {
    std::lock_guard lock(panel->mutex);
    // One action at a time, never against rows a refresh is mid-replacing.
    if (!panel->action.empty() || panel->services_loading ||
        panel->services_auto) {
      return;
    }
    panel->action = label;
    paths = panel->paths;
  }
  run_on_worker<std::vector<berth::ServiceRow>>(
      panel, label,
      [op, paths](const std::shared_ptr<Op>& registered)
          -> berth::Result<std::vector<berth::ServiceRow>> {
        const berth::Result<void> done = op();
        if (done.is_err()) {
          return std::move(done).error();
        }
        return berth::service_rows(paths, &registered->cancel);
      },
      [label](Panel& state,
              berth::Result<std::vector<berth::ServiceRow>>&& refreshed) {
        state.action.clear();
        if (refreshed.is_err()) {
          state.services_status = label +
                                  ": ok (the list could not be refreshed: " +
                                  refreshed.error().ctx() + ")";
          return;
        }
        const bool dialog_open =
            state.confirming_uninstall || state.confirming_kill;
        state.services_cursor = cursor_after_rows_refresh(
            state.services_cursor, state.rows, refreshed.value());
        state.rows = std::move(refreshed).value();
        if (dialog_open && state.services_cursor >= state.rows.size()) {
          state.services_cursor = 0;
        }
        state.services_status = label + ": ok";
      });
}

}
void service_action_start(const std::shared_ptr<Panel>& panel,
                          const std::string& service_name) {
  service_action(panel, "starting " + service_name, [service_name]() {
    return berth::service_start(service_name);
  });
}

void service_action_stop(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name) {
  service_action(panel, "stopping " + service_name, [service_name]() {
    return berth::service_stop(service_name);
  });
}

void service_action_restart(const std::shared_ptr<Panel>& panel,
                            const std::string& service_name) {
  service_action(panel, "restarting " + service_name, [service_name]() {
    return berth::service_restart(service_name);
  });
}

void service_action_kill(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name,
                         const bool confirmed) {
  if (!confirmed) {
    return;
  }
  service_action(panel, "killing " + service_name, [service_name]() {
    return berth::service_kill(service_name);
  });
}

void service_action_freeze(const std::shared_ptr<Panel>& panel,
                           const std::string& service_name) {
  bool freezing = true;
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->action.empty() || panel->services_loading ||
        panel->services_auto) {
      return;
    }
    for (const berth::ServiceRow& row : panel->rows) {
      if (row.service_name == service_name) {
        freezing = !row.frozen;
      }
    }
    panel->action = (freezing ? "freezing " : "unfreezing ") + service_name;
  }
  // A signal that was sent is not a state, so the freeze reports a BOOL.
  run_on_worker<bool>(
      panel, "freeze",
      [service_name, freezing](const std::shared_ptr<Op>& /*op*/) {
        return freezing ? berth::service_freeze(service_name)
                        : berth::service_unfreeze(service_name);
      },
      [service_name, freezing](Panel& state, berth::Result<bool>&& frozen) {
        state.action.clear();
        if (frozen.is_err()) {
          state.services_status = frozen.error().ctx();
          return;
        }
        for (berth::ServiceRow& row : state.rows) {
          if (row.service_name != service_name) {
            continue;
          }
          row.frozen = frozen.value();
        }
        state.services_status =
            row_is_frozen_after(frozen.value(), freezing, service_name);
      });
}

void service_action_uninstall(const std::shared_ptr<Panel>& panel,
                              const berth::Paths& paths,
                              const std::string& service_name) {
  service_action(panel, "uninstalling " + service_name,
                 [paths, service_name]() {
                   return berth::service_uninstall(paths, service_name);
                 });
}

Element render_services_screen(const Panel& panel) {
  Elements out;
  out.push_back(ftxui::text("MY SERVICES") | ftxui::bold |
                ftxui::color(Color::Green));

  Elements status;
  if (!panel.action.empty()) {
    status.push_back(ftxui::text("\xe2\x8b\xb0 " + panel.action + "..."));
  } else if (panel.services_loading) {
    status.push_back(ftxui::text("\xe2\x8b\xb0 " + panel.services_status));
  } else if (!panel.services_status.empty()) {
    const bool ok = panel.services_status.find(": ok") != std::string::npos;
    status.push_back(ftxui::text(panel.services_status) |
                     ftxui::color(ok ? Color::Green : Color::Red));
  } else {
    status.push_back(ftxui::text("idle"));
  }
  out.push_back(ftxui::hbox(std::move(status)) | ftxui::dim);
  out.push_back(ftxui::separator());

  if (panel.rows.empty()) {
    out.push_back(paragraph_lines(services_empty_lines()) | ftxui::flex);
  } else {
    // Header row is not selectable: the highlight would be one row off.
    out.push_back(ftxui::text(services_header_line()) | ftxui::dim);
    out.push_back(line_list(services_row_lines(panel.rows),
                            panel.services_cursor, Color::Green, true,
                            kVisibleRows - 1));
  }
  out.push_back(ftxui::separator());
  for (const KeyHint& hint : services_key_hints()) {
    out.push_back(key_hint(hint));
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

Element confirm_uninstall_screen(const Panel& panel) {
  return boxed_dialog(confirm_uninstall_lines(panel.uninstall_target),
                      Color::Red);
}

Element confirm_kill_screen(const Panel& panel) {
  return boxed_dialog(confirm_kill_lines(panel.kill_target), Color::Red);
}

UninstallKey handle_kill_key(Panel& panel, const std::string& key) {
  if (key == "y" || key == "Y") {
    panel.confirming_kill = false;
    return UninstallKey::kConfirmed;
  }
  if (key == "q") {
    panel.confirming_kill = false;
    return UninstallKey::kQuit;
  }
  if (key == "n" || key == "N" || key == "\x1b") {
    panel.confirming_kill = false;
    return UninstallKey::kDismissed;
  }
  return UninstallKey::kEaten;
}

UninstallKey handle_uninstall_key(Panel& panel, const std::string& key) {
  if (key == "y" || key == "Y") {
    panel.confirming_uninstall = false;
    return UninstallKey::kConfirmed;
  }
  if (key == "q") {
    panel.confirming_uninstall = false;
    return UninstallKey::kQuit;
  }
  if (key == "n" || key == "N" || key == "\x1b") {
    panel.confirming_uninstall = false;
    return UninstallKey::kDismissed;
  }
  return UninstallKey::kEaten;
}

}
