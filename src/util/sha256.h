#pragma once

// src/util/sha256.h — SHA-256 over the vendored public-domain
// implementation in third_party/sha2/. Exposes a streaming hasher and a
// one-shot hex helper for content pins (toolbox downloads, catalog
// plugins). Owns no I/O; callers feed bytes and consume hex strings.

#include <cstddef>
#include <string>
#include <string_view>

extern "C" {
#include "sha2/sha256.h"
}

namespace berth {

// Streaming SHA-256. finish_hex() consumes the hasher; a second call is a
// bug. Empty updates are no-ops, matching the one-shot helper.
class Sha256 {
 public:
  Sha256() { sha256_init(&ctx_); }

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  Sha256(Sha256&&) = delete;
  Sha256& operator=(Sha256&&) = delete;

  void update(std::string_view data) {
    if (data.empty()) return;
    sha256_update(&ctx_, reinterpret_cast<const BYTE*>(data.data()),
                  data.size());
  }

  [[nodiscard]] std::string finish_hex() {
    BYTE hash[SHA256_BLOCK_SIZE];
    sha256_final(&ctx_, hash);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_BLOCK_SIZE * 2);
    for (const BYTE b : hash) {
      out.push_back(kHex[(b >> 4) & 0x0F]);
      out.push_back(kHex[b & 0x0F]);
    }
    return out;
  }

 private:
  SHA256_CTX ctx_{};
};

[[nodiscard]] inline std::string sha256_hex(std::string_view data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish_hex();
}

}  // namespace berth
