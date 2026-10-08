// src/tui/view_logs.cpp — see view_logs.h for the contract.

#include "view_logs.h"

#include <array>
#include <string>
#include <utility>

#include "../util/strutil.h"

namespace tui {

namespace {

struct LogSpec {
  LogFile file;
  std::string_view label;
  std::string_view hint;
};

constexpr std::array<LogSpec, kLogFileCount> kLogs{{
    {LogFile::kDeploy, "deploy.log",
     "every deploy stage, sweep and failure (§6.7 reconciliation lives here)"},
    {LogFile::kNet, "net.log",
     "every outbound request: allowed or DENIED, one line each (§6.1)"},
    {LogFile::kCrash, "crash.log", "the crashes; an empty file is good news"},
}};

const LogSpec& spec_of(const LogFile file) {
  for (const LogSpec& spec : kLogs) {
    if (spec.file == file) {
      return spec;
    }
  }
  // A closed table with three entries; anything else is a bug in the caller.
  std::abort();
}

}  // namespace

std::string_view log_file_label(const LogFile file) {
  return spec_of(file).label;
}

std::string_view log_file_path_hint(const LogFile file) {
  return spec_of(file).hint;
}

LogFile log_file_at(const std::size_t index) {
  if (index >= kLogFileCount) {
    std::abort();
  }
  return kLogs[index].file;
}

std::size_t log_file_index(const LogFile file) {
  for (std::size_t i = 0; i < kLogs.size(); ++i) {
    if (kLogs[i].file == file) {
      return i;
    }
  }
  std::abort();
}

std::vector<std::string> log_empty_lines(const LogFile file) {
  std::vector<std::string> lines;
  switch (file) {
    case LogFile::kCrash:
      // The one good outcome, said as a good outcome.
      lines.push_back("crash.log is empty: nothing has crashed.");
      lines.push_back("That is the state you want it in. It is created on the");
      lines.push_back("first crash, so an absent file and an empty one mean");
      lines.push_back("the same thing here.");
      return lines;
    case LogFile::kNet:
      lines.push_back("net.log is empty: this session has made no outbound");
      lines.push_back("request. Every request berthpanel ever makes goes");
      lines.push_back("through the allowlist and lands here (§6.1).");
      lines.push_back("  berthpanel --show-endpoints   (the table)");
      lines.push_back("  berthpanel __egress-probe      (a live round trip)");
      return lines;
    case LogFile::kDeploy:
      lines.push_back("deploy.log is empty: nothing has been deployed from");
      lines.push_back("this panel yet. Every deploy stage, every sweep and");
      lines.push_back("every failure is appended here.");
      return lines;
  }
  return lines;
}

std::vector<std::string> log_error_lines(const LogFile file,
                                         const std::string& error) {
  std::vector<std::string> lines;
  lines.push_back(std::string(log_file_label(file)) + " could not be read:");
  for (const std::string& part : berth::split(error, "\n")) {
    lines.push_back("  " + part);
  }
  // A failed read is not an empty log, and the screen must not let the two look
  // alike (§11).
  lines.push_back(
      "this is NOT the same as an empty file: the contents are "
      "unknown right now.");
  return lines;
}

std::string net_banner(const berth::NetSnapshot& snapshot) {
  if (snapshot.total == 0) {
    return {};  // no row of zeroes: nothing has been asked for yet
  }
  std::string out = "NET " + std::to_string(snapshot.total) + " (" +
                    std::to_string(snapshot.denied) + " denied)";
  if (!snapshot.last_host.empty()) {
    out += "  last: " + snapshot.last_host;
  }
  return out;
}

std::string net_banner_note(const berth::NetSnapshot& snapshot) {
  if (snapshot.total == 0) {
    return "every outbound request passes an exact-match allowlist and lands "
           "in net.log (§6.1)";
  }
  std::string out = std::to_string(snapshot.allowed) + " allowed, " +
                    std::to_string(snapshot.denied) +
                    " denied — counted from the moment the request was made, "
                    "before the log line was written";
  if (snapshot.denied > 0) {
    // A refusal is a bug or a misconfiguration; it deserves a word of its own.
    out += "  — a DENIED line is in net.log with the reason";
  }
  return out;
}

std::string log_tabs_line(
    const LogFile selected,
    const std::vector<std::optional<std::size_t>>& counts) {
  std::string out;
  for (std::size_t i = 0; i < kLogs.size(); ++i) {
    if (i > 0) {
      out += "  ";
    }
    const bool active = kLogs[i].file == selected;
    out += active ? "\xe2\x96\xb6 " : "  ";
    out += kLogs[i].label;
    if (i < counts.size() && counts[i].has_value()) {
      out += " (" + std::to_string(counts[i].value()) + ")";
    }
    if (active) {
      out += " \xe2\x96\xb7";
    }
  }
  return out;
}

}  // namespace tui