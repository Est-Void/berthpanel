#pragma once

// src/tui/screens.h — the screens (Catalog 6.2, Deploy 6.3, My Services 6.4)
// and their workers.
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

#include "../berth/recipes.h"
#include "../core/paths.h"
#include "panel.h"

namespace tui {

// Catalog (6.2): reads the builtin catalog and the user recipes. Two file
// reads; still a worker, because the rule "nothing blocks the render thread"
// is absolute and a rule with an exception is a rule without one.
void refresh_catalog(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths);

// Deploy (6.3): ONE deploy at a time, on a worker, with the engine's cancel
// flag behind the panel's. `forced_port` 0 means "ask the recipe, then pick a
// free port" — the port field's empty case. Refuses to start while another
// deploy is in flight; the engine's own deploy lock is the second line of
// defence against a CLI deploy, not a replacement for this guard.
void start_deploy(const std::shared_ptr<Panel>& panel,
                  const berth::Paths& paths, const berth::Recipe& recipe,
                  std::uint16_t forced_port);

// Flips the cancel flag of the deploy in flight. The engine kills its
// subprocesses and returns; the journal entry stays for __cleanup (§6.7).
void cancel_deploy(const std::shared_ptr<Panel>& panel);

// System Scan (6.5): prerequisites, listening ports, service state, and ONE
// bounded allowlisted GET. Refuses to start while one is in flight.
void run_system_scan(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths);

// My Services (6.4): the registry plus one bounded `systemctl is-active` per
// row. Explicitly requested by the user, never polled from the render loop.
void refresh_services(const std::shared_ptr<Panel>& panel,
                      const berth::Paths& paths);

// start / stop / uninstall for the selected row. `uninstall` takes the
// confirmation decision as data, because the dialog is UI and this is not.
void service_action_start(const std::shared_ptr<Panel>& panel,
                          const std::string& service_name);
void service_action_stop(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name);
void service_action_uninstall(const std::shared_ptr<Panel>& panel,
                              const berth::Paths& paths,
                              const std::string& service_name);

// Render-thread entry points. All of them assume panel.mutex is already held.
[[nodiscard]] ftxui::Element render_scan_screen(const Panel& panel);
[[nodiscard]] ftxui::Element render_services_screen(const Panel& panel);
[[nodiscard]] ftxui::Element confirm_uninstall_screen(const Panel& panel);

// Deploy (6.3). `port_input` is the FTXUI Input the app owns; it is passed in
// because a Component's Render() belongs to the component tree in app.cpp.
[[nodiscard]] ftxui::Element render_deploy_screen(const Panel& panel,
                                                  ftxui::Element port_input);

// What the modal confirmation did with a key.
enum class UninstallKey {
  kEaten,      // the dialog consumed it; nothing below it may react
  kDismissed,  // n / Escape: the dialog closed
  kQuit,       // q: the dialog closed AND the panel should quit
};
[[nodiscard]] UninstallKey handle_uninstall_key(Panel& panel,
                                                const std::string& key);

}  // namespace tui