// src/berth/status.cpp — see status.h for the contract.

#include "status.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "core/jsonout.h"

namespace berth {

namespace {

std::string html_escape(const std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

std::string short_pin(const std::string& commit) {
  return commit.size() > 7 ? commit.substr(0, 7) : commit;
}

bool wants_json(const HttpRequest& request) {
  if (request.path == "/status.json")
    return true;
  if (request.query.find("format=json") != std::string::npos)
    return true;
  const std::string* accept = request.header("accept");
  return accept != nullptr &&
         accept->find("application/json") != std::string::npos;
}

std::vector<StatusService> sorted_services(
    const std::vector<StatusService>& services) {
  std::vector<StatusService> out = services;
  std::sort(out.begin(), out.end(),
            [](const StatusService& a, const StatusService& b) {
              return a.service_name < b.service_name;
            });
  return out;
}

std::vector<StatusPort> sorted_ports(const std::vector<StatusPort>& ports) {
  std::vector<StatusPort> out = ports;
  std::sort(out.begin(), out.end(),
            [](const StatusPort& a, const StatusPort& b) {
              if (a.address != b.address)
                return a.address < b.address;
              return a.port < b.port;
            });
  return out;
}

std::vector<StatusDeploy> sorted_deploys(
    const std::vector<StatusDeploy>& deploys) {
  std::vector<StatusDeploy> out = deploys;
  std::sort(out.begin(), out.end(),
            [](const StatusDeploy& a, const StatusDeploy& b) {
              return a.service_name < b.service_name;
            });
  return out;
}

}  // namespace

nlohmann::json status_json(const StatusSnapshot& snapshot) {
  nlohmann::json doc = json_document("serve-status");
  if (!snapshot.error.empty())
    doc["error"] = snapshot.error;

  nlohmann::json services = nlohmann::json::array();
  for (const StatusService& service : sorted_services(snapshot.services)) {
    services.push_back({{"service_name", service.service_name},
                        {"display_name", service.display_name},
                        {"description", service.description},
                        {"repo", service.repo},
                        {"commit", service.commit},
                        {"port", service.port}});
  }
  doc["services"] = std::move(services);

  nlohmann::json ports = nlohmann::json::array();
  for (const StatusPort& port : sorted_ports(snapshot.ports)) {
    ports.push_back({{"address", port.address},
                     {"port", port.port},
                     {"loopback", port.loopback},
                     {"wildcard", port.wildcard},
                     {"owned", port.owned}});
  }
  doc["ports"] = std::move(ports);

  nlohmann::json deploys = nlohmann::json::array();
  for (const StatusDeploy& deploy : sorted_deploys(snapshot.open_deploys)) {
    deploys.push_back({{"service_name", deploy.service_name},
                       {"stage", deploy.stage},
                       {"started_at_unix", deploy.started_at_unix}});
  }
  doc["open_deploys"] = std::move(deploys);
  return doc;
}

std::string render_status_html(const StatusSnapshot& snapshot) {
  std::string out;
  out += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n";
  out += "<title>berthpanel status</title>\n";
  out +=
      "<style>body{font-family:ui-monospace,monospace;margin:2rem;"
      "max-width:60rem}table{border-collapse:collapse;width:100%}"
      "td,th{border:1px solid #ccc;padding:.25rem .5rem;text-align:left}"
      ".owned{color:#0a0}.wild{color:#b00}</style></head><body>\n";
  out += "<h1>berthpanel</h1>\n";
  out += "<p>read-only; this endpoint is loopback only.</p>\n";

  if (!snapshot.error.empty()) {
    out += "<p class=\"wild\">error: " + html_escape(snapshot.error) + "</p>\n";
  }

  out += "<h2>Services</h2>\n";
  if (snapshot.services.empty()) {
    out += "<p>no services registered</p>\n";
  } else {
    out +=
        "<table><tr><th>service</th><th>display</th><th>pin</th>"
        "<th>port</th></tr>\n";
    for (const StatusService& service : sorted_services(snapshot.services)) {
      out += "<tr><td>" + html_escape(service.service_name) + "</td><td>" +
             html_escape(service.display_name) + "</td><td>" +
             html_escape(short_pin(service.commit)) + "</td><td>" +
             std::to_string(service.port) + "</td></tr>\n";
    }
    out += "</table>\n";
  }

  out += "<h2>Listening ports</h2>\n";
  if (snapshot.ports.empty()) {
    out += "<p>no listening ports reported</p>\n";
  } else {
    out += "<table><tr><th>address</th><th>port</th><th></th></tr>\n";
    for (const StatusPort& port : sorted_ports(snapshot.ports)) {
      out += "<tr><td>" + html_escape(port.address) + "</td><td>" +
             std::to_string(port.port) + "</td><td>";
      if (port.owned)
        out += "<span class=\"owned\">berthpanel</span>";
      else if (port.wildcard)
        out += "<span class=\"wild\">wildcard</span>";
      out += "</td></tr>\n";
    }
    out += "</table>\n";
  }

  out += "<h2>Open deploys</h2>\n";
  if (snapshot.open_deploys.empty()) {
    out += "<p>no deploy journal entries</p>\n";
  } else {
    out += "<table><tr><th>service</th><th>stage</th><th>started</th></tr>\n";
    for (const StatusDeploy& deploy : sorted_deploys(snapshot.open_deploys)) {
      out += "<tr><td>" + html_escape(deploy.service_name) + "</td><td>" +
             html_escape(deploy.stage) + "</td><td>" +
             std::to_string(deploy.started_at_unix) + "</td></tr>\n";
    }
    out += "</table>\n";
  }

  out += "</body></html>\n";
  return out;
}

RequestHandler make_status_handler(std::function<StatusSnapshot()> provider) {
  return [provider = std::move(provider)](const HttpRequest& request) {
    const StatusSnapshot snapshot = provider ? provider() : StatusSnapshot{};
    if (wants_json(request)) {
      HttpResponse reply;
      reply.content_type = "application/json; charset=utf-8";
      reply.body = json_text(status_json(snapshot));
      return reply;
    }
    if (request.path == "/" || request.path == "/status") {
      HttpResponse reply;
      reply.content_type = "text/html; charset=utf-8";
      reply.body = render_status_html(snapshot);
      return reply;
    }
    return plain_reply(404, "not found\n");
  };
}

}  // namespace berth
