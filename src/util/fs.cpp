#include "util/fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "util/errno.h"
#include "util/fd.h"

namespace berth {

namespace {

bool is_dot_entry(const char* name) {
  return name[0] == '.' &&
         (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// ENOENT means a concurrent process got there first, which is not this
// walk's failure.
bool vanished(const int saved_errno) {
  return saved_errno == ENOENT;
}

std::string dot_error(const char* op, const std::string& name, const int e) {
  return std::string("fs: ") + op + " " + name + ": " + errno_text(e);
}

Result<std::size_t> remove_children(const int dirfd);

Result<std::size_t> remove_directory(const int dirfd, const std::string& name) {
  const int child_fd = ::openat(
      dirfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  // ELOOP: the entry stopped being a directory between the stat and the open.
  // The plain rmdir below then fails loudly if it is not ours.
  if (child_fd >= 0) {
    const FdGuard guard(child_fd);
    const Result<std::size_t> nested = remove_children(child_fd);
    if (nested.is_err())
      return nested;
    if (::unlinkat(dirfd, name.c_str(), AT_REMOVEDIR) != 0) {
      if (vanished(errno))
        return nested;
      return Error(dot_error("rmdir", name, errno));
    }
    return nested.value() + 1;
  }
  if (::unlinkat(dirfd, name.c_str(), AT_REMOVEDIR) != 0) {
    if (vanished(errno))
      return std::size_t{0};
    return Error(dot_error("rmdir", name, errno));
  }
  return 1;
}

Result<std::size_t> remove_entry(const int dirfd, const std::string& name) {
  struct stat st{};
  if (::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
    if (vanished(errno))
      return std::size_t{0};
    return Error(dot_error("fstatat", name, errno));
  }
  if (S_ISDIR(st.st_mode))
    return remove_directory(dirfd, name);
  if (::unlinkat(dirfd, name.c_str(), 0) != 0) {
    if (vanished(errno))
      return std::size_t{0};
    return Error(dot_error("unlink", name, errno));
  }
  return 1;
}

// readdir into a vector first: removing entries while a directory stream is
// open is undefined behaviour.
Result<std::vector<std::string>> list_names(const int dirfd) {
  const int dup_fd = ::dup(dirfd);
  if (dup_fd < 0)
    return Error(dot_error("dup", "", errno));
  DIR* dir = ::fdopendir(dup_fd);
  if (dir == nullptr) {
    const int saved = errno;
    ::close(dup_fd);
    return Error(dot_error("fdopendir", "", saved));
  }
  struct DirCloser {
    DIR** slot;
    ~DirCloser() { ::closedir(*slot); }
  } closer{&dir};

  std::vector<std::string> names;
  while (const dirent* entry = ::readdir(dir)) {
    if (is_dot_entry(entry->d_name))
      continue;
    names.emplace_back(entry->d_name);
  }
  return names;
}

Result<std::size_t> remove_children(const int dirfd) {
  const Result<std::vector<std::string>> listed = list_names(dirfd);
  if (listed.is_err())
    return std::move(listed).error();

  std::size_t removed = 0;
  for (const std::string& name : listed.value()) {
    const Result<std::size_t> count = remove_entry(dirfd, name);
    if (count.is_err())
      return count;
    removed += count.value();
  }
  return removed;
}

Result<std::size_t> unlink_root(const std::filesystem::path& root) {
  if (::unlinkat(AT_FDCWD, root.c_str(), 0) != 0) {
    if (vanished(errno))
      return std::size_t{0};
    return Error(dot_error("unlink", root.string(), errno));
  }
  return 1;
}

}
Result<void> ensure_dir(const std::filesystem::path& dir,
                        std::filesystem::perms perms) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return Error("ensure_dir: create " + dir.string() + ": " + ec.message());
  }
  if (::chmod(dir.c_str(), static_cast<mode_t>(perms)) != 0) {
    return Error("ensure_dir: chmod " + dir.string() + ": " +
                 errno_text(errno));
  }
  return {};
}

Result<std::string> read_file(const std::filesystem::path& path) {
  const FdGuard fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    return Error("read_file: open " + path.string() + ": " + errno_text(errno));
  }
  std::string out;
  char buf[65536];
  while (true) {
    const ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return Error("read_file: read " + path.string() + ": " +
                   errno_text(errno));
    }
    if (n == 0)
      return out;
    out.append(buf, static_cast<std::size_t>(n));
  }
}

Result<std::size_t> remove_tree(const std::filesystem::path& root) {
  if (root.empty())
    return Error("fs: remove_tree: empty path");
  if (!root.is_absolute()) {
    return Error("fs: remove_tree: refusing relative path: " + root.string());
  }
  if (root == "/" || root.lexically_normal() == "/") {
    return Error("fs: remove_tree: refusing to remove /");
  }

  struct stat st{};
  if (::lstat(root.c_str(), &st) != 0) {
    if (vanished(errno))
      return std::size_t{0};
    return Error(dot_error("lstat", root.string(), errno));
  }
  // A symlinked root is unlinked, never followed: it may point anywhere.
  if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
    return unlink_root(root);

  const int fd =
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (vanished(errno))
      return std::size_t{0};
    return Error(dot_error("open", root.string(), errno));
  }
  const FdGuard guard(fd);
  const Result<std::size_t> children = remove_children(fd);
  if (children.is_err())
    return children;
  if (::unlinkat(AT_FDCWD, root.c_str(), AT_REMOVEDIR) != 0) {
    if (vanished(errno))
      return children;
    return Error(dot_error("rmdir", root.string(), errno));
  }
  return children.value() + 1;
}

}
