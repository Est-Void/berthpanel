// Smoke test for the vendored mbedTLS and the embedded trust store
// (stage 3.2). Offline: verifies the CA bundle parses into the X.509
// stack and a client TLS context seeds and sets up — the whole vendored
// integration links and runs. Live TLS is exercised by __egress-probe
// (stage 3.6, manual, real network).

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <string>

#include "core/trust_store.h"
#include "runner.h"

TEST(mbedtls_trust_store_parses_into_x509) {
  mbedtls_x509_crt cas;
  mbedtls_x509_crt_init(&cas);
  const int ret = mbedtls_x509_crt_parse(&cas, berth::kTrustStorePem,
                                         berth::kTrustStorePemLen);
  CHECK(ret == 0);
  std::size_t count = 0;
  for (const mbedtls_x509_crt* it = &cas; it != nullptr; it = it->next) {
    ++count;
  }
  CHECK(count > 100);  // the Mozilla bundle ships ~150 roots
  mbedtls_x509_crt_free(&cas);
}

TEST(mbedtls_client_context_seeds_and_sets_up) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt cas;

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);
  mbedtls_x509_crt_init(&cas);

  const int parsed = mbedtls_x509_crt_parse(&cas, berth::kTrustStorePem,
                                            berth::kTrustStorePemLen);
  if (parsed != 0) {
    throw std::runtime_error("x509 parse ret=" + std::to_string(parsed));
  }
  CHECK(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr,
                              0) == 0);
  CHECK(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) == 0);
  mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
  mbedtls_ssl_conf_ca_chain(&conf, &cas, nullptr);
  mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  CHECK(mbedtls_ssl_setup(&ssl, &conf) == 0);
  CHECK(mbedtls_ssl_set_hostname(&ssl, "github.com") == 0);

  mbedtls_x509_crt_free(&cas);
  mbedtls_ssl_config_free(&conf);
  mbedtls_ssl_free(&ssl);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
}
