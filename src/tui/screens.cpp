// src/tui/screens.cpp — see screens.h for the contract.

#include "screens.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <thread>

#include "../berth/deploy.h"
#include "../berth/service_ops.h"
#include "../berth/wizard.h"
#include "../core/netstatus.h"
#include "../util/result.h"
#include "../util/strutil.h"
#include "../util/tail.h"

namespace tui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

// How often the deploy tail reads the log the ENGINE is already writing. The
// file is the sync — there is no second sink to keep in step with it (§5: the
// deploy log belongs in a neutral module, never in tui/).
constexpr auto kDeployTailPoll = std::chrono::milliseconds(250);
// The tail gives up waiting for the deploy after this long, so a stuck reader
// cannot hold a finished deploy's result hostage.
constexpr std::chrono::milliseconds kTailDrainBudget{2000};
// The tail must know where this deploy's log starts before the engine writes
// its first line; the worker waits this long for it to say so.
constexpr std::chrono::milliseconds kTailReadyBudget{1000};

constexpr std::size_t kDeployListRows = 6;
constexpr std::size_t kDeployTailRows = 8;
// While a draft is on screen the recipe list and the log tail give up their
// rows: the review needs them, and a vbox that overflows DROPS rows — which
// silently eats the port field.
constexpr std::size_t kDraftListRows = 2;
constexpr std::size_t kDraftTailRows = 3;

}  // namespace

// --- Deploy (6.3) -----------------------------------------------------------

namespace {

// Appends freshly read lines to the screen's ring buffer and folds the newest
// stage in. Under the mutex, like every other write to the view state.
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

// The tail's handshake with the deploy worker. Shared, not stack-allocated:
// the worker outlives start_deploy(), so a captured reference would dangle.
struct TailGate {
  std::atomic<bool> ready{false};    // the tail knows where this log starts
  std::atomic<bool> drained{false};  // the tail has read its last line
};

// Reads the deploy log from wherever the last poll stopped and pushes whole
// lines into the ring. Returns nothing: a vanished log is not an error the
// screen can act on, and the engine's own result is the verdict.
void follow_deploy_log(const std::shared_ptr<Panel>& panel,
                       const std::filesystem::path& log_path,
                       const std::shared_ptr<TailGate>& gate) {
  const std::shared_ptr<Panel> keep_alive = panel;
  std::thread tail([keep_alive, log_path, gate]() {
    // Where this deploy's lines begin. If the log already exists we start at
    // its current end — this screen shows THIS deploy, not the history of every
    // deploy that ever ran. If it does not exist yet it is created by the
    // engine's first append, and the start offset is 0: exactly the beginning
    // of this deploy's own lines.
    std::size_t start_offset = 0;
    std::FILE* file = std::fopen(log_path.c_str(), "r");
    if (file != nullptr && std::fseek(file, 0, SEEK_END) == 0) {
      const long end = std::ftell(file);
      start_offset = end > 0 ? static_cast<std::size_t>(end) : 0;
    }

    std::string partial;  // a line split across two reads stays one line
    std::vector<std::string> fresh;
    const auto read_new = [&file, &partial, &fresh]() {
      if (file == nullptr) {
        return;
      }
      char buffer[8192];
      std::size_t got = 0;
      while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        partial.append(buffer, got);
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
      // The log is created on the engine's first append, which can happen
      // after this reader starts. A missing file is not "no progress to show",
      // it is "the first line has not been written yet" — so keep trying
      // instead of giving up and losing the whole stage ladder (§11).
      if (file == nullptr) {
        file = std::fopen(log_path.c_str(), "r");
        if (file != nullptr) {
          static_cast<void>(
              std::fseek(file, static_cast<long>(start_offset), SEEK_SET));
        }
      }
      read_new();
      flush();
      std::this_thread::sleep_for(kDeployTailPoll);
    }
    // One last pass: the engine wrote the done/failed lines while we slept.
    if (file == nullptr) {
      file = std::fopen(log_path.c_str(), "r");
      if (file != nullptr) {
        static_cast<void>(
            std::fseek(file, static_cast<long>(start_offset), SEEK_SET));
      }
    }
    if (file != nullptr) {
      read_new();
      flush();
      std::fclose(file);
    }
    gate->drained.store(true, std::memory_order_release);
  });
  tail.detach();
}

}  // namespace

void start_deploy(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const berth::Recipe& recipe,
                  const std::uint16_t forced_port, DeployPreStore pre_store) {
  {
    std::lock_guard lock(panel->mutex);
    // One deploy at a time, in the UI. The engine's deploy.lock is the second
    // line of defence (against a deploy started from the CLI), not the first:
    // flock contention would be discovered only after the fetch stage began.
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

  // The tail goes first: it must know the log's end offset before the engine
  // writes "fetch ...", or this deploy's first line is lost to the race.
  const std::shared_ptr<TailGate> gate = std::make_shared<TailGate>();
  follow_deploy_log(panel, paths.deploy_log, gate);

  run_on_worker<berth::DeployOutcome>(
      panel, "deploy " + recipe.service_name,
      [paths, recipe, forced_port, gate, panel,
       pre_store](const std::shared_ptr<Op>& op) {
        {
          // Publish the op BEFORE the tail-ready wait. `c` reaches the engine
          // only through panel->deploy_op; assigning it after a wait of up to
          // a second let a cancel in that window flip nothing and the deploy
          // run to completion while the screen said "cancelling".
          std::lock_guard lock(panel->mutex);
          panel->deploy_op = op;
        }
        static_cast<void>(wait_for_flag(gate->ready, kTailReadyBudget));
        if (pre_store) {
          // The store runs here, on the deploy's own worker, so it is FINISHED
          // before the first byte of the deploy. It used to run on its own
          // worker while start_deploy was called synchronously right after, so
          // a failed store still deployed the draft.
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
        // Tell the tail to stop, then WAIT for it, so the done/failed lines
        // land in the ring before the verdict is folded into the state. The
        // wait is bounded: a wedged reader must not hold the result back.
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

namespace {

// The index of a field, for the marker row above (kNone renders no marker).
DraftField draft_field_at(const std::size_t index) {
  switch (index) {
    case 0:
      return DraftField::kUrl;
    case 1:
      return DraftField::kRef;
    default:
      return DraftField::kPort;
  }
}

}  // namespace

Element render_deploy_screen(const Panel& panel, ftxui::Element url_input,
                             ftxui::Element ref_input,
                             ftxui::Element port_input) {
  // The overwrite question replaces the screen, like every other dialog: it is
  // the one thing that must not be read past.
  if (panel.draft_clash_confirming) {
    return boxed_dialog(
        draft_clash_lines(panel.draft_clash, panel.draft.commit),
        Color::Yellow);
  }
  const bool drafting = panel.draft_running || panel.draft_ready;
  Elements out;
  out.push_back(ftxui::text("DEPLOY") | ftxui::bold |
                ftxui::color(Color::Magenta));

  // A static ellipsis, never an animated spinner: the panel has no animation
  // clock, and a spinner that does not tick lies about progress.
  Elements status;
  if (panel.deploy_running.load(std::memory_order_relaxed)) {
    const bool cancelling =
        panel.deploy_cancel_requested.load(std::memory_order_relaxed);
    status.push_back(ftxui::text(std::string("\xe2\x8b\xb0 ") +
                                 (cancelling ? "cancelling " : "deploying ") +
                                 panel.deploy_service + "...") |
                     ftxui::color(cancelling ? Color::Yellow : Color::Magenta));
  } else if (!panel.deploy_status.empty()) {
    // A cancellation is not a failure: the engine returns an Error to unwind,
    // but the status stays yellow and no "deploy failed:" box is drawn.
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

  if (!panel.catalog_error.empty()) {
    // A catalog that failed to load must say so (§11). The old screen fell
    // through to an empty list and showed the user nothing at all.
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
    // Header and selectable rows are SEPARATE: line_list marks its own cursor
    // row, and panel.catalog_cursor indexes panel.catalog.recipes, so the
    // header must not be part of the vector the cursor walks. One row of the
    // budget is the header, hence the -1.
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

  // The three text fields. The labels, the values and the focus marker are all
  // laid out in one line each: an Input only draws its own box, so a bare
  // hbox of three of them would stack them instead.
  const bool locked = panel.deploy_running.load(std::memory_order_relaxed) ||
                      panel.draft_running;
  {
    // The marker and the label are ours; the VALUE is the Input's own render,
    // which brings its cursor with it. Each row is flex so the Input takes
    // whatever width is left instead of wrapping onto the next field's line.
    const ftxui::Element fields[3] = {url_input | ftxui::xflex,
                                      ref_input | ftxui::xflex,
                                      port_input | ftxui::xflex};
    for (std::size_t i = 0; i < 3; ++i) {
      const bool focused = panel.draft_field == draft_field_at(i);
      Elements row;
      row.push_back(ftxui::text(focused ? "\xe2\x96\xb8 " : "  ") |
                    ftxui::color(focused ? Color::Magenta : Color::White));
      row.push_back(
          ftxui::text(std::string(draft_field_label(draft_field_at(i))) +
                      "   ") |
          ftxui::dim);
      row.push_back(fields[i]);
      out.push_back(ftxui::hbox(std::move(row)) | ftxui::flex);
    }
    out.push_back(locked ? ftxui::text("  fields are locked while work is in "
                                       "flight")
                         : ftxui::text("  i  edit  ·  empty port = the "
                                       "recipe's default or a free one"));
  }
  out.push_back(ftxui::separator());

  // The ladder is the deploy's progress; while a draft is being reviewed it
  // would just be five lines of "nothing yet".
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
  const std::vector<std::string> hints =
      panel.draft_field != DraftField::kNone || panel.draft_running ||
              panel.draft_ready
          ? draft_key_hints(panel.draft_field, panel.draft_running,
                            panel.draft_ready)
          : deploy_key_hints(
                panel.deploy_running.load(std::memory_order_relaxed));
  for (const std::string& hint : hints) {
    out.push_back(ftxui::paragraph(hint) | ftxui::dim);
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

// --- Catalog (6.2) ---------------------------------------------------------

void refresh_catalog(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->catalog_loading) {
      return;
    }
    panel->catalog_loading = true;
    panel->catalog_error.clear();
  }
  run_on_worker<Catalog>(
      panel, "catalog load",
      [paths](const std::shared_ptr<Op>& op) -> berth::Result<Catalog> {
        std::atomic<bool>* cancel = &op->cancel;
        // Cancelled before it started is not a failure worth rendering; the
        // load simply never happens.
        if (cancel->load()) {
          return berth::Error("cancelled before the catalog read");
        }
        return load_catalog(paths);
      },
      [](Panel& state, berth::Result<Catalog>&& loaded) {
        state.catalog_loading = false;
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

// --- System Scan (6.5) -----------------------------------------------------

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
          // The scan itself failed (not one of its sections). The previous
          // report stays on screen next to this text, so a failing re-run does
          // not erase what was known.
          state.scan_error = report.error().ctx();
          return;
        }
        state.scan_error.clear();
        state.scan = std::move(report).value();
      });
}

namespace {

// One titled section of the report.
Element scan_section(const std::string& title,
                     const std::vector<std::string>& lines) {
  Elements out;
  out.push_back(ftxui::text(title) | ftxui::bold | ftxui::color(Color::Cyan));
  out.push_back(paragraph_lines(lines));
  out.push_back(ftxui::separator());
  return ftxui::vbox(std::move(out));
}

}  // namespace

Element render_scan_screen(const Panel& panel) {
  Elements out;
  out.push_back(ftxui::text("SYSTEM SCAN") | ftxui::bold |
                ftxui::color(Color::Cyan));
  // The same honesty as everywhere else: a static ellipsis, never an animated
  // spinner the panel has no clock to advance.
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

  // The network alert gets its own emphasis: it is the only section whose
  // failure the user can do something about, and the scan completes anyway.
  const std::string alert = scan_network_alert(panel.scan);
  if (!alert.empty()) {
    out.push_back(paragraph_lines(berth::split(alert, "\n")) |
                  ftxui::borderStyled(ftxui::ROUNDED, Color::Yellow));
    out.push_back(ftxui::separator());
  }

  if (panel.scan_running && !panel.scan_ever_ran) {
    // Nothing collected yet: the idle text is more useful than four empty
    // sections.
    out.push_back(paragraph_lines({"first run in progress..."}) | ftxui::dim);
    return ftxui::vbox(std::move(out)) | ftxui::flex;
  }

  out.push_back(scan_section("BUILD TOOLS THE CATALOG NEEDS",
                             scan_prereq_lines(panel.scan)));
  out.push_back(scan_section("PORTS IN USE", scan_port_lines(panel.scan)));
  out.push_back(scan_section("YOUR SERVICES", scan_service_lines(panel.scan)));
  out.push_back(scan_section("NETWORK", scan_network_lines(panel.scan)));
  out.push_back(ftxui::paragraph("r  run the scan again") | ftxui::dim);
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

// --- BYO wizard (plan 4.21) ------------------------------------------------

namespace {

// What the draft worker hands back: the draft AND the clash answer. Both are
// computed on the worker, in one pass — the clash needs the detected
// service_name, so it cannot be asked before the clone, and asking it from the
// render thread would mean file reads there (§5).
struct DraftPrepared {
  berth::DraftResult draft;
  berth::DraftClash clash;
  std::string error;  // empty when the draft was made
};

}  // namespace

void start_draft(const std::shared_ptr<Panel>& panel,
                 const berth::Paths& paths) {
  std::string url;
  std::string ref;
  {
    std::lock_guard lock(panel->mutex);
    if (panel->draft_running || panel->deploy_in_flight) {
      return;
    }
    url = berth::trim(panel->draft_url);
    ref = berth::trim(panel->draft_ref);
    if (url.empty()) {
      // Silence on an empty field: there is nothing to say about nothing, and
      // the field is on screen with its hint.
      return;
    }
    panel->draft_running = true;
    panel->draft_ready = false;
    panel->draft_stored = false;
    panel->draft_stored_path.clear();
    panel->draft_error.clear();
    panel->draft_clash = berth::DraftClash{};
    panel->draft_status =
        "resolving " + url + (ref.empty() ? "" : "@" + ref) + "...";
  }

  const std::shared_ptr<Panel> keep_alive = panel;
  run_on_worker<DraftPrepared>(
      panel, "recipe draft",
      [paths, url, ref, keep_alive](const std::shared_ptr<Op>& op) {
        {
          // Publish the op so 'c' reaches the draft's own flag while it runs;
          // it is reset in the apply once the draft is no longer in flight.
          std::lock_guard lock(keep_alive->mutex);
          keep_alive->draft_op = op;
        }
        // Progress from the worker lands in the status line under the panel
        // lock, and asks for a redraw: the render thread reads it next frame.
        const berth::DraftProgress progress =
            [keep_alive](const std::string& line) {
              std::lock_guard lock(keep_alive->mutex);
              keep_alive->draft_status = line;
              request_render(*keep_alive);
            };
        berth::Result<berth::DraftResult> prepared = berth::prepare_draft(
            paths, berth::DraftRequest{.url = url, .ref = ref}, &op->cancel,
            progress);
        DraftPrepared out;
        if (prepared.is_err()) {
          out.error = prepared.error().ctx();
          return out;
        }
        out.draft = std::move(prepared).value();
        const berth::Result<berth::DraftClash> clash =
            berth::find_recipe_clash(paths, out.draft.draft.service_name);
        if (clash.is_err()) {
          // A clash we could not determine is treated as a clash: a store that
          // cannot check the name has to ask, never assume it is free.
          out.clash.exists = true;
          out.clash.service_name = out.draft.draft.service_name;
        } else {
          out.clash = clash.value();
        }
        return out;
      },
      [](Panel& state, berth::Result<DraftPrepared>&& prepared) {
        state.draft_running = false;
        state.draft_op.reset();
        if (!prepared.value().error.empty()) {
          state.draft_ready = false;
          state.draft_error = prepared.value().error;
          state.draft_status = "the draft could not be made";
          return;
        }
        state.draft_error.clear();
        state.draft = std::move(prepared).value().draft;
        state.draft_clash = std::move(prepared).value().clash;
        state.draft_ready = true;
        state.draft_status = "review it, then d to store and deploy";
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
    // A second 's'/'d' while a store, a draft or a deploy is in flight must not
    // spawn a second writer over the same file.
    if (!panel->draft_ready || panel->draft_stored || panel->draft_running ||
        panel->draft_storing || panel->deploy_in_flight) {
      return;
    }
    if (panel->draft_clash.exists && !force) {
      clash_needs_an_answer = true;
    } else {
      recipe = panel->draft.draft;
      const berth::Result<std::uint16_t> parsed =
          parse_port_field(panel->deploy_port_field);
      if (parsed.is_err()) {
        panel->draft_error = parsed.error().ctx();
        panel->draft_status = "nothing was stored";
        return;
      }
      port = std::move(parsed).value();
    }
  }
  if (clash_needs_an_answer) {
    // The question, not the overwrite: the store has not happened yet, and it
    // will not happen until the user says so (§8).
    std::lock_guard lock(panel->mutex);
    panel->draft_clash_confirming = true;
    panel->draft_pending_deploy = deploy_after;
    return;
  }

  if (deploy_after) {
    // Store and deploy are ONE worker: start_deploy's worker runs pre_store to
    // completion before run_deploy, so a refused store aborts the deploy.
    // Starting the store on its own worker and calling start_deploy right after
    // raced the two and deployed a draft that was never written (wizard.h).
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
  run_on_worker<std::filesystem::path>(
      panel, "store draft",
      [paths, recipe, force](const std::shared_ptr<Op>& /*op*/) {
        return berth::store_draft(paths, recipe, force);
      },
      [](Panel& state, berth::Result<std::filesystem::path>&& stored) {
        state.draft_clash_confirming = false;
        state.draft_storing = false;
        if (stored.is_err()) {
          state.draft_error = stored.error().ctx();
          state.draft_status = "the draft was NOT stored";
          return;
        }
        state.draft_stored = true;
        state.draft_stored_path = stored.value();
        state.draft_error.clear();
        state.draft_status = "stored";
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

DraftKey handle_draft_clash_key(Panel& panel, const std::string& key) {
  if (key == "y" || key == "Y") {
    panel.draft_clash_confirming = false;
    return DraftKey::kConfirmed;
  }
  if (key == "n" || key == "N" || key == "\x1b") {
    panel.draft_clash_confirming = false;
    return DraftKey::kDismissed;
  }
  return DraftKey::kEaten;  // the modal is modal
}

// --- Logs (6.6) ------------------------------------------------------------

namespace {

// How many lines a log view holds. Bounded because the buffer is a ring
// (§5) and because a screen that tries to draw 20k lines is a screen that
// takes a second to redraw.
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

}  // namespace

void refresh_logs(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths) {
  LogFile file = LogFile::kDeploy;
  std::size_t generation = 0;
  {
    std::lock_guard lock(panel->mutex);
    // A newer request SUPERSEDES one in flight instead of being dropped. The
    // old behavior returned early when a tail was loading, so switching 1→2
    // while file 1 was being read left file 2 unloaded and then wrote file 1's
    // lines under file 2's selected tab. The generation makes the stale reply
    // a no-op and lets the new file load immediately.
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
        // A reply for a file the user has switched away from is dropped: the
        // screen already cleared for the new selection, and writing this under
        // it would show one file's tail under another file's name.
        if (generation != state.log_generation) {
          return;
        }
        state.log_loading = false;
        if (state.log_counts.size() != kLogFileCount) {
          state.log_counts.assign(kLogFileCount, std::nullopt);
        }
        const std::size_t slot = log_file_index(file);
        if (tail.is_err()) {
          // An unreadable log is NOT an empty one: the buffer is cleared so the
          // screen cannot show a stale tail next to an error about this file.
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
        // The scroll position is left alone on purpose: a reload must not yank
        // the view back to the bottom, and log_window clamps a scroll that is
        // now past the top of a shorter file.
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
  // What the header's NET number means, spelled out once, here where there is
  // room for it. The banner itself is only the count.
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
    // No error and no lines: the file is genuinely empty (or absent, which is
    // the same thing for crash.log). Tail.h says so in words per file.
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
      // A log longer than the window is not a log the panel can show whole,
      // and pretending otherwise would be a silent lie.
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
  out.push_back(ftxui::paragraph("1 2 3  switch log  ·  r  reload  ·  "
                                 "↑↓ scroll  ·  End pinned to the newest") |
                ftxui::dim);
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

// --- My Services (6.4) ----------------------------------------------------

void refresh_services(const std::shared_ptr<Panel>& panel,
                      const berth::Paths& paths) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->services_loading || !panel->action.empty()) {
      return;
    }
    panel->services_loading = true;
    panel->services_status = "querying systemctl --user is-active...";
  }
  run_on_worker<std::vector<berth::ServiceRow>>(
      panel, "service refresh",
      [paths](const std::shared_ptr<Op>& op) {
        return berth::service_rows(paths, &op->cancel);
      },
      [](Panel& state, berth::Result<std::vector<berth::ServiceRow>>&& rows) {
        state.services_loading = false;
        if (rows.is_err()) {
          state.services_status = rows.error().ctx();
          return;
        }
        const bool dialog_open =
            state.confirming_uninstall || state.confirming_kill;
        state.services_cursor = cursor_after_rows_refresh(
            state.services_cursor, state.rows, rows.value());
        state.rows = std::move(rows).value();
        // While a confirm dialog is up it owns the screen: the cursor is frozen
        // so the highlight cannot walk off the service the dialog is naming.
        // An index left stale by that freeze is repaired on the next refresh.
        if (dialog_open && state.services_cursor >= state.rows.size()) {
          state.services_cursor = 0;
        }
        if (state.services_status.rfind("querying", 0) == 0) {
          state.services_status.clear();
        }
      });
}

namespace {

// What the freeze/unfreeze did, said in words: a frozen service holds its port,
// which is the part a user cannot un-derive from "active".
std::string row_is_frozen_after(const bool frozen_now, const bool freezing,
                                const std::string& service_name) {
  if (frozen_now) {
    return service_name +
           " is frozen: it holds its port and answers nothing until you "
           "unfreeze it";
  }
  if (freezing) {
    // The signal went out and the substate says it did not take: say THAT
    // rather than claim a freeze that never happened (§11).
    return service_name + " did not freeze (systemd says it is still " +
           "running) — nothing was changed";
  }
  return service_name + " is running again";
}

// start / stop / restart / kill / uninstall share one shape: a label, one
// blocking call, and the outcome folded into the status line. Only the call
// differs. The action and the re-list run on ONE worker and land in ONE apply,
// so the row the user just acted on can never be left showing its old state
// (start still reading "inactive", an uninstalled service still listed). Doing
// the re-list here also avoids taking panel.mutex again from inside the apply,
// which would self-deadlock.
void service_action(const std::shared_ptr<Panel>& panel,
                    const std::string& label,
                    const std::function<berth::Result<void>()>& op) {
  berth::Paths paths;
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->action.empty()) {
      return;  // one action at a time: a second press is a no-op, not a race
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
          // The action succeeded but the re-list did not: say both, and leave
          // the old rows rather than claim a state we could not read.
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

}  // namespace

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
  // The label depends on what the row believes now, so the status line says
  // "freezing" only when the service is actually running.
  bool freezing = true;
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->action.empty()) {
      return;
    }
    for (const berth::ServiceRow& row : panel->rows) {
      if (row.service_name == service_name) {
        freezing = !row.frozen;
      }
    }
    panel->action = (freezing ? "freezing " : "unfreezing ") + service_name;
  }
  // The freeze reports back a BOOL — a signal that was sent is not a state —
  // so it does not fit service_action()'s Result<void> shape.
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
          // Both verbs return "true = frozen now", confirmed against the
          // process: the row shows what IS, not what we asked for.
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

  // A static ellipsis, not an animated spinner: the panel has no animation
  // clock, and a spinner that never ticks lies about progress.
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
    // Same split as the catalog list: the header is not selectable, or the
    // highlight and the action would be one row apart.
    out.push_back(ftxui::text(services_header_line()) | ftxui::dim);
    out.push_back(line_list(services_row_lines(panel.rows),
                            panel.services_cursor, Color::Green, true,
                            kVisibleRows - 1));
  }
  out.push_back(ftxui::separator());
  for (const std::string& hint : services_key_hints()) {
    out.push_back(ftxui::paragraph(hint) | ftxui::dim);
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

// The kill dialog reuses the uninstall dialog's key rules: same shape, same
// "the caller acts" contract. A second near-identical enum would be a trap.
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
    // "q" keeps meaning quit; the dialog only lets it out.
    panel.confirming_uninstall = false;
    return UninstallKey::kQuit;
  }
  if (key == "n" || key == "N" || key == "\x1b") {
    panel.confirming_uninstall = false;
    return UninstallKey::kDismissed;
  }
  return UninstallKey::kEaten;  // anything else is eaten by the modal
}

}  // namespace tui