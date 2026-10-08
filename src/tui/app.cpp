// src/tui/app.cpp — see app.h for the contract (render thread never blocks).

#include "app.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../berth/cancel.h"
#include "../core/netstatus.h"
#include "../util/strutil.h"
#include "chrome.h"
#include "panel.h"
#include "screens.h"
#include "view_deploy.h"
#include "view_logs.h"

namespace tui {

namespace {

using ftxui::Element;
using ftxui::Event;

// How often the signal watcher looks at its flag.
constexpr auto kCancelPoll = std::chrono::milliseconds(250);
// How long a quit waits for a cancelled operation to actually unwind. Bounded
// on purpose: a worker wedged in a syscall must not hold the exit hostage, and
// the failure has to be reported rather than waited on forever (§11).
constexpr std::chrono::milliseconds kShutdownJoinBudget{2000};

// The keys the port field owns on the Deploy tab. Digits only (the rule and
// its reason are in view_deploy.h); Return, Backspace and Delete are named
// events the app recognises itself.
bool port_key_goes_to_field(const ftxui::Event& event) {
  if (event == Event::Return || event == Event::Backspace ||
      event == Event::Delete) {
    return true;
  }
  return event.is_character() &&
         port_field_accepts_digit(std::string(event.character()));
}

class AppComponent final : public ftxui::ComponentBase {
 public:
  explicit AppComponent(const std::shared_ptr<Panel>& panel) : panel_(panel) {
    // The port field is the one place the user types, and it is a Component
    // because typing IS a component: app.cpp owns the event forwarding and
    // screens.cpp only draws what it returns. Enter runs the deploy, so the
    // launch key and the field cannot drift apart.
    ftxui::InputOption port_option;
    port_option.content = &panel_->deploy_port_field;
    port_option.placeholder = "auto";
    port_option.on_enter = [this]() { start_selected_deploy(); };
    port_input_ = ftxui::Input(port_option);
  }

  Element OnRender() override {
    std::lock_guard lock(panel_->mutex);
    return ftxui::vbox({
        header_bar(berth::netstatus_snapshot()),
        ftxui::hbox({
            tab_strip(panel_->tab),
            ftxui::separator(),
            render_content() | ftxui::borderRounded | ftxui::flex,
        }) | ftxui::flex,
        footer_bar(),
    });
  }

  bool OnEvent(Event event) override {
    const std::string key =
        event.is_character() ? std::string(event.character()) : std::string();

    // Ctrl-C is ours, not FTXUI's: it cancels what is running and takes the
    // SAME path as q (screen.ForceHandleCtrlC(false) in run_tui). Handled
    // before everything else, including the modal — a quit must not be stuck
    // behind a dialog.
    if (event == Event::CtrlC) {
      quit(panel_->ops.cancel_all());
      return true;
    }

    // The confirmation is modal: it takes the key and, for now, nothing else
    // on the screen reacts.
    if (panel_->confirming_uninstall) {
      if (handle_uninstall_key(*panel_, key) == UninstallKey::kQuit) {
        quit(panel_->ops.cancel_all());
      }
      return true;
    }

    // The port field gets digits, backspace and Enter — and nothing else. A
    // port IS digits, and a field that swallowed every letter would take 'q'
    // (quit), 'e' and 'r' hostage: the panel must always be quittable and the
    // screen's own keys must always work. During a deploy the field is locked,
    // which is what frees 'c' to mean cancel instead of "a c in the port box".
    if (tabs()[panel_->tab].tab == Tab::kDeploy &&
        !panel_->deploy_running.load(std::memory_order_relaxed) &&
        port_key_goes_to_field(event)) {
      port_input_->OnEvent(event);
      return true;
    }

    if (event == Event::ArrowUp || key == "k") {
      // Up in a log is BACK in time: +1 is one line older.
      if (scroll_logs(1)) {
        return true;
      }
      move_cursor(-1);
      return true;
    }
    if (event == Event::ArrowDown || key == "j") {
      if (scroll_logs(-1)) {
        return true;
      }
      move_cursor(1);
      return true;
    }
    if (event == Event::ArrowRight || event == Event::Tab) {
      switch_tab(1);
      return true;
    }
    if (event == Event::ArrowLeft) {
      switch_tab(-1);
      return true;
    }
    if (key == "q" || event == Event::Escape) {
      // Cancel FIRST, then leave the loop: every long operation polls its flag
      // (§11), so a quit unwinds what is running instead of orphaning it.
      quit(panel_->ops.cancel_all());
      return true;
    }

    // Screen keys. The lock is dropped around the worker spawns, which take
    // it themselves.
    switch (tabs()[panel_->tab].tab) {
      case Tab::kCatalog:
        if (key == "r") {
          run_system_scan(panel_, panel_->paths);
        }
        return true;
      case Tab::kServices:
        if (event == Event::Return || key == "r") {
          refresh_services(panel_, panel_->paths);
          return true;
        }
        handle_services_key(key);
        return true;
      case Tab::kDeploy:
        handle_deploy_key(key);
        return true;
      case Tab::kLogs:
        handle_logs_key(key, event);
        return true;
    }
    return true;
  }

  // Called once, before the loop: the two startup loads. Both are file reads
  // plus one bounded systemctl round trip per service — on workers, like
  // everything else. The System Scan is NOT here; it runs on first visit.
  void start() {
    refresh_catalog(panel_, panel_->paths);
    refresh_services(panel_, panel_->paths);
  }

 private:
  // The quit. `cancelled_ops` is carried so run_tui() can say what it was
  // unwinding — a quit that silently abandons a running deploy is the §11
  // frozen-screen bug wearing a different hat.
  void quit(const std::size_t cancelled_ops) {
    panel_->cancelled_on_quit = cancelled_ops;
    panel_->screen->Exit();
  }

  // Switching tabs. The System Scan runs the FIRST time the user opens it,
  // not at startup: a network GET should happen because someone went looking,
  // and an idle panel should not touch the network uninvited (§6.1).
  void switch_tab(const int delta) {
    bool scan_the_first_time = false;
    bool reload_logs = false;
    {
      std::lock_guard lock(panel_->mutex);
      const std::size_t count = tabs().size();
      panel_->tab =
          (panel_->tab + static_cast<std::size_t>(delta) + count) % count;
      panel_->catalog_details = false;
      if (tabs()[panel_->tab].tab == Tab::kCatalog && !panel_->scan_ever_ran) {
        panel_->scan_ever_ran = true;
        scan_the_first_time = true;
      }
      reload_logs = tabs()[panel_->tab].tab == Tab::kLogs;
    }
    if (scan_the_first_time) {
      run_system_scan(panel_, panel_->paths);
    }
    if (reload_logs) {
      // Entering the tab IS the explicit request: one bounded tail read of at
      // most 64 KiB, on a worker, and never from the render thread.
      refresh_logs(panel_, panel_->paths);
    }
  }

  void move_cursor(const int delta);

  // s / x / u on My Services. A key press that cannot act (nothing selected,
  // or an action already in flight) must not queue another subprocess.
  void handle_services_key(const std::string& key);

  // c / e / r on Deploy. Enter never reaches here: the port Input consumes it
  // and starts the deploy through its on_enter.
  void handle_deploy_key(const std::string& key);

  // 1 / 2 / 3 and r on Logs. Scrolling is handled earlier (scroll_logs), so
  // only the file switch and the reload are here.
  void handle_logs_key(const std::string& key, const ftxui::Event& event);

  bool scroll_logs(int delta);

  // The recipe under the cursor plus the port the user typed, validated. Both
  // reads happen under the lock and nothing is spawned from inside it.
  void start_selected_deploy();

  Element render_content();

  std::shared_ptr<Panel> panel_;
  ftxui::Component port_input_;
};

void AppComponent::move_cursor(const int delta) {
  std::lock_guard lock(panel_->mutex);
  std::size_t* cursor = nullptr;
  std::size_t count = 0;
  switch (tabs()[panel_->tab].tab) {
    case Tab::kCatalog:
      return;  // the scan report selects nothing
    case Tab::kDeploy:
      cursor = &panel_->catalog_cursor;
      count = panel_->catalog.recipes.size();
      break;
    case Tab::kServices:
      cursor = &panel_->services_cursor;
      count = panel_->rows.size();
      break;
    case Tab::kLogs:
      return;  // nothing to select on a not-yet-built screen
  }
  if (cursor == nullptr || count == 0) {
    return;
  }
  // Wrapping in both directions without a signed modulo: stepping up by one
  // is +1, stepping down by one is +(count - 1).
  const std::size_t offset =
      delta >= 0 ? static_cast<std::size_t>(delta) : count - 1;
  *cursor = (*cursor + offset) % count;
}

void AppComponent::handle_deploy_key(const std::string& key) {
  if (key == "c") {
    cancel_deploy(panel_);
    return;
  }
  if (key == "e") {
    std::lock_guard lock(panel_->mutex);
    panel_->catalog_details = !panel_->catalog_details;
    return;
  }
  if (key == "r") {
    refresh_catalog(panel_, panel_->paths);
  }
}

// `delta` > 0 moves one line further back in time. Returns false when the Logs
// tab is not the active one, so the same arrow keys keep moving a list cursor
// everywhere else.
bool AppComponent::scroll_logs(const int delta) {
  if (tabs()[panel_->tab].tab != Tab::kLogs) {
    return false;
  }
  std::lock_guard lock(panel_->mutex);
  if (delta > 0) {
    ++panel_->log_scroll;
  } else if (panel_->log_scroll > 0) {
    // Scrolling toward the newest line stops AT zero, where the view is
    // pinned: a log that follows its own tail is the point.
    panel_->log_scroll -= 1;
  }
  return true;
}

void AppComponent::handle_logs_key(const std::string& key,
                                   const ftxui::Event& event) {
  if (key == "1" || key == "2" || key == "3") {
    const std::size_t index = static_cast<std::size_t>(key[0] - '1');
    LogFile wanted = LogFile::kDeploy;
    if (index < kLogFileCount) {
      wanted = log_file_at(index);
    }
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->log_selected == wanted) {
        return;
      }
      panel_->log_selected = wanted;
      panel_->log_scroll = 0;
      panel_->log_error.clear();
      // The old file's lines must not stay on screen under the new file's name.
      panel_->logs_lines.clear();
    }
    refresh_logs(panel_, panel_->paths);
    return;
  }
  if (key == "r" || event == Event::Return) {
    refresh_logs(panel_, panel_->paths);
  }
}

void AppComponent::start_selected_deploy() {
  berth::Recipe recipe;
  berth::Paths paths;
  std::uint16_t port = 0;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->catalog_cursor >= panel_->catalog.recipes.size()) {
      return;
    }
    recipe = panel_->catalog.recipes[panel_->catalog_cursor];
    paths = panel_->paths;
    // A port that is not a number is a loud refusal here, before a worker is
    // spawned: coercing it to 0 would deploy somewhere the user did not ask
    // for, which is the worst possible reading of "I typed 808O".
    const berth::Result<std::uint16_t> parsed =
        parse_port_field(panel_->deploy_port_field);
    if (parsed.is_err()) {
      panel_->deploy_finished = false;
      panel_->deploy_cancelled = false;
      panel_->deploy_error = parsed.error().ctx();
      panel_->deploy_status = "no deploy started";
      return;
    }
    port = std::move(parsed).value();
  }
  start_deploy(panel_, paths, recipe, port);
}

void AppComponent::handle_services_key(const std::string& key) {
  std::string name;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->rows.empty() || !panel_->action.empty()) {
      return;
    }
    if (panel_->services_cursor >= panel_->rows.size()) {
      abort_bug("no service row selected");
    }
    const berth::ServiceRow selected = panel_->rows[panel_->services_cursor];
    name = selected.service_name;
    if (key == "u") {
      // Uninstall deletes things: it asks first, and the dialog says what
      // stays behind.
      panel_->uninstall_target = selected;
      panel_->confirming_uninstall = true;
      return;
    }
  }
  if (key == "s") {
    service_action_start(panel_, name);
  } else if (key == "x") {
    service_action_stop(panel_, name);
  }
}

Element AppComponent::render_content() {
  // The confirmation REPLACES the content rather than overlaying it: a dbox
  // over a full-width table lets the table bleed through the dialog, and a
  // confirmation you cannot read is not a confirmation.
  if (panel_->confirming_uninstall) {
    return confirm_uninstall_screen(*panel_);
  }
  switch (tabs()[panel_->tab].tab) {
    case Tab::kCatalog:
      return render_scan_screen(*panel_);
    case Tab::kDeploy:
      return render_deploy_screen(*panel_, port_input_->Render());
    case Tab::kServices:
      return render_services_screen(*panel_);
    case Tab::kLogs:
      return render_logs_screen(*panel_);
  }
  abort_bug("unknown tab in render_content");
}

// SIGINT/SIGTERM are the only thing that can interrupt a quit from outside,
// and they arrive as a flag — there is no "panel-wide cancel flag" any more,
// because a flag nothing polls is exactly the §11 bug. The watcher asks for the
// same shutdown a key press asks for: cancel what is running, then leave the
// loop.
void start_cancel_watcher(const std::shared_ptr<Panel>& panel,
                          const std::shared_ptr<std::atomic<bool>>& signalled) {
  std::thread watcher([panel, signalled]() {
    while (!signalled->load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(kCancelPoll);
    }
    static_cast<void>(panel->ops.cancel_all());
    panel->screen->Exit();
  });
  watcher.detach();
}

}  // namespace

int run_tui(const berth::Paths& paths) {
  auto panel = std::make_shared<Panel>();
  panel->paths = paths;
  // The counters describe THIS session (§6.1): net.log survives across runs, a
  // counter that did not reset would claim requests this panel never made.
  berth::netstatus_reset();

  // Ctrl-C and SIGTERM become a flag the watcher polls. The terminal is put
  // back on every exit path: Exit() restores it before Loop() returns, and the
  // ScreenInteractive destructor restores it again if we ever leave another
  // way. Shared, not on the stack: the watcher thread can outlive this frame.
  auto signalled = std::make_shared<std::atomic<bool>>(false);
  const berth::Result<void> signalling =
      berth::install_exit_signalling(signalled.get());
  if (signalling.is_err()) {
    std::fputs("berthpanel: ", stderr);
    std::fputs(signalling.error().ctx().c_str(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }

  ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
  panel->screen = &screen;
  // Ctrl-C is ours to handle, so FTXUI must not turn it into its own SIGABRT:
  // we cancel what is running, unwind it and restore the terminal exactly like
  // a normal quit.
  screen.ForceHandleCtrlC(false);

  auto app = std::make_shared<AppComponent>(panel);
  app->start();
  start_cancel_watcher(panel, signalled);
  screen.Loop(app);

  // From here on no worker may touch the screen: they check shutting_down
  // before posting, and they hold their own shared_ptr to the state.
  panel->shutting_down.store(true);

  // Stage 6.7: the quit already asked every live op to stop, and now we WAIT
  // for them — bounded. An unwound deploy leaves a journal entry that __cleanup
  // sweeps, so two seconds is worth spending to know the worker really stopped.
  // A worker that ignores the request is DETACHED, loudly: silence there is the
  // one outcome nobody can debug (§11).
  if (const std::size_t live = panel->ops.size(); live > 0) {
    static_cast<void>(panel->ops.cancel_all());
    if (panel->ops.wait_until_empty(kShutdownJoinBudget)) {
      std::fputs("berthpanel: unwound ", stderr);
      std::fputs(std::to_string(live).c_str(), stderr);
      std::fputs(" running operation(s) before exiting\n", stderr);
    } else {
      std::fputs("berthpanel: ", stderr);
      std::fputs(std::to_string(live).c_str(), stderr);
      std::fputs(" operation(s) did not stop within ", stderr);
      std::fputs(std::to_string(kShutdownJoinBudget.count()).c_str(), stderr);
      std::fputs(
          "s and were left detached — a journal entry, if any, stays "
          "for __cleanup\n",
          stderr);
    }
  }
  if (panel->cancelled_on_quit > 0) {
    std::fputs("berthpanel: cancelled ", stderr);
    std::fputs(std::to_string(panel->cancelled_on_quit).c_str(), stderr);
    std::fputs(" running operation(s) on quit\n", stderr);
  }

  berth::uninstall_exit_signalling();
  screen.Exit();
  return 0;
}

}  // namespace tui