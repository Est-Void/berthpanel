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

}
Result<void> netlog_append(const Paths& paths, const NetlogEntry& entry) {
  // Counted before the write: the NET banner may be ahead of the log, never
  // behind it.
  netstatus_record(entry.host, entry.allowed);
  const std::string line = render_entry(entry);
  const std::lock_guard<std::mutex> guard(g_netlog_mutex);
  return append_log_line(paths.net_log, paths.state_dir, line);
}

}
