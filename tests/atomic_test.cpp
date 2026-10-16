// Tests for atomic.h (stage 2.2): 0600-by-default perms regardless of
// umask, atomic overwrite with no temp leftovers, temp-name collision
// handling (skip, never reuse; exhaustion is loud), backup_unique naming.

#include "core/atomic.h"
#include "util/fs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "runner.h"
#include "test_env.h"

namespace {

std::vector<std::filesystem::path> list_dir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    out.push_back(entry.path().filename());
  }
  return out;
}

}
TEST(atomic_write_round_trip_private_perms) {
  const TempDir tmp;
  const auto dest = tmp.path() / "state.json";

  const auto w = berth::atomic_write(dest, "hello");
  CHECK(w.is_ok());
  const auto r = berth::read_file(dest);
  CHECK(r.is_ok());
  CHECK(r.value() == "hello");

  const auto perms = std::filesystem::status(dest).permissions();
  CHECK(perms == berth::kPermPrivate);
}

TEST(atomic_write_explicit_0644) {
  const TempDir tmp;
  const auto dest = tmp.path() / "public.txt";
  const auto perms_0644 =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::group_read | std::filesystem::perms::others_read;
  CHECK(berth::atomic_write(dest, "x", perms_0644).is_ok());
  CHECK(std::filesystem::status(dest).permissions() == perms_0644);
}

TEST(atomic_overwrite_leaves_no_temp_files) {
  const TempDir tmp;
  const auto dest = tmp.path() / "f.json";
  CHECK(berth::atomic_write(dest, "one").is_ok());
  CHECK(berth::atomic_write(dest, "two").is_ok());
  const auto r = berth::read_file(dest);
  CHECK(r.is_ok());
  CHECK(r.value() == "two");
  CHECK(list_dir(tmp.path()).size() == 1);
}

TEST(atomic_write_skips_colliding_temp) {
  const TempDir tmp;
  const auto dest = tmp.path() / "f.json";
  const std::string first_temp =
      dest.string() + ".tmp." + std::to_string(::getpid()) + ".0";
  {
    FILE* f = std::fopen(first_temp.c_str(), "w");
    CHECK(f != nullptr);
    std::fputs("pre-existing junk", f);
    std::fclose(f);
  }

  CHECK(berth::atomic_write(dest, "payload").is_ok());
  const auto r = berth::read_file(dest);
  CHECK(r.is_ok());
  CHECK(r.value() == "payload");
  // The colliding temp was never touched or reused.
  const auto junk = berth::read_file(first_temp);
  CHECK(junk.is_ok());
  CHECK(junk.value() == "pre-existing junk");
  // Exactly two files: dest + the pre-existing temp.
  CHECK(list_dir(tmp.path()).size() == 2);
}

TEST(atomic_write_fails_loudly_on_temp_exhaustion) {
  const TempDir tmp;
  const auto dest = tmp.path() / "f.json";
  for (int i = 0; i < 8; ++i) {
    const std::string name = dest.string() + ".tmp." +
                             std::to_string(::getpid()) + "." +
                             std::to_string(i);
    FILE* f = std::fopen(name.c_str(), "w");
    CHECK(f != nullptr);
    std::fclose(f);
  }
  const auto w = berth::atomic_write(dest, "payload");
  CHECK(w.is_err());
  CHECK(w.error().ctx().find("collision") != std::string::npos);
  CHECK(!std::filesystem::exists(dest));
}

TEST(atomic_write_missing_dir_is_error) {
  const TempDir tmp;
  const auto dest = tmp.path() / "nope" / "f.json";
  CHECK(berth::atomic_write(dest, "x").is_err());
}

TEST(read_file_missing_is_error) {
  const TempDir tmp;
  CHECK(berth::read_file(tmp.path() / "absent").is_err());
}

TEST(ensure_dir_creates_and_enforces_perms) {
  const TempDir tmp;
  const auto dir = tmp.path() / "a" / "b";
  CHECK(berth::ensure_dir(dir).is_ok());
  CHECK(std::filesystem::status(dir).permissions() == berth::kPermPrivateDir);
  // A dir that somehow became world-readable gets fixed, not tolerated.
  std::filesystem::permissions(dir, std::filesystem::perms::all,
                               std::filesystem::perm_options::replace);
  CHECK(berth::ensure_dir(dir).is_ok());
  CHECK(std::filesystem::status(dir).permissions() == berth::kPermPrivateDir);
}

TEST(backup_unique_names_progressively) {
  const TempDir tmp;
  const auto orig = tmp.path() / "state.json";
  CHECK(berth::atomic_write(orig, "bad bytes").is_ok());

  const auto b1 = berth::backup_unique(orig);
  CHECK(b1.is_ok());
  CHECK(b1.value() == orig.string() + ".corrupt");
  const auto c1 = berth::read_file(orig.string() + ".corrupt");
  CHECK(c1.is_ok());
  CHECK(c1.value() == "bad bytes");

  CHECK(berth::atomic_write(orig, "bad bytes 2").is_ok());
  const auto b2 = berth::backup_unique(orig);
  CHECK(b2.is_ok());
  CHECK(b2.value() == orig.string() + ".corrupt.1");

  // The first backup was not overwritten.
  const auto c2 = berth::read_file(orig.string() + ".corrupt");
  CHECK(c2.is_ok());
  CHECK(c2.value() == "bad bytes");
}
