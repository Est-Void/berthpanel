// Tests for fs.h (stage 4.5): recursive removal. The load-bearing case is
// the symlink: a link out of the tree must lose the LINK, never send the
// traversal through it into something we must not touch.

#include "berth/fs.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "atomic.h"
#include "runner.h"
#include "test_env.h"

using berth::remove_tree;

namespace {

void write_file(const std::filesystem::path& path, const std::string& body) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << body;
}

}  // namespace

TEST(fs_removes_nested_tree) {
  const TempDir tmp;
  const auto root = tmp.path() / "service";
  write_file(root / "app" / "bin" / "main", "binary");
  write_file(root / "app" / "config.yaml", "port: 8080");
  write_file(root / "README.md", "hi");

  const auto r = remove_tree(root);
  CHECK(r.is_ok());
  CHECK(r.value() >= 4);
  CHECK(!std::filesystem::exists(root));
}

TEST(fs_is_idempotent_on_missing_path) {
  const TempDir tmp;
  const auto r = remove_tree(tmp.path() / "never-existed");
  CHECK(r.is_ok());
  CHECK(r.value() == 0);
}

TEST(fs_refuses_dangerous_targets) {
  CHECK(remove_tree("/").is_err());
  CHECK(remove_tree("").is_err());
  CHECK(remove_tree("relative/dir").is_err());
}

TEST(fs_removes_plain_file_root) {
  const TempDir tmp;
  const auto file = tmp.path() / "lonely.txt";
  write_file(file, "x");
  const auto r = remove_tree(file);
  CHECK(r.is_ok());
  CHECK(r.value() == 1);
  CHECK(!std::filesystem::exists(file));
}

TEST(fs_symlink_to_file_loses_only_the_link) {
  const TempDir tmp;
  const auto outside = tmp.path() / "precious.txt";
  write_file(outside, "must survive");
  const auto root = tmp.path() / "service";
  write_file(root / "real.txt", "x");
  std::filesystem::create_symlink(outside, root / "link.txt");

  const auto r = remove_tree(root);
  CHECK(r.is_ok());
  CHECK(!std::filesystem::exists(root));
  CHECK(std::filesystem::exists(outside));
  const auto body = berth::read_file(outside);
  CHECK(body.is_ok());
  CHECK(body.value() == "must survive");
}

TEST(fs_symlink_to_directory_is_not_followed) {
  const TempDir tmp;
  const auto outside_dir = tmp.path() / "outside";
  write_file(outside_dir / "inner.txt", "still here");
  const auto root = tmp.path() / "service";
  write_file(root / "file.txt", "x");
  std::filesystem::create_symlink(outside_dir, root / "escape");

  const auto r = remove_tree(root);
  CHECK(r.is_ok());
  CHECK(!std::filesystem::exists(root / "escape"));
  CHECK(std::filesystem::exists(outside_dir / "inner.txt"));
}

TEST(fs_symlinked_root_is_unlinked_not_entered) {
  const TempDir tmp;
  const auto target = tmp.path() / "target";
  write_file(target / "inside.txt", "x");
  const auto link = tmp.path() / "link-to-target";
  std::filesystem::create_symlink(target, link);

  const auto r = remove_tree(link);
  CHECK(r.is_ok());
  CHECK(!std::filesystem::exists(link));
  CHECK(std::filesystem::exists(target / "inside.txt"));
}

TEST(fs_removes_readonly_files) {
  const TempDir tmp;
  const auto root = tmp.path() / "service";
  const auto readonly_file = root / "a" / "b.txt";
  write_file(readonly_file, "x");
  // Deletion depends on the parent directory's write bit, not the file's
  // mode: a read-only file must not stop cleanup.
  std::filesystem::permissions(readonly_file,
                               std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace);
  const auto r = remove_tree(root);
  CHECK(r.is_ok());
  CHECK(!std::filesystem::exists(root));
}
