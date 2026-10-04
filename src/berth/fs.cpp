#include "fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "../util/errno.h"
#include "../util/fd.h"

namespace berth {

namespace {

bool is_dot_entry(const char* name) {
  return name[0] == '.' &&
         (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

Result<std::size_t> remove_children(const int dirfd) {
  const int dup_fd = ::dup(dirfd);
  if (dup_fd < 0) {
    return Error("fs: dup: " + errno_text(errno));
  }
  DIR* dir = ::fdopendir(dup_fd);
  if (dir == nullptr) {
    const std::string why = errno_text(errno);
    ::close(dup_fd);
    return Error("fs: fdopendir: " + why);
  }
  struct DirCloser {
    DIR** slot;
    ~DirCloser() { ::closedir(*slot); }
  } closer{&dir};

  std::vector<std::string> names;
  while (const dirent* entry = ::readdir(dir)) {
    if (is_dot_entry(entry->d_name)) continue;
    names.emplace_back(entry->d_name);
  }

  std::size_t removed = 0;
  for (const std::string& name : names) {
    struct stat st {};
    if (::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;  // raced away: not ours to mourn
      return Error("fs: fstatat " + name + ": " + errno_text(errno));
    }
    if (S_ISDIR(st.st_mode)) {
      const int child_fd =
          ::openat(dirfd, name.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child_fd < 0) {
        if (errno == ENOENT) continue;
        if (errno == ELOOP) {
          // Not a real dir after all (race): fall through to unlink.
        } else {
          return Error("fs: openat " + name + ": " + errno_text(errno));
        }
      } else {
        const FdGuard child_guard(child_fd);
        Result<std::size_t> nested = remove_children(child_fd);
        if (nested.is_err()) return nested;
        removed += nested.value();
      }
      if (::unlinkat(dirfd, name.c_str(), AT_REMOVEDIR) != 0) {
        if (errno == ENOENT) continue;
        return Error("fs: rmdir " + name + ": " + errno_text(errno));
      }
      ++removed;
      continue;
    }
    if (::unlinkat(dirfd, name.c_str(), 0) != 0) {
      if (errno == ENOENT) continue;
      return Error("fs: unlink " + name + ": " + errno_text(errno));
    }
    ++removed;
  }
  return removed;
}

}  // namespace

Result<std::size_t> remove_tree(const std::filesystem::path& root) {
  if (root.empty()) return Error("fs: remove_tree: empty path");
  if (!root.is_absolute()) {
    return Error("fs: remove_tree: refusing relative path: " + root.string());
  }
  if (root == "/" || root.lexically_normal() == "/") {
    return Error("fs: remove_tree: refusing to remove /");
  }

  struct stat st {};
  if (::lstat(root.c_str(), &st) != 0) {
    if (errno == ENOENT) return std::size_t{0};
    return Error("fs: lstat " + root.string() + ": " + errno_text(errno));
  }
  // A symlinked root is unlinked, never followed (it may point anywhere).
  if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
    if (::unlinkat(AT_FDCWD, root.c_str(), 0) != 0) {
      if (errno == ENOENT) return std::size_t{0};
      return Error("fs: unlink " + root.string() + ": " + errno_text(errno));
    }
    return 1;
  }

  const int fd =
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return std::size_t{0};
    return Error("fs: open " + root.string() + ": " + errno_text(errno));
  }
  {
    const FdGuard guard(fd);
    Result<std::size_t> children = remove_children(fd);
    if (children.is_err()) return children;
    if (::unlinkat(AT_FDCWD, root.c_str(), AT_REMOVEDIR) != 0) {
      if (errno == ENOENT) return children.value();
      return Error("fs: rmdir " + root.string() + ": " + errno_text(errno));
    }
    return children.value() + 1;
  }
}

}  // namespace berth
