// src/tui/app.cpp — see app.h for the contract (render thread never blocks).

#include "app.h"

#include <ftxui/component/component_base.hpp>
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
#include "chrome.h"
#include "panel.h"
#include "screens.h"

namespace tui {

namespace {

using ftxui::Element;
using ftxui::Event;

// How often the signal watcher turns a cancelled flag into an exit.
constexpr auto kCancelPoll = std::chrono::milliseconds(250);

class AppComponent final : public ftxui::ComponentBase {
 public:
  explicit AppComponent(const std::shared_ptr<Panel>& panel) : panel_(panel) {}

  Element OnRender() override {
    std::lock_guard lock(panel_->mutex);
    return ftxui::vbox({
        header_bar(),
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

    // The confirmation is modal: it takes the key and, for now, nothing else
    // on the screen reacts.
    if (panel_->confirming_uninstall) {
      const bool handled = handle_uninstall_key(*panel_, key);
      if (!handled && key == "q") {
        quit();
      }
      return true;
    }

    if (event == Event::ArrowDown || key == "j") {
      move_cursor(1);
      return true;
    }
    if (event == Event::ArrowUp || key == "k") {
      move_cursor(-1);
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
      quit();
      return true;
    }

    // Screen keys. The lock is dropped around the worker spawns, which take
    // it themselves.
    switch (tabs()[panel_->tab].tab) {
      case Tab::kCatalog:
        if (event == Event::Return) {
          std::lock_guard lock(panel_->mutex);
          panel_->catalog_details = !panel_->catalog_details;
        } else if (key == "r") {
          refresh_catalog(panel_, panel_->paths);
        }
        return true;
      case Tab::kServices:
        if (event == Event::Return || key == "r") {
          refresh_services(panel_, panel_->paths);
          return true;
        }
        if (!handle_services_key(key)) {
          return true;
        }
        return true;
      case Tab::kDeploy:
      case Tab::kLogs:
        return true;  // nothing to act on yet
    }
    return true;
  }

  // Called once, before the loop: the two startup loads. Both are file reads
  // plus one bounded systemctl round trip per service — on workers, like
  // everything else.
  void start() {
    refresh_catalog(panel_, panel_->paths);
    refresh_services(panel_, panel_->paths);
  }

 private:
  void quit() {
    // Every long operation in the engine polls this flag, so a quit unwinds
    // rather than orphaning a half-run action.
    panel_->cancel.store(true);
    panel_->screen->Exit();
  }

  void switch_tab(const int delta) {
    std::lock_guard lock(panel_->mutex);
    const std::size_t count = tabs().size();
    panel_->tab =
        (panel_->tab + static_cast<std::size_t>(delta) + count) % count;
    panel_->catalog_details = false;
  }

  void move_cursor(const int delta);

  // s / x / u on My Services. Returns false when there is nothing selected or
  // an action is already in flight — a key press that cannot act must not
  // queue another subprocess.
  bool handle_services_key(const std::string& key);

  Element render_content();

  std::shared_ptr<Panel> panel_;
};

void AppComponent::move_cursor(const int delta) {
  std::lock_guard lock(panel_->mutex);
  std::size_t* cursor = nullptr;
  std::size_t count = 0;
  switch (tabs()[panel_->tab].tab) {
    case Tab::kCatalog:
      cursor = &panel_->catalog_cursor;
      count = panel_->catalog.recipes.size();
      break;
    case Tab::kServices:
      cursor = &panel_->services_cursor;
      count = panel_->rows.size();
      break;
    case Tab::kDeploy:
    case Tab::kLogs:
      return;  // nothing to select on a not-yet-built screen
  }
  if (cursor == nullptr || count == 0) {
    return;
  }
  // Wrapping in both directions, without ever computing count - 1.
  const std::size_t offset = static_cast<std::size_t>(
      (delta + static_cast<int>(count)) % static_cast<int>(count));
  *cursor = (*cursor + offset) % count;
}

bool AppComponent::handle_services_key(const std::string& key) {
  berth::ServiceRow selected;
  std::string name;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->rows.empty() || !panel_->action.empty()) {
      return false;
    }
    if (panel_->services_cursor >= panel_->rows.size()) {
      abort_bug("no service row selected");
    }
    selected = panel_->rows[panel_->services_cursor];
    name = selected.service_name;
    if (key == "u") {
      // Uninstall deletes things: it asks first, and the dialog says what
      // stays behind.
      panel_->uninstall_target = selected;
      panel_->confirming_uninstall = true;
      return false;
    }
  }
  if (key == "s") {
    service_action_start(panel_, name);
  } else if (key == "x") {
    service_action_stop(panel_, name);
  } else {
    return false;
  }
  return true;
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
      return render_catalog_screen(*panel_);
    case Tab::kServices:
      return render_services_screen(*panel_);
    case Tab::kDeploy:
    case Tab::kLogs:
      // Every placeholder says what is missing and how to work around it
      // today. An empty box would read as a broken feature (§11: silence on
      // a rejected input is a bug, not a policy).
      return notice_box(pending_notice(tabs()[panel_->tab].tab)) | ftxui::flex;
  }
  abort_bug("unknown tab in render_content");
}

void start_cancel_watcher(const std::shared_ptr<Panel>& panel) {
  // SIGINT/SIGTERM flip the cancel flag and nothing more; a TUI that only
  // flipped a flag would ignore systemd's stop until SIGKILL. The watcher
  // turns a signal into a clean exit, and it is itself interruptible.
  std::thread watcher([panel]() {
    while (!panel->cancel.load() && !panel->shutting_down.load()) {
      std::this_thread::sleep_for(kCancelPoll);
    }
    if (!panel->shutting_down.load()) {
      panel->screen->Exit();
    }
  });
  watcher.detach();
}

}  // namespace

int run_tui(const berth::Paths& paths) {
  auto panel = std::make_shared<Panel>();
  panel->paths = paths;

  // Ctrl-C and SIGTERM become the cooperative cancel flag every long
  // operation already polls. The watcher thread turns the flag into an exit.
  const berth::Result<void> signalling =
      berth::install_exit_signalling(&panel->cancel);
  if (signalling.is_err()) {
    std::fputs("berthpanel: ", stderr);
    std::fputs(signalling.error().ctx().c_str(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }

  ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
  panel->screen = &screen;

  auto app = std::make_shared<AppComponent>(panel);
  app->start();
  start_cancel_watcher(panel);
  screen.Loop(app);

  // From here on no worker may touch the screen: they check shutting_down
  // before posting, and they hold their own shared_ptr to the state.
  panel->shutting_down.store(true);
  berth::uninstall_exit_signalling();
  return 0;
}

}  // namespace tui