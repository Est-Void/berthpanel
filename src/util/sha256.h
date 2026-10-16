#pragma once

#include <cstddef>
#include <string>
#include <string_view>

extern "C" {
#include "sha2/sha256.h"
}

namespace berth {

// finish_hex() consumes the hasher; a second call is a bug.
class Sha256 {
 public:
  Sha256() { sha256_init(&ctx_); }

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  Sha256(Sha256&&) = delete;
  Sha256& operator=(Sha256&&) = delete;

  void update(std::string_view data) {
    if (data.empty())
      return;
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

}
