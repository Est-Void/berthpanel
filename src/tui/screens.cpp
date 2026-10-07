// src/tui/screens.cpp — see screens.h for the contract.

#include "screens.h"

#include <string>
#include <utility>

#include "../berth/service_ops.h"
#include "../util/result.h"

namespace tui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

// --- Catalog (6.2) ---------------------------------------------------------

void refresh_catalog(const std::shared_ptr<Panel>& panel,
                     const berth::Paths& paths) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->catalog_loading) {
      return;
    }
    panel->catalog_loading = true;
    panel->catalog_error.clear();
  }
  run_on_worker<Catalog>(
      panel,
      [paths](std::atomic<bool>* cancel) -> berth::Result<Catalog> {
        // Cancelled before it started is not a failure worth rendering; the
        // load simply never happens.
        if (cancel->load()) {
          return berth::Error("cancelled before the catalog read");
        }
        return load_catalog(paths);
      },
      [](Panel& state, berth::Result<Catalog>&& loaded) {
        state.catalog_loading = false;
        if (loaded.is_err()) {
          state.catalog_error = loaded.error().ctx();
          state.catalog = Catalog{};
          state.catalog_cursor = 0;
          return;
        }
        state.catalog = std::move(loaded).value();
        if (state.catalog_cursor >= state.catalog.recipes.size()) {
          state.catalog_cursor = 0;
        }
      });
}

Element render_catalog_screen(const Panel& panel) {
  if (panel.catalog_loading) {
    return notice_box("reading the catalog...");
  }
  if (!panel.catalog_error.empty()) {
    // Loud on purpose: a corrupt recipe must not render as an empty catalog
    // (§11), and the load is an Err precisely so this can say so.
    return notice_box(
        "the catalog could not be read. this is a loud failure on purpose: "
        "a corrupt recipe must never look like an empty catalog.\n\n" +
        panel.catalog_error +
        "\n\nfix the file, then press r to read it again.");
  }
  if (panel.catalog.recipes.empty()) {
    return notice_box(
        "the catalog is empty: no builtin recipes compiled into this binary "
        "and no user recipes under the recipes dir.");
  }

  Elements out;
  out.push_back(ftxui::text("CATALOG") | ftxui::bold |
                ftxui::color(Color::Cyan));
  out.push_back(ftxui::paragraph(
                    "read-only. the System Scan itself (prerequisites, ports, "
                    "interfaces) lands with stage 6.5 — this tab lists "
                    "recipes until then.") |
                ftxui::dim);
  out.push_back(ftxui::separator());
  out.push_back(line_list(catalog_list_lines(panel.catalog),
                          panel.catalog_cursor, Color::Cyan, true));
  out.push_back(ftxui::separator());
  if (panel.catalog_details) {
    out.push_back(paragraph_lines(
        catalog_detail_lines(panel.catalog, panel.catalog_cursor)));
  } else {
    out.push_back(
        ftxui::paragraph("Enter — details for the selected recipe "
                         "(description, tools, build steps, start command)") |
        ftxui::dim);
    out.push_back(ftxui::paragraph(
                      "deploying is not wired into the panel yet: the recipe's "
                      "pinned commit and limits are here, and the deploy "
                      "screen is the next stage.") |
                  ftxui::dim);
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

// --- My Services (6.4) ----------------------------------------------------

void refresh_services(const std::shared_ptr<Panel>& panel,
                      const berth::Paths& paths) {
  {
    std::lock_guard lock(panel->mutex);
    if (panel->services_loading || !panel->action.empty()) {
      return;
    }
    panel->services_loading = true;
    panel->services_status = "querying systemctl --user is-active...";
  }
  run_on_worker<std::vector<berth::ServiceRow>>(
      panel,
      [paths](std::atomic<bool>* cancel) {
        return berth::service_rows(paths, cancel);
      },
      [](Panel& state, berth::Result<std::vector<berth::ServiceRow>>&& rows) {
        state.services_loading = false;
        if (rows.is_err()) {
          state.services_status = rows.error().ctx();
          return;
        }
        state.rows = std::move(rows).value();
        if (state.services_cursor >= state.rows.size()) {
          state.services_cursor = 0;
        }
        if (state.services_status.rfind("querying", 0) == 0) {
          state.services_status.clear();
        }
      });
}

namespace {

// start / stop / uninstall share one shape: a label, one blocking call, and
// the outcome folded into the status line. Only the call differs.
void service_action(const std::shared_ptr<Panel>& panel,
                    const std::string& label,
                    const std::function<berth::Result<void>()>& op) {
  {
    std::lock_guard lock(panel->mutex);
    if (!panel->action.empty()) {
      return;  // one action at a time: a second press is a no-op, not a race
    }
    panel->action = label;
  }
  run_on_worker<void>(
      panel, [op](std::atomic<bool>* /*cancel*/) { return op(); },
      [label](Panel& state, berth::Result<void>&& done) {
        state.action.clear();
        if (done.is_err()) {
          state.services_status = done.error().ctx();
          return;
        }
        state.services_status = label + ": ok";
      });
}

}  // namespace

void service_action_start(const std::shared_ptr<Panel>& panel,
                          const std::string& service_name) {
  service_action(panel, "starting " + service_name, [service_name]() {
    return berth::service_start(service_name);
  });
}

void service_action_stop(const std::shared_ptr<Panel>& panel,
                         const std::string& service_name) {
  service_action(panel, "stopping " + service_name, [service_name]() {
    return berth::service_stop(service_name);
  });
}

void service_action_uninstall(const std::shared_ptr<Panel>& panel,
                              const berth::Paths& paths,
                              const std::string& service_name) {
  service_action(panel, "uninstalling " + service_name,
                 [paths, service_name]() {
                   return berth::service_uninstall(paths, service_name);
                 });
}

Element render_services_screen(const Panel& panel) {
  Elements out;
  out.push_back(ftxui::text("MY SERVICES") | ftxui::bold |
                ftxui::color(Color::Green));

  // A static ellipsis, not an animated spinner: the panel has no animation
  // clock, and a spinner that never ticks lies about progress.
  Elements status;
  if (!panel.action.empty()) {
    status.push_back(ftxui::text("\xe2\x8b\xb0 " + panel.action + "..."));
  } else if (panel.services_loading) {
    status.push_back(ftxui::text("\xe2\x8b\xb0 " + panel.services_status));
  } else if (!panel.services_status.empty()) {
    const bool ok = panel.services_status.find(": ok") != std::string::npos;
    status.push_back(ftxui::text(panel.services_status) |
                     ftxui::color(ok ? Color::Green : Color::Red));
  } else {
    status.push_back(ftxui::text("idle"));
  }
  out.push_back(ftxui::hbox(std::move(status)) | ftxui::dim);
  out.push_back(ftxui::separator());

  if (panel.rows.empty()) {
    out.push_back(paragraph_lines(services_empty_lines()) | ftxui::flex);
  } else {
    out.push_back(line_list(services_table_lines(panel.rows),
                            panel.services_cursor, Color::Green, true));
  }
  out.push_back(ftxui::separator());
  for (const std::string& hint : services_key_hints()) {
    out.push_back(ftxui::paragraph(hint) | ftxui::dim);
  }
  return ftxui::vbox(std::move(out)) | ftxui::flex;
}

Element confirm_uninstall_screen(const Panel& panel) {
  Elements body;
  for (const std::string& line :
       confirm_uninstall_lines(panel.uninstall_target)) {
    body.push_back(line.empty() ? ftxui::text(" ") : ftxui::paragraph(line));
  }
  Element dialog = ftxui::vbox(std::move(body)) |
                   ftxui::borderStyled(ftxui::ROUNDED, Color::Red);
  // Slightly above centre, with the spacers doing the centring — no absolute
  // coordinates, so the dialog survives a resize.
  const Element spacer = ftxui::text(" ");
  return ftxui::vbox({
      ftxui::hbox({ftxui::xflex_shrink(spacer), std::move(dialog),
                   ftxui::xflex_shrink(spacer)}) |
          ftxui::yflex,
      ftxui::yflex_shrink(spacer),
      ftxui::yflex_shrink(spacer),
      ftxui::yflex_shrink(spacer),
  });
}

UninstallKey handle_uninstall_key(Panel& panel, const std::string& key) {
  if (key == "y" || key == "Y") {
    panel.confirming_uninstall = false;
    return UninstallKey::kEaten;  // the caller runs the uninstall
  }
  if (key == "q") {
    // "q" keeps meaning quit; the dialog only lets it out.
    panel.confirming_uninstall = false;
    return UninstallKey::kQuit;
  }
  if (key == "n" || key == "N" || key == "\x1b") {
    panel.confirming_uninstall = false;
    return UninstallKey::kDismissed;
  }
  return UninstallKey::kEaten;  // anything else is eaten by the modal
}

}  // namespace tui