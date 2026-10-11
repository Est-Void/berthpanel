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
#include "view_catalog.h"
#include "view_deploy.h"
#include "view_edit.h"
#include "view_logs.h"
#include "view_refresh.h"

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

// The keys a focused text field owns. Everything TYPEABLE, because the fields
// are behind an explicit insert mode: with no field focused, q / e / r / c / j
// / k are the screen's own, and a port that is not a number is refused loudly
// by parse_port_field rather than by silently dropping keystrokes. Left/Right
// and Home/End move the caret inside the field; Up/Down stay global (list/log
// navigation), so they are deliberately NOT forwarded.
bool key_goes_to_field(const ftxui::Event& event) {
  return event.is_character() || event == Event::Backspace ||
         event == Event::Delete || event == Event::ArrowLeft ||
         event == Event::ArrowRight || event == Event::Home ||
         event == Event::End;
}

class AppComponent final : public ftxui::ComponentBase {
 public:
  explicit AppComponent(const std::shared_ptr<Panel>& panel) : panel_(panel) {
    // The three text fields are Components because typing IS a component:
    // app.cpp owns the event forwarding and the focus marker, screens.cpp only
    // draws what they return. Each Enter means its own thing, so the launch
    // key and the field cannot drift apart.
    // A standalone Input has no inactive parent, so Focused() is ALWAYS true
    // and InputOption::Default() paints it `inverted` — a white block on a dark
    // terminal. The panel draws its own focus marker (CreateField), so this
    // style only has to render text: white, dim placeholder, cursor intact.
    const auto field_style = [](ftxui::InputState state) {
      state.element |= ftxui::color(ftxui::Color::White);
      if (state.is_placeholder) {
        state.element |= ftxui::dim;
      }
      return state.element;
    };

    // The eight recipe-edit fields. Same shape as the creation form's: a bare
    // Input that only renders text, because the panel draws its own focus
    // marker (EditField / CreateField). Numeric fields are text too;
    // assemble_recipe_edits() parses them loudly on save rather than dropping
    // keystrokes (§12.16).
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

    // The creation form's eleven fields, same bare Input shape: the panel draws
    // the focus marker (CreateField) and parses the numbers on create, not as
    // they are typed (§12.16). The eight recipe values are EditFields, so the
    // loud parsers and the ONE validator are the ones the edit screen uses.
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
    // Esc arrives as a Special event with an EMPTY character, so a string-keyed
    // dialog would never see it; name it "\x1b" for every dialog below.
    const std::string dialog_key = event == Event::Escape ? "\x1b" : key;

    // Ctrl-C is ours, not FTXUI's: it cancels what is running and takes the
    // SAME path as q (screen.ForceHandleCtrlC(false) in run_tui). Handled
    // before everything else, including the modal — a quit must not be stuck
    // behind a dialog.
    if (event == Event::CtrlC) {
      quit(panel_->ops.cancel_all());
      return true;
    }

    // The modals below read and mutate Panel fields that a worker's apply
    // callback can also touch (draft_clash_confirming, confirming_kill,
    // confirming_uninstall): the read-and-decide is done under the panel lock.
    // The ACTION is taken with the lock released, because the action functions
    // take it themselves.

    // The overwrite question is modal, and it is FIRST: a dialog that answers
    // "store this over an existing recipe" must not be reachable while another
    // dialog is open.
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

    // The confirmation is modal: it takes the key and nothing below it may
    // react. The dialog DECIDES, the caller ACTS — a confirmation that returns
    // "eaten" without an uninstall behind it is a dialog that lies (§11).
    // SIGKILL is the most destructive thing here, so its question is asked
    // before anything else can consume the key.
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
        // The kill dialog owns "q" the same way the uninstall dialog does.
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

    // Removing a recipe is a delete, so it asks first, and the dialog says
    // what it deletes and what it does NOT (a deployed service). It is the
    // LAST modal checked so a more destructive question is never buried.
    bool remove_confirming = false;
    RemoveConfirm remove_decision = RemoveConfirm::kEaten;
    std::string remove_target;
    {
      std::lock_guard lock(panel_->mutex);
      if (panel_->confirming_recipe_remove) {
        remove_confirming = true;
        remove_decision = recipe_remove_key(dialog_key);
        remove_target = panel_->recipe_remove_target;
        // The dialog DECIDED: close it before the caller acts, so a slow
        // worker cannot leave the question on screen over the outcome.
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

    // Insert mode. The fields take typeable keys ONLY while one of them is
    // focused ('i' enters, Esc leaves) — otherwise 'q' would be a letter in the
    // URL box and the panel could not be quit from this tab (§12.16). During a
    // deploy or a draft the fields are locked, which is also what frees 'c' to
    // mean cancel.
    const bool deploy_tab = tabs()[panel_->tab].tab == Tab::kDeploy;
    if (deploy_tab) {
      // The CREATION form owns the Deploy tab's keys while it is open. It comes
      // first, before the edit screen and the draft fields: it is the only
      // thing on screen while it is up, so Esc must leave IT rather than quit,
      // and j/k must not move the recipe cursor under it.
      if (handle_create_key(event, key)) {
        return true;
      }
      // The edit sub-screen owns the Deploy tab's keys while it is open. It
      // comes FIRST so Esc releases a field or leaves the screen instead of
      // quitting, and so j/k cannot move the recipe cursor under the editor.
      if (handle_edit_key(event, key)) {
        return true;
      }
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
    // A ready draft owns Enter until it is dismissed. Escape drops it (rather
    // than quitting the whole panel) and hands the keys back to the list, so
    // the recipe just stored can be deployed with the next Enter (4.21).
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
        // Enter with NO field focused launches the selected recipe. This is the
        // key the screen advertises; without it the only way to deploy was to
        // press i, Tab, Tab into the port field and press Enter there. A draft
        // that is waiting owns Enter through its own d/s keys instead.
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

  // The focused field's Component, or null when no field is focused.

  // Insert mode on/off. Setting a field re-renders, because the marker moves.

  // Recipe edit (4.19+), the Deploy tab's 'E'. Returns true when it consumed
  // the key; 'q' with no field focused falls through to the global quit.
  bool handle_edit_key(const ftxui::Event& event, const std::string& key);
  ftxui::ComponentBase* edit_input_for(EditField field);
  [[nodiscard]] ftxui::Elements edit_input_elements();
  void set_edit_field(EditField field);
  void open_edit();
  void close_edit();
  void save_edit();

  // The recipe under the cursor plus the port the user typed, validated. Both
  // reads happen under the lock and nothing is spawned from inside it. Returns
  // whether a deploy actually started (false when the port was refused or the
  // catalog is empty), so the caller knows whether to drop insert mode.
  bool start_selected_deploy();

  Element render_content();

  // --- The creation form ----------------------------------------------------
  // 'i' opens ONE form for a new service. It replaces the old three-field
  // draft form: the eight recipe values are typed here, not repaired after the
  // detector's guess was stored. `submit_create` hands them to the worker,
  // which applies them ON TOP of what detection found and hands the result to
  // the existing review (s stores, d stores and deploys).
  ftxui::ComponentBase* create_input_for(CreateField field);
  ftxui::Elements create_input_elements();
  void set_create_field(CreateField field);
  void open_create();
  void close_create();
  void submit_create();

  // The creation form's own key handling. Returns true when the form consumed
  // the key (it is open, or it was open and this key opened/closed it).
  bool handle_create_key(const ftxui::Event& event, const std::string& key);

  std::shared_ptr<Panel> panel_;
  // The eight recipe-edit fields, in EditField order.
  ftxui::Component edit_start_cmd_;
  ftxui::Component edit_bind_;
  ftxui::Component edit_health_path_;
  ftxui::Component edit_display_name_;
  ftxui::Component edit_description_;
  ftxui::Component edit_port_;
  ftxui::Component edit_memory_max_;
  ftxui::Component edit_tasks_max_;
  // The creation form's eleven fields, in CreateField order.
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
  // A confirm dialog is about a NAMED service or recipe ("uninstall
  // berthpanel-memos?"). Moving the highlight underneath it makes the screen
  // offer the user something they are no longer looking at, so the dialog
  // freezes the cursor.
  if (panel_->confirming_uninstall || panel_->confirming_kill ||
      panel_->confirming_recipe_remove) {
    return;
  }
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
    // Whatever is in flight: the deploy's own flag or the draft's. ONE key for
    // both, but only THAT operation's flag — cancelling the draft must not kill
    // a log tail or a service refresh that happens to be running (6.7: one flag
    // per operation, never a panel-wide cancel).
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
    // Quick mode: the reviewed draft is stored AND deployed with one key. The
    // store comes first — what deploys is what is on disk (plan 4.21).
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
    // Shift-E, because 'e' is already the details toggle. Edits are for USER
    // recipes only; a builtin is refused on screen, never silently edited.
    open_edit();
    return;
  }
  if (key == "x") {
    // Removal is Deploy-tab only, and never while a draft or a worker holds
    // the screen: the recipe it names must be the one the user is looking at.
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
      // A builtin is refused with a clear message, on screen — never a silent
      // no-op and never a delete.
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
    // A form that opens with stale values from an abandoned attempt would be
    // describing something the user is not looking at (§11).
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
  // The keys go back to the screen: the form is on its way to the worker, and
  // the next thing the user wants is the review's s/d, not more typing.
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
  // 'i' opens the form from the Deploy tab — the ONLY way in, so it must work
  // from anywhere that is not already the form or the editor.
  if (!open && !editing && key == "i") {
    open_create();
    return true;
  }
  if (!open) {
    return false;
  }
  if (running) {
    // The worker owns the screen. 'c' cancels THIS operation's flag and
    // nothing else — one flag per operation, never a panel-wide cancel (6.7).
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
  // 'q' is the panel's quit everywhere — but only when no field is focused,
  // exactly like the edit screen (§12.16).
  if (focus == CreateField::kNone && key == "q") {
    return false;  // fall through to the global quit
  }
  // The form owns every remaining key: no cursor movement, no tab switch.
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
      // A bad port/tasks is a loud refusal on the render thread, before any
      // worker is spawned — exactly like the deploy screen's port field.
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
  // 'q' is the panel's quit everywhere — but only when a field is NOT focused:
  // inside a field it is text, and Esc (not q) is the way out, exactly as the
  // wizard's insert mode works (§12.16).
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
    return true;  // the write is in flight: swallow everything but q
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
  // The edit screen owns every remaining key: no cursor movement, no tab
  // switch, no accidental deploy while the form is open.
  return true;
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
    // A port that is not a number is a loud refusal here, before a worker is
    // spawned: coercing it to 0 would deploy somewhere the user did not ask
    // for, which is the worst possible reading of "I typed 808O".
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
    // Nothing selected, an action already running, or a refresh in flight (the
    // rows are mid-replace and the cursor may point at a row that is gone): a
    // key press must not queue a subprocess against stale data. A background
    // tick counts: its apply could otherwise overwrite the action's re-list.
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
      // Uninstall deletes things: it asks first, and the dialog says what
      // stays behind.
      panel_->uninstall_target = selected;
      panel_->confirming_uninstall = true;
      return;
    }
    // K, not k: 'k' is the vim cursor key panel-wide, and stealing it from two
    // screens to save one keystroke would break cursor movement everywhere
    // else.
    if (key == "K") {
      // SIGKILL is irreversible for whatever the service had not flushed: it
      // asks, and the dialog names the cost instead of just the verb.
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

// The periodic list ticker (My Services + the Deploy catalog). One detached
// thread waking on a short poll; it asks the PURE auto_refresh_due() before it
// spawns anything, so a tick can never overlap an in-flight refresh (the
// workers' own loading flags are the second line of defence) and never runs
// after q/Ctrl-C has started the unwind (§6.7). The tab SCOPES it: only the
// list the user is actually looking at is refreshed, and each refresh reuses
// its cursor-preserving path, so the selection never jumps.
void start_auto_refresh(const std::shared_ptr<Panel>& panel) {
  std::thread ticker([panel]() {
    // Wake often enough that a quit is not held by the sleep; the DECISION
    // still uses the full cadence through since_last.
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
        // A dialog freezes the cursor, so a refresh under it is a refresh the
        // user cannot see change; treat it as busy too.
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

// SIGINT/SIGTERM are the only thing that can interrupt a quit from outside,
// and they arrive as a flag — there is no "panel-wide cancel flag" any more,
// because a flag nothing polls is exactly the §11 bug. The watcher asks for the
// same shutdown a key press asks for: cancel what is running, then leave the
// loop.
void start_cancel_watcher(const std::shared_ptr<Panel>& panel,
                          const std::shared_ptr<std::atomic<bool>>& signalled) {
  std::thread watcher([panel, signalled]() {
    // Also stop on a normal quit: after Loop returns the stack
    // ScreenInteractive is on its way out, so this thread must not touch
    // panel->screen again. It used to poll signalled forever after a 'q' (a
    // leaked thread) and could call Exit() on a destroyed screen if a killed
    // signal arrived in the window after the loop had already returned.
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
  start_auto_refresh(panel);
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