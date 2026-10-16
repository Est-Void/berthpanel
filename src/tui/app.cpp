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
#include "view_catalog.h"
#include "view_deploy.h"
#include "view_edit.h"
#include "view_logs.h"
#include "view_refresh.h"

namespace tui {

namespace {

using ftxui::Element;
using ftxui::Event;

constexpr auto kCancelPoll = std::chrono::milliseconds(250);
// Bounded: a wedged worker must not hold the exit hostage (§11).
constexpr std::chrono::milliseconds kShutdownJoinBudget{2000};

// Typeable only, and NOT Up/Down: with no field focused they stay global.
bool key_goes_to_field(const ftxui::Event& event) {
  return event.is_character() || event == Event::Backspace ||
         event == Event::Delete || event == Event::ArrowLeft ||
         event == Event::ArrowRight || event == Event::Home ||
         event == Event::End;
}

class AppComponent final : public ftxui::ComponentBase {
 public:
  explicit AppComponent(const std::shared_ptr<Panel>& panel) : panel_(panel) {
    // Standalone Input Focused() is ALWAYS true → Default() paints inverted.
    const auto field_style = [](ftxui::InputState state) {
      state.element |= ftxui::color(ftxui::Color::White);
      if (state.is_placeholder) {
        state.element |= ftxui::dim;
      }
      return state.element;
    };

    const auto make_edit = [&field_style](const char* placeholder,
                                          std::string* content) {
      ftxui::InputOption option;
      option.transform = field_style;
      option.content = content;
      option.placeholder = placeholder;
      return ftxui::Input(option);
    };
    edit_start_cmd_ = make_edit("e.g. {self} --addr 127.0.0.1 --port {port}",
                                &panel_->edit.start_cmd);
    edit_bind_ = make_edit("127.0.0.1 (when start_cmd has no bind flag)",
                           &panel_->edit.bind);
    edit_health_path_ = make_edit("/", &panel_->edit.health_path);
    edit_display_name_ = make_edit("display name", &panel_->edit.display_name);
    edit_description_ = make_edit("one line", &panel_->edit.description);
    edit_port_ = make_edit("empty = free at deploy", &panel_->edit.port);
    edit_memory_max_ = make_edit("e.g. 512M", &panel_->edit.memory_max);
    edit_tasks_max_ = make_edit("e.g. 300", &panel_->edit.tasks_max);

    create_url_ =
        make_edit("https://github.com/owner/repo", &panel_->create_url);
    create_ref_ = make_edit("default branch or tag", &panel_->create_ref);
    create_deploy_port_ = make_edit("auto", &panel_->create_deploy_port);
    create_start_cmd_ = make_edit("{self} --addr 127.0.0.1 --port {port}",
                                  &panel_->create.start_cmd);
    create_bind_ = make_edit("127.0.0.1", &panel_->create.bind);
    create_health_path_ = make_edit("/", &panel_->create.health_path);
    create_display_name_ =
        make_edit("display name", &panel_->create.display_name);
    create_description_ = make_edit("one line", &panel_->create.description);
    create_app_port_ =
        make_edit("empty = free at deploy", &panel_->create.port);
    create_memory_max_ = make_edit("e.g. 512M", &panel_->create.memory_max);
    create_tasks_max_ = make_edit("e.g. 300", &panel_->create.tasks_max);
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
    // Esc is a Special event with an EMPTY character: name it for every dialog.
    const std::string dialog_key = event == Event::Escape ? "\x1b" : key;

    // Ctrl-C takes the SAME path as q, ahead of every modal.
    if (event == Event::CtrlC) {
      quit(panel_->ops.cancel_all());
      return true;
    }

    // Modals decide under the panel lock and act with it released (the action
    // functions take it themselves). The overwrite question comes FIRST.
    bool draft_confirming = false;
    bool draft_confirmed = false;
    bool deploy_after = false;
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->draft_clash_confirming) {
        draft_confirming = true;
        draft_confirmed =
            handle_draft_clash_key(*panel_, dialog_key) == DraftKey::kConfirmed;
        deploy_after = panel_->draft_pending_deploy;
      }
    }
    if (draft_confirming) {
      if (draft_confirmed)
        accept_draft(panel_, panel_->paths, deploy_after, /*force=*/true);
      return true;
    }

    // A modal eats the key: the dialog DECIDES, the caller ACTS (§11).
    bool kill_confirming = false;
    UninstallKey kill_decision = UninstallKey::kEaten;
    std::string kill_target;
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->confirming_kill) {
        kill_confirming = true;
        kill_decision = handle_kill_key(*panel_, dialog_key);
        kill_target = panel_->kill_target.service_name;
      }
    }
    if (kill_confirming) {
      if (kill_decision == UninstallKey::kConfirmed) {
        service_action_kill(panel_, kill_target, /*confirmed=*/true);
      } else if (kill_decision == UninstallKey::kQuit) {
        quit(panel_->ops.cancel_all());
      }
      return true;
    }

    bool uninstall_confirming = false;
    UninstallKey uninstall_decision = UninstallKey::kEaten;
    std::string uninstall_target;
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->confirming_uninstall) {
        uninstall_confirming = true;
        uninstall_decision = handle_uninstall_key(*panel_, dialog_key);
        uninstall_target = panel_->uninstall_target.service_name;
      }
    }
    if (uninstall_confirming) {
      if (uninstall_decision == UninstallKey::kQuit) {
        quit(panel_->ops.cancel_all());
      } else if (uninstall_decision == UninstallKey::kConfirmed) {
        service_action_uninstall(panel_, panel_->paths, uninstall_target);
      }
      return true;
    }

    // A delete asks first; LAST, so a more destructive question is not buried.
    bool remove_confirming = false;
    RemoveConfirm remove_decision = RemoveConfirm::kEaten;
    std::string remove_target;
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->confirming_recipe_remove) {
        remove_confirming = true;
        remove_decision = recipe_remove_key(dialog_key);
        remove_target = panel_->recipe_remove_target;
        // Close it before the caller acts, or a slow worker leaves it up.
        if (remove_decision != RemoveConfirm::kEaten) {
          panel_->confirming_recipe_remove = false;
        }
      }
    }
    if (remove_confirming) {
      if (remove_decision == RemoveConfirm::kConfirmed) {
        remove_user_recipe(panel_, panel_->paths, remove_target);
      } else if (remove_decision == RemoveConfirm::kQuit) {
        quit(panel_->ops.cancel_all());
      }
      return true;
    }

    // Insert mode: the fields take typeable keys only while one is focused, so
    // 'q' is still a quit here (§12.16).
    const bool deploy_tab = tabs()[panel_->tab].tab == Tab::kDeploy;
    if (deploy_tab) {
      // The form and editor own the tab's keys first: Esc leaves THEM, not us.
      if (handle_create_key(event, key)) {
        return true;
      }
      if (handle_edit_key(event, key)) {
        return true;
      }
    }

    if (event == Event::ArrowUp || key == "k") {
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
    // A ready draft owns Enter; Escape drops it rather than quitting the panel.
    if (deploy_tab && event == Event::Escape) {
      bool draft_up = false;
      {
        std::lock_guard lock(panel_->mutex);
        draft_up = panel_->draft_ready;
      }
      if (draft_up) {
        discard_draft(panel_);
        return true;
      }
    }
    if (key == "q" || event == Event::Escape) {
// Cancel first, then leave the loop: a quit unwinds what is running (§11).
      quit(panel_->ops.cancel_all());
      return true;
    }

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
        if (event == Event::Return) {
          bool draft_up = false;
          {
            std::lock_guard lock(panel_->mutex);
            draft_up = panel_->draft_ready || panel_->draft_running;
          }
          if (!draft_up) {
            start_selected_deploy();
          }
          return true;
        }
        handle_deploy_key(key);
        return true;
      case Tab::kLogs:
        handle_logs_key(key, event);
        return true;
    }
    return true;
  }

  void start() {
    refresh_catalog(panel_, panel_->paths);
    refresh_services(panel_, panel_->paths);
  }

 private:
  // cancelled_ops is carried so run_tui can say what it was unwinding (§11).
  void quit(const std::size_t cancelled_ops) {
    panel_->cancelled_on_quit = cancelled_ops;
    panel_->screen->Exit();
  }

  // The scan runs on the FIRST tab open: no uninvited network at startup.
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
      // Entering the tab IS the request: one bounded tail read, on a worker.
      refresh_logs(panel_, panel_->paths);
    }
  }

  void move_cursor(const int delta);

  void handle_services_key(const std::string& key);

  void handle_deploy_key(const std::string& key);

  void handle_logs_key(const std::string& key, const ftxui::Event& event);

  bool scroll_logs(int delta);

  bool handle_edit_key(const ftxui::Event& event, const std::string& key);
  ftxui::ComponentBase* edit_input_for(EditField field);
  [[nodiscard]] ftxui::Elements edit_input_elements();
  void set_edit_field(EditField field);
  void open_edit();
  void close_edit();
  void save_edit();

  // Nothing spawns under the lock; false when the port was refused.
  bool start_selected_deploy();

  Element render_content();

  // 'i' opens ONE form: the eight values are typed, not repaired afterwards.
  ftxui::ComponentBase* create_input_for(CreateField field);
  ftxui::Elements create_input_elements();
  void set_create_field(CreateField field);
  void open_create();
  void close_create();
  void submit_create();

  bool handle_create_key(const ftxui::Event& event, const std::string& key);

  std::shared_ptr<Panel> panel_;
  ftxui::Component edit_start_cmd_;
  ftxui::Component edit_bind_;
  ftxui::Component edit_health_path_;
  ftxui::Component edit_display_name_;
  ftxui::Component edit_description_;
  ftxui::Component edit_port_;
  ftxui::Component edit_memory_max_;
  ftxui::Component edit_tasks_max_;
  ftxui::Component create_url_;
  ftxui::Component create_ref_;
  ftxui::Component create_deploy_port_;
  ftxui::Component create_start_cmd_;
  ftxui::Component create_bind_;
  ftxui::Component create_health_path_;
  ftxui::Component create_display_name_;
  ftxui::Component create_description_;
  ftxui::Component create_app_port_;
  ftxui::Component create_memory_max_;
  ftxui::Component create_tasks_max_;
};

void AppComponent::move_cursor(const int delta) {
  std::lock_guard lock(panel_->mutex);
  // A confirm dialog freezes the highlight: moving it names something else.
  if (panel_->confirming_uninstall || panel_->confirming_kill ||
      panel_->confirming_recipe_remove) {
    return;
  }
  std::size_t* cursor = nullptr;
  std::size_t count = 0;
  switch (tabs()[panel_->tab].tab) {
    case Tab::kCatalog:
      return;
    case Tab::kDeploy:
      cursor = &panel_->catalog_cursor;
      count = panel_->catalog.recipes.size();
      break;
    case Tab::kServices:
      cursor = &panel_->services_cursor;
      count = panel_->rows.size();
      break;
    case Tab::kLogs:
      return;
  }
  if (cursor == nullptr || count == 0) {
    return;
  }
  // Wrap without a signed modulo: down-by-one is +(count - 1).
  const std::size_t offset =
      delta >= 0 ? static_cast<std::size_t>(delta) : count - 1;
  *cursor = (*cursor + offset) % count;
}

void AppComponent::handle_deploy_key(const std::string& key) {
  if (key == "c") {
    // ONE key for both, but only THAT operation's flag (§6.7).
    std::shared_ptr<Op> draft;
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->draft_running) {
        draft = panel_->draft_op;
      }
    }
    if (draft != nullptr) {
      draft->cancel.store(true, std::memory_order_relaxed);
      request_render(*panel_);
      return;
    }
    cancel_deploy(panel_);
    return;
  }
  if (key == "d") {
    accept_draft(panel_, panel_->paths, /*deploy_after=*/true, /*force=*/false);
    return;
  }
  if (key == "s") {
    accept_draft(panel_, panel_->paths, /*deploy_after=*/false,
                 /*force=*/false);
    return;
  }
  if (key == "e") {
    std::lock_guard lock(panel_->mutex);
    panel_->catalog_details = !panel_->catalog_details;
    return;
  }
  if (key == "E") {
    // Shift-E, because 'e' is the details toggle; builtins are refused.
    open_edit();
    return;
  }
  if (key == "x") {
    std::lock_guard lock(panel_->mutex);
    if (panel_->draft_ready || panel_->draft_running || panel_->draft_storing ||
        panel_->edit_active || panel_->catalog_loading ||
        panel_->catalog_auto || panel_->recipe_removing ||
        panel_->deploy_in_flight) {
      return;
    }
    if (panel_->catalog.recipes.empty() ||
        panel_->catalog_cursor >= panel_->catalog.recipes.size()) {
      return;
    }
    const RecipeSource source = panel_->catalog.sources[panel_->catalog_cursor];
    const RecipeRemoval removal = recipe_removal(source);
    if (!removal.removable) {
      // Refused on screen with a message: never a silent no-op, never a delete.
      panel_->catalog_status = removal.refusal;
      return;
    }
    panel_->recipe_remove_target =
        panel_->catalog.recipes[panel_->catalog_cursor].service_name;
    panel_->confirming_recipe_remove = true;
    return;
  }
  if (key == "r") {
    refresh_catalog(panel_, panel_->paths);
  }
}

ftxui::ComponentBase* AppComponent::edit_input_for(const EditField field) {
  switch (field) {
    case EditField::kStartCmd:
      return edit_start_cmd_.get();
    case EditField::kBind:
      return edit_bind_.get();
    case EditField::kHealthPath:
      return edit_health_path_.get();
    case EditField::kDisplayName:
      return edit_display_name_.get();
    case EditField::kDescription:
      return edit_description_.get();
    case EditField::kPort:
      return edit_port_.get();
    case EditField::kMemoryMax:
      return edit_memory_max_.get();
    case EditField::kTasksMax:
      return edit_tasks_max_.get();
    case EditField::kNone:
      return nullptr;
  }
  return nullptr;
}

ftxui::Elements AppComponent::edit_input_elements() {
  return {edit_start_cmd_->Render(),   edit_bind_->Render(),
          edit_health_path_->Render(), edit_display_name_->Render(),
          edit_description_->Render(), edit_port_->Render(),
          edit_memory_max_->Render(),  edit_tasks_max_->Render()};
}

ftxui::ComponentBase* AppComponent::create_input_for(const CreateField field) {
  switch (field) {
    case CreateField::kUrl:
      return create_url_.get();
    case CreateField::kRef:
      return create_ref_.get();
    case CreateField::kDeployPort:
      return create_deploy_port_.get();
    case CreateField::kStartCmd:
      return create_start_cmd_.get();
    case CreateField::kBind:
      return create_bind_.get();
    case CreateField::kHealthPath:
      return create_health_path_.get();
    case CreateField::kDisplayName:
      return create_display_name_.get();
    case CreateField::kDescription:
      return create_description_.get();
    case CreateField::kAppPort:
      return create_app_port_.get();
    case CreateField::kMemoryMax:
      return create_memory_max_.get();
    case CreateField::kTasksMax:
      return create_tasks_max_.get();
    case CreateField::kNone:
      return nullptr;
  }
  return nullptr;
}

ftxui::Elements AppComponent::create_input_elements() {
  return {create_url_->Render(),          create_ref_->Render(),
          create_deploy_port_->Render(),  create_start_cmd_->Render(),
          create_bind_->Render(),         create_health_path_->Render(),
          create_display_name_->Render(), create_description_->Render(),
          create_app_port_->Render(),     create_memory_max_->Render(),
          create_tasks_max_->Render()};
}

void AppComponent::set_create_field(const CreateField field) {
  {
    std::lock_guard lock(panel_->mutex);
    panel_->create_field = field;
  }
  request_render(*panel_);
}

void AppComponent::open_create() {
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->create_running || panel_->deploy_in_flight) {
      return;
    }
    panel_->create_active = true;
    panel_->create_url.clear();
    panel_->create_ref.clear();
    panel_->create_deploy_port.clear();
    panel_->create = EditFields{};
    panel_->create_status.clear();
    panel_->create_error.clear();
  }
  set_create_field(CreateField::kNone);
}

void AppComponent::close_create() {
  {
    std::lock_guard lock(panel_->mutex);
    panel_->create_active = false;
    panel_->create_field = CreateField::kNone;
  }
  request_render(*panel_);
}

void AppComponent::submit_create() {
  berth::DraftRequest request;
  EditFields fields;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->create_running) {
      return;
    }
    request.url = panel_->create_url;
    request.ref = panel_->create_ref;
    fields = panel_->create;
  }
  start_create_recipe(panel_, panel_->paths, request, fields);
  set_create_field(CreateField::kNone);
}

bool AppComponent::handle_create_key(const ftxui::Event& event,
                                     const std::string& key) {
  bool open = false;
  bool running = false;
  bool editing = false;
  CreateField focus = CreateField::kNone;
  {
    std::lock_guard lock(panel_->mutex);
    open = panel_->create_active || panel_->create_running;
    running = panel_->create_running;
    editing = panel_->edit_active;
    focus = panel_->create_field;
  }
  if (!open && !editing && key == "i") {
    open_create();
    return true;
  }
  if (!open) {
    return false;
  }
  if (running) {
    if (key == "c") {
      std::lock_guard lock(panel_->mutex);
      if (panel_->create_op) {
        panel_->create_op->cancel.store(true, std::memory_order_relaxed);
      }
      request_render(*panel_);
    }
    return true;
  }
  ftxui::ComponentBase* focused = create_input_for(focus);
  if (focused != nullptr) {
    if (event == Event::Escape) {
      set_create_field(CreateField::kNone);
      return true;
    }
    if (event == Event::Tab) {
      set_create_field(next_create_field(focus));
      return true;
    }
    if (key_goes_to_field(event)) {
      focused->OnEvent(event);
      return true;
    }
    if (event == Event::Return) {
      submit_create();
      return true;
    }
  }
  if (event == Event::Escape) {
    close_create();
    return true;
  }
  if (key == "i") {
    set_create_field(CreateField::kUrl);
    return true;
  }
  if (event == Event::Return) {
    submit_create();
    return true;
  }
  if (focus == CreateField::kNone && key == "q") {
    return false;  // fall through to the global quit
  }
  return true;
}

void AppComponent::set_edit_field(const EditField field) {
  {
    std::lock_guard lock(panel_->mutex);
    panel_->edit_field = field;
  }
  request_render(*panel_);
}

void AppComponent::open_edit() {
  bool refused = false;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->catalog.recipes.empty() ||
        panel_->catalog_cursor >= panel_->catalog.recipes.size()) {
      return;
    }
    const RecipeSource source = panel_->catalog.sources[panel_->catalog_cursor];
    const std::string refusal = edit_refusal_message(source);
    if (!refusal.empty()) {
      panel_->edit_error = refusal;
      panel_->edit_status.clear();
      refused = true;
    } else {
      const berth::Recipe& recipe =
          panel_->catalog.recipes[panel_->catalog_cursor];
      panel_->edit_service = recipe.service_name;
      panel_->edit = edit_fields_from_recipe(recipe);
      panel_->edit_active = true;
      panel_->edit_field = EditField::kNone;
      panel_->edit_saved = false;
      panel_->edit_stored_path.clear();
      panel_->edit_diff.clear();
      panel_->edit_status.clear();
      panel_->edit_error.clear();
    }
  }
  if (refused) {
    request_render(*panel_);
    return;
  }
  set_edit_field(EditField::kNone);
}

void AppComponent::close_edit() {
  {
    std::lock_guard lock(panel_->mutex);
    panel_->edit_active = false;
    panel_->edit_field = EditField::kNone;
  }
  request_render(*panel_);
}

void AppComponent::save_edit() {
  berth::RecipeEdits edits;
  bool refused = false;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->edit_running) {
      return;
    }
    const berth::Result<berth::RecipeEdits> assembled =
        assemble_recipe_edits(panel_->edit);
    if (assembled.is_err()) {
      panel_->edit_error = assembled.error().ctx();
      panel_->edit_status = "the edit was NOT saved";
      refused = true;
    } else {
      edits = assembled.value();
      panel_->edit_error.clear();
    }
  }
  if (refused) {
    request_render(*panel_);
    return;
  }
  start_recipe_edit(panel_, panel_->paths, edits);
}

bool AppComponent::handle_edit_key(const ftxui::Event& event,
                                   const std::string& key) {
  bool active = false;
  bool running = false;
  EditField focus = EditField::kNone;
  {
    std::lock_guard lock(panel_->mutex);
    active = panel_->edit_active;
    running = panel_->edit_running;
    focus = panel_->edit_field;
  }
  if (!active) {
    return false;
  }
  // Inside a field 'q' is text and Esc is the way out (§12.16).
  if (focus == EditField::kNone && key == "q") {
    return false;
  }
  if (event == Event::Escape) {
    if (focus != EditField::kNone) {
      set_edit_field(EditField::kNone);
    } else {
      close_edit();
    }
    return true;
  }
  if (running) {
    return true;  // the write is in flight
  }
  if ((event == Event::Tab || event == Event::Return) &&
      focus != EditField::kNone) {
    set_edit_field(next_edit_field(focus));
    return true;
  }
  if (focus != EditField::kNone && key_goes_to_field(event)) {
    ftxui::ComponentBase* input = edit_input_for(focus);
    if (input != nullptr) {
      input->OnEvent(event);
      return true;
    }
  }
  if (focus == EditField::kNone) {
    if (key == "i") {
      set_edit_field(EditField::kStartCmd);
      return true;
    }
    if (key == "s") {
      save_edit();
      return true;
    }
  }
  return true;
}

bool AppComponent::scroll_logs(const int delta) {
  if (tabs()[panel_->tab].tab != Tab::kLogs) {
    return false;
  }
  std::lock_guard lock(panel_->mutex);
  if (delta > 0) {
    ++panel_->log_scroll;
  } else if (panel_->log_scroll > 0) {
    // Stops AT zero, where the view is pinned: a log follows its own tail.
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
      panel_->logs_lines.clear();
    }
    refresh_logs(panel_, panel_->paths);
    return;
  }
  if (key == "r" || event == Event::Return) {
    refresh_logs(panel_, panel_->paths);
  }
}

bool AppComponent::start_selected_deploy() {
  berth::Recipe recipe;
  berth::Paths paths;
  std::uint16_t port = 0;
  {
    std::lock_guard lock(panel_->mutex);
    if (panel_->catalog_cursor >= panel_->catalog.recipes.size()) {
      return false;
    }
    recipe = panel_->catalog.recipes[panel_->catalog_cursor];
    paths = panel_->paths;
    // Loud refusal before a worker spawns: coercing to 0 deploys elsewhere.
    const berth::Result<std::uint16_t> parsed =
        parse_port_field(panel_->create_deploy_port);
    if (parsed.is_err()) {
      panel_->deploy_finished = false;
      panel_->deploy_cancelled = false;
      panel_->deploy_error = parsed.error().ctx();
      panel_->deploy_status = "no deploy started";
      return false;
    }
    port = std::move(parsed).value();
  }
  start_deploy(panel_, paths, recipe, port);
  return true;
}

void AppComponent::handle_services_key(const std::string& key) {
  std::string name;
  {
    std::lock_guard lock(panel_->mutex);
    // No selection, an action, or a refresh in flight: rows may be mid-replace.
    if (panel_->rows.empty() || !panel_->action.empty() ||
        panel_->services_loading || panel_->services_auto) {
      return;
    }
    if (panel_->services_cursor >= panel_->rows.size()) {
      abort_bug("no service row selected");
    }
    const berth::ServiceRow selected = panel_->rows[panel_->services_cursor];
    name = selected.service_name;
    if (key == "u") {
      panel_->uninstall_target = selected;
      panel_->confirming_uninstall = true;
      return;
    }
    // K, not k: 'k' is the vim cursor key panel-wide.
    if (key == "K") {
      panel_->kill_target = selected;
      panel_->confirming_kill = true;
      return;
    }
  }
  if (key == "s") {
    service_action_start(panel_, name);
  } else if (key == "x") {
    service_action_stop(panel_, name);
  } else if (key == "t") {
    service_action_restart(panel_, name);
  } else if (key == "p") {
    service_action_freeze(panel_, name);
  }
}

Element AppComponent::render_content() {
  if (panel_->confirming_kill) {
    return confirm_kill_screen(*panel_);
  }
  if (panel_->confirming_uninstall) {
    return confirm_uninstall_screen(*panel_);
  }
  switch (tabs()[panel_->tab].tab) {
    case Tab::kCatalog:
      return render_scan_screen(*panel_);
    case Tab::kDeploy:
      if (panel_->edit_active) {
        return render_edit_screen(*panel_, edit_input_elements());
      }
      return render_deploy_screen(*panel_, create_input_elements());
    case Tab::kServices:
      return render_services_screen(*panel_);
    case Tab::kLogs:
      return render_logs_screen(*panel_);
  }
  abort_bug("unknown tab in render_content");
}

// One detached ticker for the two self-updating lists. The PURE
// auto_refresh_due() gates it; the tab SCOPES it: only the visible list.
void start_auto_refresh(const std::shared_ptr<Panel>& panel) {
  std::thread ticker([panel]() {
    // Wake often enough that a quit is not held by the sleep.
    constexpr auto kTickWake = std::chrono::milliseconds(250);
    auto last = std::chrono::steady_clock::now();
    while (!panel->shutting_down.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(kTickWake);
      if (panel->shutting_down.load(std::memory_order_relaxed)) {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      const auto since =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
      bool services_tab = false;
      bool deploy_tab = false;
      bool services_busy = false;
      bool catalog_busy = false;
      {
        std::lock_guard lock(panel->mutex);
        const Tab active = tabs()[panel->tab].tab;
        services_tab = active == Tab::kServices;
        deploy_tab = active == Tab::kDeploy;
        services_busy = panel->services_loading || panel->services_auto ||
                        !panel->action.empty() || panel->confirming_uninstall ||
                        panel->confirming_kill;
        catalog_busy = panel->catalog_loading || panel->catalog_auto ||
                       panel->recipe_removing || panel->edit_active ||
                       panel->edit_running || panel->confirming_recipe_remove ||
                       panel->deploy_in_flight || panel->draft_running ||
                       panel->draft_ready;
      }
      if (services_tab &&
          auto_refresh_due({services_tab, services_busy, false, since})) {
        refresh_services(panel, panel->paths, /*background=*/true);
        last = now;
      } else if (deploy_tab &&
                 auto_refresh_due({deploy_tab, catalog_busy, false, since})) {
        refresh_catalog(panel, panel->paths, /*background=*/true);
        last = now;
      }
    }
  });
  ticker.detach();
}

void start_cancel_watcher(const std::shared_ptr<Panel>& panel,
                          const std::shared_ptr<std::atomic<bool>>& signalled) {
  std::thread watcher([panel, signalled]() {
    // Stop on a normal quit too: after Loop the ScreenInteractive is gone.
    while (!signalled->load(std::memory_order_relaxed) &&
           !panel->shutting_down.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(kCancelPoll);
    }
    if (panel->shutting_down.load(std::memory_order_relaxed)) {
      return;
    }
    static_cast<void>(panel->ops.cancel_all());
    panel->screen->Exit();
  });
  watcher.detach();
}

}
int run_tui(const berth::Paths& paths) {
  auto panel = std::make_shared<Panel>();
  panel->paths = paths;
  berth::netstatus_reset();

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
  // Ctrl-C is ours to handle, so FTXUI must not turn it into its own SIGABRT.
  screen.ForceHandleCtrlC(false);

  auto app = std::make_shared<AppComponent>(panel);
  app->start();
  start_auto_refresh(panel);
  start_cancel_watcher(panel, signalled);
  screen.Loop(app);

  panel->shutting_down.store(true);

  // WAIT bounded: a worker that ignores the request is left detached, loudly.
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

}
