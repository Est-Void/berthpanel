// Tests for util/sha256.h (stage 1.4): FIPS 180 known vectors, streaming
// equivalence with the one-shot helper, output shape.

#include "util/sha256.h"

#include <string>

#include "runner.h"

using berth::Sha256;
using berth::sha256_hex;

TEST(sha256_known_vectors) {
  CHECK(sha256_hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex("The quick brown fox jumps over the lazy dog") ==
        "d7a8fbb307d7809469ca9abcb0082e4f8"
        "d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(sha256_million_a_vector) {
  const std::string million(1000000, 'a');
  CHECK(sha256_hex(million) ==
        "cdc76e5c9914fb9281a1c7e284d73e67"
        "f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_streaming_matches_one_shot) {
  Sha256 hasher;
  hasher.update("ab");
  hasher.update("");
  hasher.update("c");
  CHECK(hasher.finish_hex() == sha256_hex("abc"));
}

TEST(sha256_hex_is_64_lowercase) {
  const std::string hex = sha256_hex("x");
  CHECK(hex.size() == 64);
  for (const char c : hex) {
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}
