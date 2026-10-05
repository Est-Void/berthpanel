#include "berth/goenv.h"

#include <algorithm>
#include <string>
#include <vector>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"
#include "util/sha256.h"

using berth::append_segment;
using berth::current_segment_size;
using berth::escape_module_path;
using berth::finalize_segmented;
using berth::ModuleFiles;
using berth::range_header_value;
using berth::write_module_cache_entry;

TEST(goenv_module_path_escaping) {
  CHECK(escape_module_path("github.com/Masterminds/semver").value() ==
        "github.com/!masterminds/semver");
  CHECK(escape_module_path("gopkg.in/yaml.v3").value() == "gopkg.in/yaml.v3");
  CHECK(escape_module_path("github.com/Foo/Bar").value() ==
        "github.com/!foo/!bar");
  CHECK(escape_module_path("").is_err());
  CHECK(escape_module_path("bad path/x").is_err());
  CHECK(escape_module_path("has:colon/x").is_err());
}

TEST(goenv_range_header_and_segments) {
  CHECK(range_header_value(0, 100).value() == "bytes=0-");
  CHECK(range_header_value(40, 100).value() == "bytes=40-");
  CHECK(range_header_value(100, 100).is_err());

  const TempDir tmp;
  const auto partial = tmp.path() / "partial.bin";
  CHECK(current_segment_size(partial).value() == 0);
  CHECK(append_segment(partial, "abc").value() == 3);
  CHECK(append_segment(partial, "def").value() == 6);
  CHECK(current_segment_size(partial).value() == 6);
  const auto body = berth::read_file(partial);
  CHECK(body.is_ok());
  CHECK(body.value() == "abcdef");
}

TEST(goenv_finalize_checks_size_then_hash) {
  const TempDir tmp;
  const auto partial = tmp.path() / "tool.part";
  const auto dest = tmp.path() / "tool.bin";
  const std::string payload = "go toolchain bytes";
  CHECK(append_segment(partial, payload).is_ok());
  const std::string sha = berth::sha256_hex(payload);

  const auto bad_size = finalize_segmented(partial, dest, sha, 999);
  CHECK(bad_size.is_err());
  CHECK(!std::filesystem::exists(dest));

  const auto bad_hash =
      finalize_segmented(partial, dest, berth::sha256_hex("other"), 0);
  CHECK(bad_hash.is_err());
  CHECK(!std::filesystem::exists(dest));

  const auto ok = finalize_segmented(partial, dest, sha, payload.size());
  CHECK(ok.is_ok());
  CHECK(std::filesystem::exists(dest));
  CHECK(!std::filesystem::exists(partial));
}

TEST(goenv_module_cache_entry_layout) {
  const TempDir tmp;
  const auto cache = tmp.path() / "modcache";
  const ModuleFiles files{.info = R"({"Version":"v1.2.3"})",
                          .mod = "module example.com/x\n",
                          .zip = "PK\x03\x04zip"};

  const auto dir =
      write_module_cache_entry(cache, "example.com/X", "v1.2.3", files);
  CHECK(dir.is_ok());
  const std::filesystem::path expected =
      cache / "cache" / "download" / "example.com/!x" / "@v";
  CHECK(dir.value() == expected);
  CHECK(berth::read_file(expected / "v1.2.3.info").value() == files.info);
  CHECK(berth::read_file(expected / "v1.2.3.mod").value() == files.mod);
  CHECK(berth::read_file(expected / "v1.2.3.zip").value() == files.zip);
  CHECK(write_module_cache_entry(cache, "example.com/X", "bad/version", files)
            .is_err());
}

TEST(goenv_build_env_is_offline_and_pinned) {
  const std::vector<std::string> env =
      berth::go_build_env("/tools/go", "/cache/mod", 2);
  const auto has = [&env](const std::string& needle) {
    return std::find_if(env.begin(), env.end(), [&](const std::string& e) {
             return e.rfind(needle, 0) == 0;
           }) != env.end();
  };
  CHECK(has("GOROOT=/tools/go"));
  CHECK(has("GOMODCACHE=/cache/mod"));
  CHECK(has("GOTOOLCHAIN=local"));
  CHECK(has("GOSUMDB=off"));
  CHECK(has("GOFLAGS=-mod=mod"));
  bool proxy_is_file = false;
  for (const std::string& e : env) {
    if (e.rfind("GOPROXY=", 0) == 0)
      proxy_is_file = e.find("file://") == 8;
  }
  CHECK(proxy_is_file);
}