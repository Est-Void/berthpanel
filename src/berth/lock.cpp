// src/berth/lock.cpp — see lock.h for the contract.

#include "lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>

#include "../util/errno.h"
#include "../util/fs.h"

namespace berth {

Result<DeployLock> DeployLock::acquire(const Paths& paths) {
  Result<void> dir = ensure_dir(paths.state_dir);
  if (dir.is_err())
    return std::move(dir).error();

  FdGuard fd(::open(paths.deploy_lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC,
                    S_IRUSR | S_IWUSR));
  if (fd.get() < 0) {
    return Error("deploy lock: open " + paths.deploy_lock.string() + ": " +
                 errno_text(errno));
  }
  if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      return Error("deploy lock: another deploy is already running");
    }
    return Error("deploy lock: flock: " + errno_text(errno));
  }
  return DeployLock(std::move(fd));
}

int lock_test_child(const char* lock_path) {
  FdGuard fd(
      ::open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR));
  if (fd.get() < 0)
    return 1;
  if (::flock(fd.get(), LOCK_EX) != 0)
    return 1;  // blocking: wait our turn
  ::pause();   // the parent SIGKILLs us; the kernel then drops the lock
  return 0;
}

}  // namespace berth
