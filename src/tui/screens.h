#pragma once

// Each screen has exactly two entry points, and the split is the rule:
// `refresh_*` blocks and MUST run on a worker, `render_*` must not block and
// runs on the render thread with panel.mutex held. The strings both agree on
// live in the pure view models (view_*.h).
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

// `background` is the silent auto-refresh tick: the same in-flight guard, but
// no status line and no loading flag, so a periodic refresh can never clobber
// an action's verdict or flicker the empty-state text.
void refresh_catalog(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths, bool background = false);

// Reloads the catalog in the SAME worker, so the list can never race the
// write. It does NOT touch a deployed service — that is uninstalled on the My
// Services tab. The confirmation is UI; the removal is not.
void remove_user_recipe(const std::shared_ptr<Panel>& panel,
                        const berth::Paths& paths,
                        const std::string& service_name);

// ONE deploy at a time; the engine's own deploy lock is the second line of
// defence against a CLI deploy, not a replacement for this guard. `pre_store`
// runs on the SAME worker immediately before the deploy and aborts it on
// failure, so a store that fails is never deployed.
using DeployPreStore = std::function<berth::Result<void>()>;
void start_deploy(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const berth::Recipe& recipe,
                  std::uint16_t forced_port, DeployPreStore pre_store = {});

// The journal entry stays for __cleanup (§6.7).
void cancel_deploy(const std::shared_ptr<Panel>& panel);

// The user's own eight values, applied ON TOP of what detection found, through
// the PURE engine (apply_recipe_edits, which runs validate_recipe). Nothing is
// stored by this call: it ends with a draft on the review, where s/d keep their
// own keys. Whether to write is a human decision, with its own key.
void start_create_recipe(const std::shared_ptr<Panel>& panel,
                         const berth::Paths& paths,
                         const berth::DraftRequest& request,
                         const EditFields& fields);

// Always stores before deploying, so what deploys is what is on disk.
void accept_draft(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, bool deploy_after, bool force);

void discard_draft(const std::shared_ptr<Panel>& panel);

// `edits` is already assembled and numerically validated on the render thread
// (assemble_recipe_edits); this worker calls user_recipe_update — the ONE
// engine — and reloads the catalog.
void start_recipe_edit(const std::shared_ptr<Panel>& panel,
                       const berth::Paths& paths,
                       const berth::RecipeEdits& edits);

enum class DraftKey {
  kConfirmed,  // y: store with --force semantics
  kEaten,      // anything else: the modal keeps the key
  kDismissed,  // n / Escape: nothing is overwritten
};
[[nodiscard]] DraftKey handle_draft_clash_key(Panel& panel,
                                              const std::string& key);

// Refuses to start while one is in flight.
void run_system_scan(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths);

// Reads at most kTailWindowBytes from the END of the file — never the whole
// thing, and never on the render thread. An absent file is not an error.
void refresh_logs(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths);

void refresh_services(const std::shared_ptr<Panel>& panel,
                      const berth::Paths& paths, bool background = false);

// `uninstall` takes the confirmation decision as data: dialog is UI, this is
// not.
void service_action_start(const std::shared_ptr<Panel>& panel,
                          const std::string& service_name);
void service_action_stop(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name);
void service_action_restart(const std::shared_ptr<Panel>& panel,
                            const std::string& service_name);
void service_action_kill(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name, bool confirmed);
// The engine reads the unit's SubState back; the row is updated from what it
// actually says.
void service_action_freeze(const std::shared_ptr<Panel>& panel,
                           const std::string& service_name);
void service_action_uninstall(const std::shared_ptr<Panel>& panel,
                              const berth::Paths& paths,
                              const std::string& service_name);

// Render-thread entry points; all of them assume panel.mutex is already held.
[[nodiscard]] ftxui::Element render_scan_screen(const Panel& panel);
[[nodiscard]] ftxui::Element render_logs_screen(const Panel& panel);
[[nodiscard]] ftxui::Element render_services_screen(const Panel& panel);
[[nodiscard]] ftxui::Element confirm_uninstall_screen(const Panel& panel);
[[nodiscard]] ftxui::Element confirm_kill_screen(const Panel& panel);

// The creation form's Inputs are passed in because a Component's Render()
// belongs to the component tree in app.cpp.
[[nodiscard]] ftxui::Element render_deploy_screen(
    const Panel& panel, const ftxui::Elements& create_inputs);

// Same, in EditField order (start_cmd .. tasks_max).
[[nodiscard]] ftxui::Element render_edit_screen(
    const Panel& panel, const ftxui::Elements& edit_inputs);

enum class UninstallKey {
  kConfirmed,  // y: the dialog closed AND the caller must uninstall
  kEaten,      // the dialog consumed it; nothing below it may react
  kDismissed,  // n / Escape: the dialog closed
  kQuit,       // q: the dialog closed AND the panel should quit
};
[[nodiscard]] UninstallKey handle_uninstall_key(Panel& panel,
                                                const std::string& key);

[[nodiscard]] UninstallKey handle_kill_key(Panel& panel,
                                           const std::string& key);

}
