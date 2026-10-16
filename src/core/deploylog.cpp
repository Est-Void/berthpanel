#include "deploylog.h"

#include <mutex>

#include <string>

namespace berth {

namespace {

std::mutex g_deploylog_mutex;

std::string render_line(const std::string_view line) {
  std::string text = log_timestamp();
  text += " ";
  text += one_line(line);
  text += "\n";
  return text;
}

}
Result<void> deploylog_append(const Paths& paths, const std::string_view line) {
  const std::string text = render_line(line);
  const std::lock_guard<std::mutex> guard(g_deploylog_mutex);
  return append_log_line(paths.deploy_log, paths.state_dir, text);
}

}
