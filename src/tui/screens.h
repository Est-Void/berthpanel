#pragma once

// src/tui/screens.h — the two screens that exist in this stage (Catalog 6.2,
// My Services 6.4) and their workers.
//
// Each screen gets exactly three entry points, and the split is the whole
// design: `refresh_*` blocks and must run on a worker, `render_*` must not
// block and runs on the render thread, and the pure view models in
// view_catalog.h / view_services.h hold the strings both of them agree on.
// Nothing here touches the tab index or the chrome — that is app.cpp's.

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "../core/paths.h"
#include "panel.h"

namespace tui {

// Catalog (6.2): reads the builtin catalog and the user recipes. Two file
// reads; still a worker, because the rule "nothing blocks the render thread"
// is absolute and a rule with an exception is a rule without one.
void refresh_catalog(const std::shared_ptr<Panel>& panel,
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

// Render-thread entry points. Both assume panel.mutex is already held.
[[nodiscard]] ftxui::Element render_catalog_screen(const Panel& panel);
[[nodiscard]] ftxui::Element render_services_screen(const Panel& panel);
[[nodiscard]] ftxui::Element confirm_uninstall_screen(const Panel& panel);

// The uninstall confirmation. Returns false when the dialog is up, so the
// caller knows the key was eaten and must not act on it.
bool handle_uninstall_key(Panel& panel, const std::string& key);

}  // namespace tui