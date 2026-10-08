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
    while (keep_alive->deploy_running.load(std::memory_order_relaxed) &&
           !keep_alive->shutting_down.load(std::memory_order_relaxed)) {
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

  const std::shared_ptr<Panel> keep_alive = panel;
  run_on_worker<berth::DeployOutcome>(
      panel, "deploy " + recipe.service_name,
      [paths, recipe, forced_port, gate, keep_alive,
       panel](const std::shared_ptr<Op>& op) {
        static_cast<void>(wait_bounded(gate->ready, kTailReadyBudget));
        {
          // 'c' flips this same flag, so the key and the quit reach the engine
          // by one path instead of two. A screen refresh must NOT be able to
          // kill a deploy in flight, which is why it is the op's flag and not
          // the panel-wide one.
          std::lock_guard lock(panel->mutex);
          panel->deploy_op = op;
        }
        berth::RealDeployOps ops;
        berth::DeployOptions options;
        options.cancel = &op->cancel;
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
  std::size_t scroll = 0;
  {
    std::lock_guard lock(panel->mutex);
    if (panel->log_loading) {
      return;
    }
    panel->log_loading = true;
    panel->log_error.clear();
    file = panel->log_selected;
    scroll = panel->log_scroll;
  }
  const std::filesystem::path path = log_path_of(paths, file);
  run_on_worker<berth::Tail>(
      panel, "log tail",
      [path](const std::shared_ptr<Op>& /*op*/) {
        return berth::tail_lines(path, kLogViewLines);
      },
      [file, scroll](Panel& state, berth::Result<berth::Tail>&& tail) {
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
        // A refresh must not yank the view back to the bottom: someone
        // scrolling up to read an old line is doing so on purpose.
        state.log_scroll = scroll;
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
      panel, label,
      [op](const std::shared_ptr<Op>& /*registered*/) { return op(); },
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