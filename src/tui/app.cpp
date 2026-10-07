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
    return true;
  }

 private:
  void quit() {
    // Every long operation in the engine polls this flag, so a quit unwinds
    // rather than orphaning a half-run action.
    panel_->cancel.store(true);
    panel_->screen->Exit();
  }

  void switch_tab(const int delta) {
    const std::size_t count = tabs().size();
    panel_->tab =
        (panel_->tab + static_cast<std::size_t>(delta) + count) % count;
  }

  Element render_content();

  std::shared_ptr<Panel> panel_;
};

// Every tab is a placeholder in this stage, and every placeholder says what
// is missing and how to work around it today. An empty box would read as a
// broken feature (§11: silence on a rejected input is a bug, not a policy).
Element AppComponent::render_content() {
  return notice_box(pending_notice(tabs()[panel_->tab].tab)) | ftxui::flex;
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
  start_cancel_watcher(panel);
  screen.Loop(app);

  // From here on no worker may touch the screen: they check shutting_down
  // before posting, and they hold their own shared_ptr to the state.
  panel->shutting_down.store(true);
  berth::uninstall_exit_signalling();
  return 0;
}

}  // namespace tui