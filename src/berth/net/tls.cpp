// src/berth/net/tls.cpp — see tls.h for the contract.

#include "tls.h"

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <poll.h>
#include <psa/crypto.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "../../core/trust_store.h"
#include "socket.h"

namespace berth {

namespace {

// TLS 1.3 in mbedTLS 3.6 routes through PSA, which must be initialised once
// per process. Failure here is loud: no TLS, no request.
std::once_flag g_psa_init;

void ensure_psa_initialised() {
  std::call_once(g_psa_init, [] {
    if (::psa_crypto_init() != PSA_SUCCESS) {
      std::fputs("berth: fatal: psa_crypto_init failed\n", stderr);
      std::abort();
    }
  });
}

}  // namespace

TlsSession::TlsSession(const int fd) : fd_(fd) {
  mbedtls_entropy_init(&entropy_);
  mbedtls_ctr_drbg_init(&drbg_);
  mbedtls_x509_crt_init(&cas_);
  mbedtls_ssl_config_init(&conf_);
  mbedtls_ssl_init(&ssl_);
}

TlsSession::~TlsSession() {
  mbedtls_ssl_free(&ssl_);
  mbedtls_ssl_config_free(&conf_);
  mbedtls_x509_crt_free(&cas_);
  mbedtls_ctr_drbg_free(&drbg_);
  mbedtls_entropy_free(&entropy_);
}

Error TlsSession::tls_error(const std::string& what, const int rc) {
  char buf[256];
  mbedtls_strerror(rc, buf, sizeof(buf));
  return Error("tls: " + what + ": " + buf);
}

int TlsSession::bio_send(void* ctx, const unsigned char* buf,
                         const std::size_t len) {
  auto* self = static_cast<TlsSession*>(ctx);
  const ssize_t n = ::send(self->fd_, buf, len, MSG_NOSIGNAL);
  if (n >= 0)
    return static_cast<int>(n);
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  }
  return MBEDTLS_ERR_NET_SEND_FAILED;
}

int TlsSession::bio_recv(void* ctx, unsigned char* buf, const std::size_t len) {
  auto* self = static_cast<TlsSession*>(ctx);
  const ssize_t n = ::recv(self->fd_, buf, len, 0);
  if (n >= 0)
    return static_cast<int>(n);
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  return MBEDTLS_ERR_NET_RECV_FAILED;
}

Result<void> TlsSession::handshake(
    const std::string& hostname,
    const std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel) {
  ensure_psa_initialised();

  int rc = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                 nullptr, 0);
  if (rc != 0)
    return tls_error("ctr_drbg_seed", rc);
  rc = mbedtls_x509_crt_parse(&cas_, kTrustStorePem, kTrustStorePemLen);
  if (rc != 0)
    return tls_error("trust store parse", rc);
  rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0)
    return tls_error("config defaults", rc);

  mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conf_, &cas_, nullptr);
  mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
  rc = mbedtls_ssl_setup(&ssl_, &conf_);
  if (rc != 0)
    return tls_error("ssl_setup", rc);
  rc = mbedtls_ssl_set_hostname(&ssl_, hostname.c_str());
  if (rc != 0)
    return tls_error("set_hostname", rc);
  mbedtls_ssl_set_bio(&ssl_, this, bio_send, bio_recv, nullptr);

  while (true) {
    rc = mbedtls_ssl_handshake(&ssl_);
    if (rc == 0)
      return {};
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      return tls_error("handshake " + hostname, rc);
    }
    const short want = rc == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
    const Result<void> waited =
        wait_socket(fd_, want, deadline, cancel, "tls handshake");
    if (waited.is_err())
      return std::move(waited).error();
  }
}

Result<void> TlsSession::write_all(
    const std::string_view data,
    const std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int rc = mbedtls_ssl_write(
        &ssl_, reinterpret_cast<const unsigned char*>(data.data() + sent),
        data.size() - sent);
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) {
      const Result<void> waited =
          wait_socket(fd_, POLLIN | POLLOUT, deadline, cancel, "tls write");
      if (waited.is_err())
        return std::move(waited).error();
      continue;
    }
    if (rc <= 0)
      return tls_error("ssl_write", rc);
    sent += static_cast<std::size_t>(rc);
  }
  return {};
}

Result<std::size_t> TlsSession::read_some(
    unsigned char* buf, const std::size_t buf_size,
    const std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel) {
  while (true) {
    const int rc = mbedtls_ssl_read(&ssl_, buf, buf_size);
    // mbedtls_ssl_read may need to WRITE during a read (post-handshake TLS
    // 1.3 traffic, key updates). Treating WANT_WRITE as fatal aborted an
    // otherwise-valid connection; the handshake loop above and write_all()
    // both already wait on the direction mbedTLS asked for.
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      const short want = rc == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
      const Result<void> waited =
          wait_socket(fd_, want, deadline, cancel, "tls read");
      if (waited.is_err())
        return std::move(waited).error();
      continue;
    }
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
      return std::size_t{0};
    }
    if (rc < 0)
      return tls_error("ssl_read", rc);
    return static_cast<std::size_t>(rc);
  }
}

}  // namespace berth
