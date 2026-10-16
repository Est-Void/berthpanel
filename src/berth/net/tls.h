#pragma once

// Verify REQUIRED against the embedded roots; no system CA path is consulted.

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

  // Verifies against the embedded roots for `hostname`, never an address.
  [[nodiscard]] Result<void> handshake(
      const std::string& hostname,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>* cancel);

  [[nodiscard]] Result<void> write_all(
      std::string_view data, std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>* cancel);

  // 0 means TLS close-notify (EOF).
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

}
