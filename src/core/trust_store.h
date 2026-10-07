#pragma once

// src/trust_store.h — the embedded Mozilla CA bundle (§2: no system CA
// path anywhere). The definition is generated at build time into
// $(BUILD)/generated/trust_store.cpp from the pinned PEM in
// third_party/trust/; see that VERSION.txt for the pin.

#include <cstddef>

namespace berth {

extern const unsigned char kTrustStorePem[];
extern const std::size_t kTrustStorePemLen;

}  // namespace berth
