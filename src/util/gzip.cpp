#include "gzip.h"

#include <zlib.h>

#include <string>
#include <vector>

namespace berth {

Result<std::string> gunzip(const std::string_view compressed,
                           const std::size_t max_out) {
  if (compressed.empty()) return Error("gzip: empty input");

  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());

  // 16 + MAX_WBITS: gzip wrapper. The 32 variant would also accept zlib
  // streams; we want gzip only, so a wrong container is a loud error.
  const int init = ::inflateInit2(&stream, 16 + MAX_WBITS);
  if (init != Z_OK) return Error("gzip: inflateInit2 failed: " +
                                 std::to_string(init));

  std::string out;
  std::vector<char> chunk(64 * 1024);
  int status = Z_OK;
  while (true) {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    status = ::inflate(&stream, Z_NO_FLUSH);
    const std::size_t produced = chunk.size() - stream.avail_out;
    if (produced > 0) {
      if (out.size() + produced > max_out) {
        ::inflateEnd(&stream);
        return Error("gzip: output exceeds the " + std::to_string(max_out) +
                     " byte cap");
      }
      out.append(chunk.data(), produced);
    }
    if (status == Z_STREAM_END) break;
    if (status != Z_OK) {
      const std::string why = stream.msg != nullptr ? stream.msg : "corrupt";
      ::inflateEnd(&stream);
      return Error("gzip: inflate failed: " + why);
    }
    if (stream.avail_in == 0 && produced == 0) {
      ::inflateEnd(&stream);
      return Error("gzip: truncated stream");
    }
  }
  if (stream.avail_in > 0) {
    ::inflateEnd(&stream);
    return Error("gzip: trailing data after the final block");
  }
  ::inflateEnd(&stream);
  return out;
}

}  // namespace berth