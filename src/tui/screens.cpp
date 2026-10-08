// src/tui/screens.cpp — see screens.h for the contract.

#include "screens.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <thread>

#include "../berth/deploy.h"
#include "../berth/service_ops.h"
#include "../util/result.h"

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

// Waits for a flag in slices, so a cancel or a shutdown is noticed promptly
// instead of after a full sleep. A worker may wait; the render thread may not.
bool wait_bounded(const std::atomic<bool>& flag,
                  const std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!flag.load(std::memory_order_relaxed)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

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
    std::FILE* file = std::fopen(log_path.c_str(), "r");
    if (file == nullptr) {
      // No log yet (it is created on the first append). Say so and leave: the
      // deploy still runs and its result still lands.
      gate->ready.store(true, std::memory_order_relaxed);
      gate->drained.store(true, std::memory_order_relaxed);
      return;
    }
    // Start at EOF: this screen shows THIS deploy, not the history of every
    // deploy that ever ran on this machine.
    std::fseek(file, 0, SEEK_END);

    std::string partial;  // a line split across two reads stays one line
    std::vector<std::string> fresh;
    const auto read_new = [&file, &partial, &fresh]() {
      char buffer[8192];
      bool any = false;
      std::size_t got = 0;
      while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        any = true;
        partial.append(buffer, got);
        std::size_t newline = 0;
        while ((newline = partial.find('\n')) != std::string::npos) {
          fresh.push_back(partial.substr(0, newline));
          partial.erase(0, newline + 1);
        }
      }
      return any;
    };
    const auto drain = [&]() {
      read_new();
      if (fresh.empty()) {
        return;
      }
      absorb_deploy_lines(keep_alive, fresh);
      fresh.clear();
      request_render(*keep_alive);
    };

    gate->ready.store(true, std::memory_order_release);
    while (keep_alive->deploy_running.load(std::memory_order_relaxed)) {
      drain();
      std::this_thread::sleep_for(kDeployTailPoll);
    }
    // One last pass: the engine wrote the done/failed lines while we slept.
    drain();
    std::fclose(file);
    gate->drained.store(true, std::memory_order_release);
  });
  tail.detach();
}

}  // namespace

void start_deploy(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const berth::Recipe& recipe,
                  const std::uint16_t forced_port) {
  std::shared_ptr<std::atomic<bool>> engine_cancel;
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
    engine_cancel = std::make_shared<std::atomic<bool>>(false);
    panel->deploy_engine_cancel = engine_cancel;
  }

  // The tail goes first: it must know the log's end offset before the engine
  // writes "fetch ...", or this deploy's first line is lost to the race.
  const std::shared_ptr<TailGate> gate = std::make_shared<TailGate>();
  follow_deploy_log(panel, paths.deploy_log, gate);

  const std::shared_ptr<Panel> keep_alive = panel;
  run_on_worker<berth::DeployOutcome>(
      panel,
      [paths, recipe, forced_port, engine_cancel, gate,
       keep_alive](std::atomic<bool>* /*panel_cancel*/) {
        static_cast<void>(wait_bounded(gate->ready, kTailReadyBudget));
        berth::RealDeployOps ops;
        berth::DeployOptions options;
        // The deploy's OWN cancel flag, not the panel-wide one: a screen
        // refresh must not be able to kill a deploy in flight.
        options.cancel = engine_cancel.get();
        berth::Result<berth::DeployOutcome> outcome =
            run_deploy(paths, recipe, forced_port, ops, options);
        // Tell the tail to stop, then WAIT for it, so the done/failed lines
        // land in the ring before the verdict is folded into the state. The
        // wait is bounded: a wedged reader must not hold the result back.
        keep_alive->deploy_running.store(false, std::memory_order_release);
        static_cast<void>(wait_bounded(gate->drained, kTailDrainBudget));
        return outcome;
      },
      [](Panel& state, berth::Result<berth::DeployOutcome>&& outcome) {
        state.deploy_finished = true;
        state.deploy_in_flight = false;
        state.deploy_engine_cancel.reset();
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
  std::shared_ptr<std::atomic<bool>> engine_cancel;
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->deploy_running.load(std::memory_order_relaxed) ||
        panel->deploy_cancel_requested.load(std::memory_order_relaxed)) {
      return;
    }
    panel->deploy_cancel_requested.store(true, std::memory_order_relaxed);
    engine_cancel = panel->deploy_engine_cancel;
    panel->deploy_status = "cancelling " + panel->deploy_service + "...";
  }
  if (engine_cancel != nullptr) {
    engine_cancel->store(true, std::memory_order_relaxed);
  }
  request_render(*panel);
}

Element render_deploy_screen(const Panel& panel, ftxui::Element port_input) {
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
    const bool ok = panel.deploy_finished && !panel.deploy_cancelled &&
                    panel.deploy_error.empty();
    status.push_back(ftxui::text(panel.deploy_status) |
                     ftxui::color(ok ? Color::Green : Color::Red));
  } else {
    status.push_back(ftxui::text("idle"));
  }
  out.push_back(ftxui::hbox(std::move(status)) | ftxui::dim);
  out.push_back(ftxui::separator());

  if (panel.catalog_error.empty() && panel.catalog.recipes.empty() &&
      !panel.catalog_loading) {
    out.push_back(
        ftxui::paragraph("the catalog is empty: no builtin recipes compiled "
                         "into this binary and no user recipes under the "
                         "recipes dir") |
        ftxui::dim);
  } else {
    out.push_back(line_list(catalog_list_lines(panel.catalog),
                            panel.catalog_cursor, Color::Magenta, true,
                            kDeployListRows));
  }
  if (panel.catalog_details) {
    out.push_back(paragraph_lines(
        catalog_detail_lines(panel.catalog, panel.catalog_cursor)));
  }
  out.push_back(ftxui::separator());

  // The port field. It is inert while a deploy runs (the app stops forwarding
  // keys to it then) — a port cannot change halfway through a build.
  Elements port_row;
  port_row.push_back(ftxui::text("port ") | ftxui::dim);
  port_row.push_back(std::move(port_input));
  if (panel.deploy_running.load(std::memory_order_relaxed)) {
    port_row.push_back(ftxui::text("  (locked while deploying)") | ftxui::dim);
  } else {
    port_row.push_back(
        ftxui::text("  empty = the recipe's own default, or a free port") |
        ftxui::dim);
  }
  out.push_back(ftxui::hbox(std::move(port_row)));
  out.push_back(ftxui::separator());

  out.push_back(paragraph_lines(deploy_stage_lines(
      panel.deploy_stage, panel.deploy_finished, panel.deploy_status)));
  if (panel.deploy_cancelled) {
    for (const std::string& line :
         deploy_cancelled_lines(panel.deploy_service)) {
      out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Yellow));
    }
  }
  if (panel.deploy_finished && panel.deploy_error.empty()) {
    out.push_back(paragraph_lines(deploy_outcome_lines(panel.deploy_outcome)) |
                  ftxui::color(Color::Green));
  }
  if (!panel.deploy_error.empty()) {
    for (const std::string& line : deploy_error_lines(panel.deploy_error)) {
      out.push_back(ftxui::paragraph(line) | ftxui::color(Color::Red));
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
  out.push_back(
      log_window(panel.deploy_log_lines, 0, kDeployTailRows, Color::Cyan));
  out.push_back(ftxui::separator());
  for (const std::string& hint :
       deploy_key_hints(panel.deploy_running.load(std::memory_order_relaxed))) {
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
      panel,
      [paths](std::atomic<bool>* cancel) -> berth::Result<Catalog> {
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

Element render_catalog_screen(const Panel& panel) {
  if (panel.catalog_loading) {
    return notice_box("reading the catalog...");
  }
  if (!panel.catalog_error.empty()) {
    // Loud on purpose: a corrupt recipe must not render as an empty catalog
    // (§11), and the load is an Err precisely so this can say so.
    return notice_box(
        "the catalog could not be read. this is a loud failure on purpose: "
        "a corrupt recipe must never look like an empty catalog.\n\n" +
        panel.catalog_error +
        "\n\nfix the file, then press r to read it again.");
  }
  if (panel.catalog.recipes.empty()) {
    return notice_box(
        "the catalog is empty: no builtin recipes compiled into this binary "
        "and no user recipes under the recipes dir.");
  }

  Elements out;
  out.push_back(ftxui::text("CATALOG") | ftxui::bold |
                ftxui::color(Color::Cyan));
  out.push_back(ftxui::paragraph(
                    "read-only. the System Scan itself (prerequisites, ports, "
                    "interfaces) lands with stage 6.5 — this tab lists "
                    "recipes until then.") |
                ftxui::dim);
  out.push_back(ftxui::separator());
  out.push_back(line_list(catalog_list_lines(panel.catalog),
                          panel.catalog_cursor, Color::Cyan, true));
  out.push_back(ftxui::separator());
  if (panel.catalog_details) {
    out.push_back(paragraph_lines(
        catalog_detail_lines(panel.catalog, panel.catalog_cursor)));
  } else {
    out.push_back(
        ftxui::paragraph("Enter — details for the selected recipe "
                         "(description, tools, build steps, start command)") |
        ftxui::dim);
    out.push_back(
        ftxui::paragraph("to deploy one, switch to the Deploy tab — the same "
                         "recipes, with a port field and a cancel key.") |
        ftxui::dim);
  }
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
      panel,
      [paths](std::atomic<bool>* cancel) {
        return berth::service_rows(paths, cancel);
      },
      [](Panel& state, berth::Result<std::vector<berth::ServiceRow>>&& rows) {
        state.services_loading = false;
        if (rows.is_err()) {
          state.services_status = rows.error().ctx();
          return;
        }
        state.rows = std::move(rows).value();
        if (state.services_cursor >= state.rows.size()) {
          state.services_cursor = 0;
        }
        if (state.services_status.rfind("querying", 0) == 0) {
          state.services_status.clear();
        }
      });
}

namespace {

// start / stop / uninstall share one shape: a label, one blocking call, and
// the outcome folded into the status line. Only the call differs.
void service_action(const std::shared_ptr<Panel>& panel,
                    const std::string& label,
                    const std::function<berth::Result<void>()>& op) {
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->action.empty()) {
      return;  // one action at a time: a second press is a no-op, not a race
    }
    panel->action = label;
  }
  run_on_worker<void>(
      panel, [op](std::atomic<bool>* /*cancel*/) { return op(); },
      [label](Panel& state, berth::Result<void>&& done) {
        state.action.clear();
        if (done.is_err()) {
          state.services_status = done.error().ctx();
          return;
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
    out.push_back(line_list(services_table_lines(panel.rows),
                            panel.services_cursor, Color::Green, true));
  }
  out.push_back(ftxui::separator());
  for (const std::string& hint : services_key_hints()) {
    out.push_back(ftxui::paragraph(hint) | ftxui::dim);
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

Element confirm_uninstall_screen(const Panel& panel) {
  Elements body;
  for (const std::string& line :
       confirm_uninstall_lines(panel.uninstall_target)) {
    body.push_back(line.empty() ? ftxui::text(" ") : ftxui::paragraph(line));
  }
  Element dialog = ftxui::vbox(std::move(body)) |
                   ftxui::borderStyled(ftxui::ROUNDED, Color::Red);
  // Slightly above centre, with the spacers doing the centring — no absolute
  // coordinates, so the dialog survives a resize.
  const Element spacer = ftxui::text(" ");
  return ftxui::vbox({
      ftxui::hbox({ftxui::xflex_shrink(spacer), std::move(dialog),
                   ftxui::xflex_shrink(spacer)}) |
          ftxui::yflex,
      ftxui::yflex_shrink(spacer),
      ftxui::yflex_shrink(spacer),
      ftxui::yflex_shrink(spacer),
  });
}

UninstallKey handle_uninstall_key(Panel& panel, const std::string& key) {
  if (key == "y" || key == "Y") {
    panel.confirming_uninstall = false;
    return UninstallKey::kEaten;  // the caller runs the uninstall
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