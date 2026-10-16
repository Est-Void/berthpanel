#pragma once

// Embedded Mozilla CA bundle — there is no system CA path anywhere (§2).

#include <cstddef>

namespace berth {

extern const unsigned char kTrustStorePem[];
extern const std::size_t kTrustStorePemLen;

}
