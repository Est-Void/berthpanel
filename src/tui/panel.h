#pragma once

// src/tui/panel.h — the panel's shared view state and the small render
// helpers every screen builds on.
//
// Panel is one struct on purpose: the render thread reads it and the workers
// write it, under ONE mutex. Per-field locks are how a half-updated view
// happens — a list that has been refreshed next to a status line that has
// not. The critical sections are a handful of string copies.
//
// Everything here is UI-THREAD state except the two atomics and the mutex,
// which the workers touch. There is no blocking call anywhere in this header:
// an engine call belongs on a worker (app.h).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../util/result.h"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "../analyzer/scan.h"
#include "../berth/deploy.h"
#include "../berth/service_ops.h"
#include "../core/netstatus.h"
#include "../core/paths.h"
#include "chrome.h"
#include "ops.h"
#include "ring.h"
#include "view_catalog.h"
#include "view_deploy.h"
#include "view_logs.h"
#include "view_scan.h"
#include "view_services.h"

namespace tui {

inline constexpr std::size_t kSignWidth = 5;  // the widest sign line
inline constexpr std::size_t kSignGap = 3;
inline constexpr std::size_t kVisibleRows = 12;  // list window height
// Bounded, always (§5): 2000 lines is enough to read a deploy tail and small
// enough that a runaway build cannot grow the panel's memory.
inline constexpr std::size_t kLogCapacity = 2000;

struct Panel {
  // The XDG layout, copied in once: no screen builds a path itself (§3).
  berth::Paths paths;

  std::atomic<bool> shutting_down{false};
  mutable std::mutex mutex;

  // The running long operations. The quit path goes through it; it is the
  // difference between "q cancelled the thing you were waiting for" and "q
  // hoped for the best" (dev.md §6.7).
  OpRegistry ops;

  // The one handle a worker may use to reach the render loop. Owned by
  // app.cpp for the panel's lifetime; null in tests, where request_render()
  // is then a no-op.
  ftxui::ScreenInteractive* screen = nullptr;

  // Bounded log buffers, owned here so no screen can invent its own unbounded
  // vector (dev.md §5). One per view: the deploy tail and the Logs screen's
  // selection are different files and different lifetimes.
  RingBuffer<std::string> logs_lines{kLogCapacity};

  // Catalog (6.2). Loaded once at startup; the DEPLOY screen is where the
  // recipe list lives, because that is where a recipe becomes an action.
  bool catalog_loading = false;
  std::string catalog_error;
  Catalog catalog;
  std::size_t catalog_cursor = 0;
  bool catalog_details = false;

  // Deploy (6.3). `deploy_running` is an ATOMIC and not mutex-guarded state
  // because two threads read it without the panel lock: the render thread (can
  // I start a second deploy?) and the log tail (is there still something to
  // read?). `deploy_in_flight` is the mutex-guarded one, and it stays true
  // from the key press until the result is folded in — a second deploy cannot
  // be started in the window where the engine is done but the screen has not
  // drawn the verdict yet. Everything else here is ordinary UI-thread state.
  std::atomic<bool> deploy_running{false};
  bool deploy_in_flight = false;
  std::atomic<bool> deploy_cancel_requested{false};
  // The operation THIS deploy runs as. 'c' flips its cancel flag, which is the
  // same flag the quit path flips — one path to the engine, not two.
  std::shared_ptr<Op> deploy_op;
  std::string deploy_status;
  std::string deploy_error;
  DeployStage deploy_stage = DeployStage::kOther;
  bool deploy_finished = false;
  bool deploy_cancelled = false;
  berth::DeployOutcome deploy_outcome;
  std::string deploy_service;     // the recipe being deployed
  std::string deploy_port_field;  // the port Input's content, "" = auto
  RingBuffer<std::string> deploy_log_lines{kLogCapacity};

  // BYO wizard (plan 4.21): paste a GitHub URL, review the draft, store it,
  // deploy it. `draft_field` is kNone until 'i' — the fields are behind an
  // explicit insert mode so that q / e / r / c keep working (§12.16).
  DraftField draft_field = DraftField::kNone;
  std::string draft_url;
  std::string draft_ref;
  bool draft_running = false;
  bool draft_ready = false;  // a draft is waiting for review
  berth::DraftResult draft;
  berth::DraftClash draft_clash;
  bool draft_stored = false;
  std::filesystem::path draft_stored_path;
  std::string draft_status;
  std::string draft_error;
  bool draft_clash_confirming = false;
  // What the clash modal does after 'y': store AND deploy, or store only.
  bool draft_pending_deploy = false;

  // My Services (6.4)
  bool services_loading = false;
  std::string services_status;
  std::vector<berth::ServiceRow> rows;
  std::size_t services_cursor = 0;
  std::string action;  // non-empty while a start/stop/uninstall is running
  bool confirming_uninstall = false;
  berth::ServiceRow uninstall_target;

  // System Scan (6.5). One worker at a time; `scan_ever_ran` is what makes the
  // tab run its scan on the first visit instead of at startup — a network GET
  // should happen because the user went looking, not because the binary did.
  bool scan_running = false;
  bool scan_ever_ran = false;
  std::string scan_error;
  berth::ScanReport scan;

  // Logs (6.6). One buffer for the SELECTED log, refilled by a worker on an
  // explicit r — never polled from the render loop, and never the whole file.
  LogFile log_selected = LogFile::kDeploy;
  bool log_loading = false;
  bool log_truncated = false;
  bool log_existed = false;
  std::string log_error;
  // Lines on screen per log file; unset until that file has been read, so
  // the tab line never prints a count for a log nobody has looked at.
  std::vector<std::optional<std::size_t>> log_counts;
  std::size_t log_scroll = 0;  // lines up from the newest (0 = live)

  // How many long operations the last quit had to cancel. Set on the quit path
  // and reported on stderr afterwards, because a panel that swallows Ctrl-C
  // without saying what it interrupted has told the user nothing (§11).
  std::size_t cancelled_on_quit = 0;

  std::size_t tab = 0;
};

// Nudges the render loop. The panel's message queue: a worker may post here
// and nowhere else. Does nothing once the panel is shutting down, which is
// what lets the workers be detached instead of joined.
void request_render(const Panel& panel);

// The one way off the render thread. `work` runs on a detached thread with the
// operation's own cancel flag (pass `&op->cancel` to the engine); `apply` runs
// back on the render thread with the mutex held, so a worker result is folded
// into the state without a second lock and without a torn read.
//
// Detached, not joined: a `systemctl` that ignores its timeout must not hold
// the quit hostage. What makes that safe is stated here once: the state is
// captured by shared_ptr (so it outlives the panel), the op deregisters itself
// on the way out (so a later cancel_all() never flips a flag nobody reads),
// request_render() goes quiet after shutdown. A worker that is still running
// when the process exits has already been told to stop.
template <typename T>
void run_on_worker(
    const std::shared_ptr<Panel>& panel, const std::string& label,
    const std::function<berth::Result<T>(const std::shared_ptr<Op>&)>& work,
    const std::function<void(Panel&, berth::Result<T>&&)>& apply) {
  // The registry carries its own mutex, so this does NOT need the panel lock:
  // the two are never held at once, and a worker never blocks the render thread
  // on the registry.
  const std::shared_ptr<Op> op = panel->ops.begin(label);
  const std::shared_ptr<Panel> keep_alive = panel;
  std::thread worker([keep_alive, work, apply, op]() {
    berth::Result<T> outcome = work(op);
    // Deregister BEFORE the shutdown check: an op that finished during the
    // quit must not be left in the list for a later cancel to find.
    static_cast<void>(keep_alive->ops.end(op));
    if (keep_alive->shutting_down.load()) {
      return;
    }
    std::lock_guard lock(keep_alive->mutex);
    apply(*keep_alive, std::move(outcome));
    request_render(*keep_alive);
  });
  worker.detach();
}

// Aborts with a message: a violated UI invariant is a bug in this binary,
// never something to render around.
[[noreturn]] void abort_bug(const char* what);

// The closed accent palette from chrome.h, as FTXUI colors.
[[nodiscard]] ftxui::Color accent_color(std::string_view name);

// Pads an already-assembled cell out to `width`. Padding, not formatting:
// the caller owns the text.
void pad_to_width(std::string& out, std::size_t width);

// The window of `lines` to draw so the cursor stays visible. `cursor` indexes
// `lines`; returns the first index to draw.
[[nodiscard]] std::size_t window_start(std::size_t count, std::size_t cursor,
                                       std::size_t visible);

// A list of plain lines with one cursor row; everything but the cursor is
// dimmed. `cursor_visible` is false when there is nothing to select. `visible`
// is the window height, so a screen with two lists gives each a share instead
// of letting both ask for the full frame.
[[nodiscard]] ftxui::Element line_list(const std::vector<std::string>& lines,
                                       std::size_t cursor, ftxui::Color accent,
                                       bool cursor_visible,
                                       std::size_t visible = kVisibleRows);

// Free text, wrapped: ftxui::text() clips at the box edge, and a hint that
// vanishes past the right margin is a hint that does not exist.
[[nodiscard]] ftxui::Element paragraph_lines(
    const std::vector<std::string>& lines);

// A log TAIL, scrolled. `scroll` is how many lines UP from the newest one the
// window sits, so 0 pins the bottom — which is what a live log must do, or the
// newest line scrolls off the moment it arrives. Copy-free: the buffer is read
// through RingBuffer::at, never cloned per frame.
template <typename T>
[[nodiscard]] ftxui::Element log_window(const RingBuffer<T>& buffer,
                                        std::size_t scroll, std::size_t visible,
                                        ftxui::Color accent) {
  if (buffer.empty()) {
    return ftxui::text(" ") | ftxui::dim | ftxui::flex;
  }
  const std::size_t total = buffer.size();
  // How many lines from the end the window starts. A scroll past the top
  // clamps: the top of a live log is the oldest line still kept.
  const std::size_t wanted = scroll + visible;
  const std::size_t from_end = wanted < total ? wanted : total;
  std::size_t first = total - from_end;
  if (first + visible > total) {
    first = total > visible ? total - visible : 0;
  }
  ftxui::Elements rows;
  for (std::size_t i = first; i < total && rows.size() < visible; ++i) {
    rows.push_back(ftxui::paragraph(buffer.at(i)) | ftxui::color(accent));
  }
  return ftxui::vbox(std::move(rows)) | ftxui::flex;
}

// A modal dialog: the lines in a rounded border, centred with spacers. It
// REPLACES the screen rather than overlaying it — a dbox over a full-width
// table lets the table bleed through, and a confirmation you cannot read is
// not a confirmation.
[[nodiscard]] ftxui::Element boxed_dialog(const std::vector<std::string>& lines,
                                          ftxui::Color accent);

// The bordered header bar with the brand mark and the version. `net` is the
// NET banner (view_logs.h): read from one atomic word on every render, which
// is the only reason it can be there at all.
[[nodiscard]] ftxui::Element header_bar(const berth::NetSnapshot& net);

// The fixed-width vertical tab strip.
[[nodiscard]] ftxui::Element tab_strip(std::size_t active);

// The key hints along the bottom.
[[nodiscard]] ftxui::Element footer_bar();

}  // namespace tui