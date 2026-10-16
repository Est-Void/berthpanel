#pragma once

// Panel is ONE struct under ONE mutex: per-field locks are how a half-updated
// view happens. Nothing here blocks — an engine call belongs on a worker.
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
#include "view_edit.h"
#include "view_logs.h"
#include "view_scan.h"
#include "view_services.h"

namespace tui {

inline constexpr std::size_t kSignWidth = 5;
inline constexpr std::size_t kSignGap = 3;
inline constexpr std::size_t kVisibleRows = 12;
// Bounded (§5): a runaway build must not grow the panel's memory.
inline constexpr std::size_t kLogCapacity = 2000;

struct Panel {
  // Copied in once: no screen builds a path itself (§3).
  berth::Paths paths;

  std::atomic<bool> shutting_down{false};
  mutable std::mutex mutex;

  // The quit path cancels through it: a quit must unwind what it waited for.
  OpRegistry ops;

  // The one handle a worker uses to reach the render loop; null in tests.
  ftxui::ScreenInteractive* screen = nullptr;

  // Owned here so no screen can invent its own unbounded vector (§5).
  RingBuffer<std::string> logs_lines{kLogCapacity};

  // catalog_loading is the visible refresh; catalog_auto the silent tick, so a
  // tick cannot blank the empty-state or clobber an action's status.
  bool catalog_loading = false;
  bool catalog_auto = false;
  std::string catalog_error;
  Catalog catalog;
  std::size_t catalog_cursor = 0;
  bool catalog_details = false;
  // The removal modal's target is a service NAME, not an index: a refresh (or
  // the auto-refresh tick) can move the row under a dialog.
  bool confirming_recipe_remove = false;
  std::string recipe_remove_target;
  bool recipe_removing = false;
  std::string catalog_status;

  // deploy_running is ATOMIC because two threads read it without the panel lock
  // (render thread, log tail). deploy_in_flight is the guarded one, true from
  // the key press until the verdict is drawn.
  std::atomic<bool> deploy_running{false};
  bool deploy_in_flight = false;
  std::atomic<bool> deploy_cancel_requested{false};
  // 'c' flips the same flag the quit path flips: one path to the engine.
  std::shared_ptr<Op> deploy_op;
  std::string deploy_status;
  std::string deploy_error;
  DeployStage deploy_stage = DeployStage::kOther;
  bool deploy_finished = false;
  bool deploy_cancelled = false;
  berth::DeployOutcome deploy_outcome;
  std::string deploy_service;
  RingBuffer<std::string> deploy_log_lines{kLogCapacity};

  // Insert mode, so q / e / r / c keep working with no field focused (§12.16).
  bool draft_running = false;
  // Without it a second 's' during the write queued a second writer.
  bool draft_storing = false;
  // One flag per operation, never a panel-wide cancel (§6.7).
  std::shared_ptr<Op> draft_op;
  bool draft_ready = false;
  berth::DraftResult draft;
  berth::DraftClash draft_clash;
  bool draft_stored = false;
  std::filesystem::path draft_stored_path;
  std::string draft_status;
  std::string draft_error;
  bool draft_clash_confirming = false;
  bool draft_pending_deploy = false;

  // create_field is the insert-mode focus, kNone until 'i'; the eight values
  // are the view model's EditFields, so two forms cannot disagree.
  bool create_active = false;
  CreateField create_field = CreateField::kNone;
  EditFields create;
  std::string create_url;
  std::string create_ref;
  std::string create_deploy_port;
  std::string create_status;
  std::string create_error;
  bool create_running = false;
  std::shared_ptr<Op> create_op;

  // edit_field is the focus, kNone until 'i'. A builtin recipe never sets
  // these: the refusal is a status line on the Deploy screen.
  bool edit_active = false;
  EditField edit_field = EditField::kNone;
  EditFields edit;
  std::string edit_service;
  bool edit_running = false;
  bool edit_saved = false;
  std::filesystem::path edit_stored_path;
  std::string edit_diff;
  std::string edit_status;
  std::string edit_error;

  // services_auto is the silent tick: it never replaces an action's status.
  bool services_loading = false;
  bool services_auto = false;
  std::string services_status;
  std::vector<berth::ServiceRow> rows;
  std::size_t services_cursor = 0;
  std::string action;
  bool confirming_uninstall = false;
  berth::ServiceRow uninstall_target;
  bool confirming_kill = false;
  berth::ServiceRow kill_target;

  // The tab scans on first visit: a network GET only when the user looks.
  bool scan_running = false;
  bool scan_ever_ran = false;
  std::string scan_error;
  berth::ScanReport scan;

  // Refilled by a worker on an explicit r — never polled from the render loop.
  LogFile log_selected = LogFile::kDeploy;
  bool log_loading = false;
  // A stale generation belongs to a file the user already left.
  std::size_t log_generation = 0;
  bool log_truncated = false;
  bool log_existed = false;
  std::string log_error;
  std::vector<std::optional<std::size_t>> log_counts;
  std::size_t log_scroll = 0;  // lines up from the newest (0 = live)

  // Reported on stderr: a quit must say what it interrupted (§11).
  std::size_t cancelled_on_quit = 0;

  std::size_t tab = 0;
};

// Workers post here and nowhere else; silent after shutdown, so they detach.
void request_render(const Panel& panel);

// The one way off the render thread: `work` runs detached on the op's own
// cancel flag, `apply` back on the render thread with the mutex held.
// Detached, not joined — a `systemctl` that ignores its timeout must not hold
// the quit hostage. Safe because the state is a shared_ptr and the op
// deregisters on the way out.
template <typename T>
void run_on_worker(
    const std::shared_ptr<Panel>& panel, const std::string& label,
    const std::function<berth::Result<T>(const std::shared_ptr<Op>&)>& work,
    const std::function<void(Panel&, berth::Result<T>&&)>& apply) {
  // The registry has its own mutex: the two are never held at once.
  const std::shared_ptr<Op> op = panel->ops.begin(label);
  const std::shared_ptr<Panel> keep_alive = panel;
  std::thread worker([keep_alive, work, apply, op]() {
    berth::Result<T> outcome = work(op);
    // Deregister BEFORE the shutdown check, or a quit leaves an op to find.
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

[[noreturn]] void abort_bug(const char* what);

[[nodiscard]] ftxui::Color accent_color(std::string_view name);

void pad_to_width(std::string& out, std::size_t width);

[[nodiscard]] std::size_t window_start(std::size_t count, std::size_t cursor,
                                       std::size_t visible);

// cursor_visible is false when there is nothing to select; visible is this
// list's share of the frame.
[[nodiscard]] ftxui::Element line_list(const std::vector<std::string>& lines,
                                       std::size_t cursor, ftxui::Color accent,
                                       bool cursor_visible,
                                       std::size_t visible = kVisibleRows);

// Wrapped: ftxui::text() clips at the box edge.
[[nodiscard]] ftxui::Element paragraph_lines(
    const std::vector<std::string>& lines);

// scroll counts UP from the newest, so 0 pins the bottom. Copy-free: read
// through RingBuffer::at, never cloned per frame.
template <typename T>
[[nodiscard]] ftxui::Element log_window(const RingBuffer<T>& buffer,
                                        std::size_t scroll, std::size_t visible,
                                        ftxui::Color accent) {
  if (buffer.empty()) {
    return ftxui::text(" ") | ftxui::dim | ftxui::flex;
  }
  const std::size_t total = buffer.size();
  // A scroll past the top clamps to the oldest line still kept.
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

// REPLACES the screen; a dbox over a full-width table lets the table bleed
// through.
[[nodiscard]] ftxui::Element boxed_dialog(const std::vector<std::string>& lines,
                                          ftxui::Color accent);

[[nodiscard]] ftxui::Element header_bar(const berth::NetSnapshot& net);

[[nodiscard]] ftxui::Element tab_strip(std::size_t active);

[[nodiscard]] ftxui::Element footer_bar();

[[nodiscard]] ftxui::Element key_hint(const KeyHint& hint);

[[nodiscard]] ftxui::Element key_hint_line(const std::vector<KeyHint>& hints);

}
