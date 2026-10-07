#pragma once

// src/berth/net/tls.h — one mbedTLS client session over one connected
// socket. Owns the handshake, the writes and the reads, and nothing else: no
// retry policy, no URL, no logging. The trust store is the EMBEDDED Mozilla
// root set (core/trust_store.h); no system CA path is ever consulted (dev.md
// §2), so a tampered /etc/ssl cannot widen what this connects to.
//
// Nothing is global except a single psa_crypto_init, the one process-wide
// mbedTLS requirement. Sessions never share state.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "util/result.h"

namespace berth {

class TlsSession {
 public:
  explicit TlsSession(int fd);
  TlsSession(const TlsSession&) = delete;
  TlsSession& operator=(const TlsSession&) = delete;
  ~TlsSession();

  // Verifies the peer against the embedded roots for `hostname` — the
  // CONNECTED name, never a resolved address.
  [[nodiscard]] Result<void> handshake(
      const std::string& hostname,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>* cancel);

  [[nodiscard]] Result<void> write_all(
      std::string_view data, std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>* cancel);

  // Reads up to `buf_size` bytes; 0 means TLS close-notify (EOF).
  [[nodiscard]] Result<std::size_t> read_some(
      unsigned char* buf, std::size_t buf_size,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>* cancel);

 private:
  static int bio_send(void* ctx, const unsigned char* buf, std::size_t len);
  static int bio_recv(void* ctx, unsigned char* buf, std::size_t len);
  [[nodiscard]] static Error tls_error(const std::string& what, int rc);

  int fd_;
  mbedtls_entropy_context entropy_;
  mbedtls_ctr_drbg_context drbg_;
  mbedtls_x509_crt cas_;
  mbedtls_ssl_config conf_;
  mbedtls_ssl_context ssl_;
};

}  // namespace berth
