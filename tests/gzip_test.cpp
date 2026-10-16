#include "util/gzip.h"

#include <string>

#include "runner.h"

using berth::gunzip;

namespace {

// A real gzip stream (produced by gzip, mtime zeroed) so the test checks
// container compatibility, not a round-trip through the same library.
const unsigned char kRealGzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff,
    0x2b, 0x4e, 0x4d, 0x4d, 0x51, 0x48, 0xcb, 0x2f, 0x52, 0x28,
    0xc9, 0x48, 0x55, 0x48, 0xaf, 0xca, 0x2c, 0x50, 0x28, 0xce,
    0xcd, 0xcf, 0x4e, 0x55, 0x28, 0x49, 0x2d, 0x2e, 0xe1, 0x02,
    0x00, 0x1e, 0xa5, 0xfd, 0xda, 0x1d, 0x00, 0x00, 0x00};

std::string_view seed() {
  return std::string_view(reinterpret_cast<const char*>(kRealGzip),
                          sizeof(kRealGzip));
}

}
TEST(gzip_inflates_a_real_stream) {
  const auto out = gunzip(seed(), 1024);
  CHECK(out.is_ok());
  CHECK(out.value() == "seed for the gzip smoke test\n");
}

TEST(gzip_rejects_garbage_and_empty_input) {
  CHECK(gunzip("", 1024).is_err());
  CHECK(gunzip("not gzip at all", 1024).is_err());
  // Raw zlib container, not gzip: we want a loud error, not silent luck.
  const std::string zlib_stream("\x78\x9c\x2b\x2e\x2d\x00\x00\x00\x2e", 9);
  CHECK(gunzip(zlib_stream, 1024).is_err());
}

TEST(gzip_rejects_truncated_stream) {
  const std::string_view truncated = seed().substr(0, seed().size() - 6);
  CHECK(gunzip(truncated, 1024).is_err());
}

TEST(gzip_enforces_the_output_cap) {
  CHECK(gunzip(seed(), 4).is_err());
  const auto out = gunzip(seed(), 32);
  CHECK(out.is_ok());
  CHECK(out.value().size() == 29);
}

TEST(gzip_rejects_trailing_garbage) {
  std::string with_tail(seed());
  with_tail += "junk";
  CHECK(gunzip(with_tail, 1024).is_err());
}
