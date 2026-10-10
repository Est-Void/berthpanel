#pragma once

// src/tui/screens.h — the four screens (System Scan 6.5, Deploy 6.3, My
// Services 6.4, Logs 6.6) and their workers.
//
// Each screen gets exactly three entry points, and the split is the whole
// design: `refresh_*` blocks and must run on a worker, `render_*` must not
// block and runs on the render thread, and the pure view models in
// view_*.h hold the strings both of them agree on. Nothing here touches the
// tab index or the chrome — that is app.cpp's.

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "../berth/recipe_edit.h"
#include "../berth/recipes.h"
#include "../core/paths.h"
#include "panel.h"

namespace tui {

// Catalog (6.2): reads the builtin catalog and the user recipes. Two file
// reads; still a worker, because the rule "nothing blocks the render thread"
// is absolute and a rule with an exception is a rule without one. The RECIPE
// LIST itself lives on the Deploy screen, where a recipe becomes an action.
// `background` is the silent auto-refresh tick: it takes the same in-flight
// guard but touches no status line and no user-visible loading flag, so a
// periodic refresh can never clobber an action's verdict or flicker the
// empty-state text.
void refresh_catalog(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths, bool background = false);

// Removes the SELECTED user recipe from the catalog: calls the ENGINE
// `user_recipe_remove(paths, service_name)` on a worker, then reloads the
// catalog in the SAME worker so the list can never race the write. It does
// NOT touch a deployed service — that is uninstalled on the My Services tab.
// The builtin refusal is decided on the render thread (recipe_removal), so
// this is only reached for a user recipe. The confirmation is UI; the removal
// is not, exactly like the uninstall dialog.
void remove_user_recipe(const std::shared_ptr<Panel>& panel,
                        const berth::Paths& paths,
                        const std::string& service_name);

// Deploy (6.3): ONE deploy at a time, on a worker, with the engine's cancel
// flag behind the panel's. `forced_port` 0 means "ask the recipe, then pick a
// free port" — the port field's empty case. Refuses to start while another
// deploy is in flight; the engine's own deploy lock is the second line of
// defence against a CLI deploy, not a replacement for this guard.
//
// `pre_store`, when set, runs on the SAME worker immediately before the deploy
// and aborts it on failure. That is what makes "d = store and deploy" honest:
// the store is not a separate async step whose result can arrive after the
// deploy already started, so a draft that was not written is never deployed.
using DeployPreStore = std::function<berth::Result<void>()>;
void start_deploy(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const berth::Recipe& recipe,
                  std::uint16_t forced_port, DeployPreStore pre_store = {});

// Flips the cancel flag of the deploy in flight. The engine kills its
// subprocesses and returns; the journal entry stays for __cleanup (§6.7).
void cancel_deploy(const std::shared_ptr<Panel>& panel);

// BYO wizard (plan 4.21). prepare_draft() is expensive — an API call, a clone,
// a tree walk — so it runs on a worker like everything else here.
void start_draft(const std::shared_ptr<Panel>& panel,
                 const berth::Paths& paths);

// The reviewed draft's next step: store it (always before a deploy, so what
// deploys is what is on disk) and optionally deploy it right away. A name that
// is already taken opens the confirmation instead of clobbering; the modal's
// answer calls back in here with `force`.
void accept_draft(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, bool deploy_after, bool force);

// Throws the reviewed draft away without storing it.
void discard_draft(const std::shared_ptr<Panel>& panel);

// Edit an existing USER recipe (stage 4.19+). `edits` comes from the form,
// already assembled and numerically validated on the render thread
// (assemble_recipe_edits); this worker calls user_recipe_update — the ONE
// engine — and reloads the catalog, so the list shows what was just stored.
// A builtin-only or unknown service is refused by the engine; nothing is
// written and the error is shown verbatim.
void start_recipe_edit(const std::shared_ptr<Panel>& panel,
                       const berth::Paths& paths,
                       const berth::RecipeEdits& edits);

// The overwrite confirmation. Same shape as the uninstall dialog: the dialog
// decides, the caller acts.
enum class DraftKey {
  kConfirmed,  // y: store with --force semantics
  kEaten,      // anything else: the modal keeps the key
  kDismissed,  // n / Escape: nothing is overwritten
};
[[nodiscard]] DraftKey handle_draft_clash_key(Panel& panel,
                                              const std::string& key);

// System Scan (6.5): prerequisites, listening ports, service state, and ONE
// bounded allowlisted GET. Refuses to start while one is in flight.
void run_system_scan(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths);

// Logs (6.6): the tail of the selected log, on a worker. Reads at most
// kTailWindowBytes from the END of the file — never the whole thing, and never
// on the render thread. An absent file is not an error (tail.h).
void refresh_logs(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths);

// My Services (6.4): the registry plus one bounded `systemctl is-active` per
// row. Requested by the user (Enter/r) or by the periodic ticker; never polled
// from the render loop. `background` is the silent tick (see refresh_catalog).
void refresh_services(const std::shared_ptr<Panel>& panel,
                      const berth::Paths& paths, bool background = false);

// start / stop / uninstall for the selected row. `uninstall` takes the
// confirmation decision as data, because the dialog is UI and this is not.
void service_action_start(const std::shared_ptr<Panel>& panel,
                          const std::string& service_name);
void service_action_stop(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name);
void service_action_restart(const std::shared_ptr<Panel>& panel,
                            const std::string& service_name);
// SIGKILL. The confirmation is UI, the kill is not: `confirmed` is the dialog's
// answer as data, exactly like `uninstall`.
void service_action_kill(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name, bool confirmed);
// Freeze / unfreeze (SIGSTOP / SIGCONT). The engine reads the unit's SubState
// back afterwards, and the row is updated from what it actually says.
void service_action_freeze(const std::shared_ptr<Panel>& panel,
                           const std::string& service_name);
void service_action_uninstall(const std::shared_ptr<Panel>& panel,
                              const berth::Paths& paths,
                              const std::string& service_name);

// Render-thread entry points. All of them assume panel.mutex is already held.
[[nodiscard]] ftxui::Element render_scan_screen(const Panel& panel);
[[nodiscard]] ftxui::Element render_logs_screen(const Panel& panel);
[[nodiscard]] ftxui::Element render_services_screen(const Panel& panel);
[[nodiscard]] ftxui::Element confirm_uninstall_screen(const Panel& panel);
[[nodiscard]] ftxui::Element confirm_kill_screen(const Panel& panel);

// Deploy (6.3, 4.21). The three FTXUI Inputs the app owns are passed in
// because a Component's Render() belongs to the component tree in app.cpp.
[[nodiscard]] ftxui::Element render_deploy_screen(const Panel& panel,
                                                  ftxui::Element url_input,
                                                  ftxui::Element ref_input,
                                                  ftxui::Element port_input);

// Recipe edit (4.19+). The eight FTXUI Inputs are passed in, in EditField
// order (start_cmd .. tasks_max), because their Render() belongs to the
// component tree in app.cpp. The screen only places them.
[[nodiscard]] ftxui::Element render_edit_screen(
    const Panel& panel, const ftxui::Elements& edit_inputs);

// What the modal confirmation did with a key. The dialog decides, the caller
// acts: kConfirmed means "run the uninstall", and nothing else does.
enum class UninstallKey {
  kConfirmed,  // y: the dialog closed AND the caller must uninstall
  kEaten,      // the dialog consumed it; nothing below it may react
  kDismissed,  // n / Escape: the dialog closed
  kQuit,       // q: the dialog closed AND the panel should quit
};
[[nodiscard]] UninstallKey handle_uninstall_key(Panel& panel,
                                                const std::string& key);

// The kill confirmation: the same shape, the same contract — the caller runs
// the kill, the dialog only decides.
[[nodiscard]] UninstallKey handle_kill_key(Panel& panel,
                                           const std::string& key);

}  // namespace tui