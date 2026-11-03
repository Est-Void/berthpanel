#include "buildenv.h"

namespace berth {

std::vector<std::string> build_environment(
    const std::filesystem::path& work_dir) {
  const std::filesystem::path root = work_dir / ".berth-build";
  return {
      "HOME=" + (root / "home").string(),
      "XDG_CACHE_HOME=" + (root / "cache").string(),
      "XDG_CONFIG_HOME=" + (root / "config").string(),
      "GOCACHE=" + (root / "cache" / "go-build").string(),
      "GOPATH=" + (root / "gopath").string(),
      "GOMODCACHE=" + (root / "gopath" / "pkg" / "mod").string(),
      "CARGO_HOME=" + (root / "cargo").string(),
      "NPM_CONFIG_CACHE=" + (root / "cache" / "npm").string(),
      "NPM_CONFIG_STORE_DIR=" + (root / "cache" / "pnpm-store").string(),
  };
}

}
