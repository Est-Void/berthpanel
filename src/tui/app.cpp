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
#include "../util/strutil.h"
#include "chrome.h"
#include "panel.h"
#include "screens.h"
#include "view_deploy.h"

namespace tui {

namespace {

using ftxui::Element;
using ftxui::Event;

// How often the signal watcher turns a cancelled flag into an exit.
constexpr auto kCancelPoll = std::chrono::milliseconds(250);

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
      if (handle_uninstall_key(*panel_, key) == UninstallKey::kQuit) {
        quit();
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
        handle_services_key(key);
        return true;
      case Tab::kDeploy:
        handle_deploy_key(key);
        return true;
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

  // s / x / u on My Services. A key press that cannot act (nothing selected,
  // or an action already in flight) must not queue another subprocess.
  void handle_services_key(const std::string& key);

  // c / e / r on Deploy. Enter never reaches here: the port Input consumes it
  // and starts the deploy through its on_enter.
  void handle_deploy_key(const std::string& key);

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
      cursor = &panel_->catalog_cursor;
      count = panel_->catalog.recipes.size();
      break;
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
      return render_catalog_screen(*panel_);
    case Tab::kDeploy:
      return render_deploy_screen(*panel_, port_input_->Render());
    case Tab::kServices:
      return render_services_screen(*panel_);
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