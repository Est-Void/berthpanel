// src/core/netlog.cpp — see netlog.h for the contract.

#include "netlog.h"

#include <mutex>

#include <string>

#include "core/applog.h"
#include "core/netstatus.h"

namespace berth {

namespace {

std::mutex g_netlog_mutex;

std::string render_entry(const NetlogEntry& entry) {
  std::string line = log_timestamp();
  line += " ";
  line += one_line(entry.host);
  line += " ";
  line += one_line(entry.ip);
  line += " port=";
  line += std::to_string(entry.port);
  line += " purpose=";
  line += one_line(entry.purpose);
  line += entry.allowed ? " allowed" : " DENIED";
  if (!entry.note.empty()) {
    line += " note=\"";
    line += one_line(entry.note);
    line += "\"";
  }
  line += "\n";
  return line;
}

}  // namespace

Result<void> netlog_append(const Paths& paths, const NetlogEntry& entry) {
  // Counted BEFORE the write, so the NET banner can be ahead of the log but
  // never behind it (§6.1: the attempt happened whether or not we got to say
  // so). netstatus.cpp is the whole implementation — see netstatus.h.
  netstatus_record(entry.host, entry.allowed);
  const std::string line = render_entry(entry);
  // One mutex across both sinks: a line must never interleave with another.
  const std::lock_guard<std::mutex> guard(g_netlog_mutex);
  return append_log_line(paths.net_log, paths.state_dir, line);
}

}  // namespace berth
