#include "util/tmpdir.h"

#include <stdlib.h>

#include <string>

#include "util/fs.h"

namespace berth {

TempDir::~TempDir() {
  if (path_.empty())
    return;
  // Best effort by construction: a destructor cannot report, and a leftover
  // /tmp entry must never mask the error that led here.
  static_cast<void>(remove_tree(path_));
}

Result<TempDir> TempDir::create(const std::string_view prefix) {
  std::string tmpl = "/tmp/berthpanel-" + std::string(prefix) + "-XXXXXX";
  const char* made = ::mkdtemp(tmpl.data());
  if (made == nullptr) {
    return Error("tmpdir: mkdtemp " + tmpl + " failed");
  }
  const std::filesystem::path dir(made);
  const Result<void> chmod = ensure_dir(dir, kPermPrivateDir);
  if (chmod.is_err())
    return std::move(chmod).error();
  return TempDir(dir);
}

}
