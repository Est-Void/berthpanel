#include "berth/recipes.h"

#include <string>

#include "runner.h"

using berth::Recipe;
using berth::split_command_template;
using berth::validate_recipe;

namespace {

Recipe good() {
  Recipe r;
  r.owner = "usememos";
  r.name = "example";
  r.commit = "0123456789abcdef0123456789abcdef01234567";
  r.language = "go";
  r.service_name = "berthpanel-example";
  r.display_name = "Example";
  r.description = "test";
  r.build_steps = {{"go", "build", "-o", "example", "."}};
  r.tools = {"go"};
  r.binary_name = "example";
  r.start_cmd = "{bin} --addr 127.0.0.1 --port {port} --data {project}/data";
  r.res = {"512M", 300};
  return r;
}

}  // namespace

TEST(recipes_split_template_before_substitution) {
  const auto tokens =
      split_command_template("/bin/app --addr 127.0.0.1 --port {port}");
  CHECK(tokens.is_ok());
  CHECK(tokens.value().size() == 5);
  CHECK(tokens.value()[3] == "--port");
  CHECK(tokens.value()[4] == "{port}");

  const auto quoted =
      split_command_template("\"/opt/my app/bin\" --port {port}");
  CHECK(quoted.is_ok());
  CHECK(quoted.value()[0] == "/opt/my app/bin");  // the space survives

  CHECK(split_command_template("").is_err());
  CHECK(split_command_template("\"unbalanced").is_err());
}

TEST(recipes_validator_accepts_a_sound_recipe) {
  CHECK(validate_recipe(good()).is_ok());
}

TEST(recipes_validator_requires_limits) {
  Recipe r = good();
  r.res = {"", 0};
  CHECK(validate_recipe(r).is_err());
  r.res = {"512M", 0};
  CHECK(validate_recipe(r).is_err());
  r.res = {"not-a-size", 300};
  CHECK(validate_recipe(r).is_err());
  r.res = {"512M", 1};
  CHECK(validate_recipe(r).is_ok());
}

TEST(recipes_validator_requires_loopback_bind) {
  Recipe r = good();
  r.start_cmd = "{bin} --port {port}";
  CHECK(validate_recipe(r).is_err());  // no explicit loopback

  r.start_cmd = "{bin} --addr 0.0.0.0 --port {port}";
  CHECK(validate_recipe(r).is_err());  // wildcard

  r.start_cmd = "{bin} --port {port} --data /var/lib/app";
  CHECK(validate_recipe(r).is_err());  // no {port}-adjacent loopback anywhere

  r.start_cmd = "{bin} --addr localhost --port {port}";
  CHECK(validate_recipe(r).is_ok());
}

TEST(recipes_validator_requires_port_placeholder) {
  Recipe r = good();
  r.start_cmd = "{bin} --addr 127.0.0.1";
  CHECK(validate_recipe(r).is_err());
}

TEST(recipes_validator_requires_tools_to_cover_build_steps) {
  Recipe r = good();
  r.tools = {};  // go build but no go declared
  CHECK(validate_recipe(r).is_err());

  r = good();
  r.build_steps = {{"sh", "-c", "make"}};
  r.tools = {"sh"};
  CHECK(validate_recipe(r).is_ok());

  r.build_steps = {{"bun", "install"}};
  CHECK(validate_recipe(r).is_err());  // bun not in tools
}

TEST(recipes_validator_rejects_dangerous_steps_and_shell_syntax) {
  Recipe r = good();
  r.build_steps = {{"sh", "-c", "sudo make install"}};
  CHECK(validate_recipe(r).is_err());

  r = good();
  r.start_cmd = "{bin} --addr 127.0.0.1 --port {port}; rm -rf /";
  CHECK(validate_recipe(r).is_err());

  r = good();
  r.start_cmd = "{bin} $(whoami) --addr 127.0.0.1 --port {port}";
  CHECK(validate_recipe(r).is_err());
}

TEST(recipes_validator_requires_pins_and_allowlisted_urls) {
  Recipe r = good();
  r.commit = "v1.0.0";
  CHECK(validate_recipe(r).is_err());

  r = good();
  r.plugins = {{"https://evil.example.com/plugin.js", std::string(64, 'a')}};
  CHECK(validate_recipe(r).is_err());  // host not allowlisted

  r.plugins = {{"https://cdn.jsdelivr.net/npm/x/plugin.js", "not-a-hash"}};
  CHECK(validate_recipe(r).is_err());

  r.plugins = {
      {"https://cdn.jsdelivr.net/npm/x/plugin.js", std::string(64, 'a')}};
  CHECK(validate_recipe(r).is_ok());
}

TEST(recipes_validator_prebuilt_needs_pinned_asset) {
  Recipe r = good();
  r.language = "prebuilt";
  r.build_steps.clear();
  r.tools.clear();
  CHECK(validate_recipe(r).is_err());  // no prebuilt pin

  r.prebuilt = berth::PrebuiltBinary{
      .url = "https://github.com/o/r/releases/download/v1/x.tar.gz",
      .sha256 = std::string(64, 'b'),
      .member = "app"};
  CHECK(validate_recipe(r).is_ok());

  r.prebuilt->sha256 = "short";
  CHECK(validate_recipe(r).is_err());

  r.prebuilt->url = "https://evil.example.com/app.tar.gz";
  r.prebuilt->sha256 = std::string(64, 'b');
  CHECK(validate_recipe(r).is_err());
}

TEST(recipes_validator_rejects_privileged_ports) {
  Recipe r = good();
  r.port = 80;
  CHECK(validate_recipe(r).is_err());
  r.port = 8080;
  CHECK(validate_recipe(r).is_ok());
}

TEST(recipes_json_round_trip) {
  const Recipe r = good();
  const auto doc = berth::recipe_to_json(r);
  const auto back = berth::recipe_from_json(doc);
  CHECK(back.is_ok());
  CHECK(back.value().service_name == r.service_name);
  CHECK(back.value().commit == r.commit);
  CHECK(back.value().res.memory_max == "512M");
  CHECK(back.value().res.tasks_max == 300);
  CHECK(back.value().build_steps.size() == 1);
  CHECK(back.value().build_steps[0].front() == "go");
  CHECK(validate_recipe(back.value()).is_ok());
}

TEST(recipes_prebuilt_archive_defaults_to_targz_and_round_trips_raw) {
  // An existing user recipe has no archive field and must keep loading as the
  // tar.gz shape (absent = kTarGz).
  const nlohmann::json doc = {
      {"owner", "o"},
      {"name", "r"},
      {"commit", "0123456789abcdef0123456789abcdef01234567"},
      {"language", "prebuilt"},
      {"service_name", "berthpanel-x"},
      {"binary_name", "x"},
      {"start_cmd", "{self} --addr 127.0.0.1 --port {port}"},
      {"tools", nlohmann::json::array()},
      {"res", {{"memory_max", "128M"}, {"tasks_max", 50}}},
      {"prebuilt",
       {{"url", "https://github.com/o/r/releases/download/v1/x.tar.gz"},
        {"sha256", std::string(64, 'a')},
        {"member", "x"}}}};
  const auto absent = berth::recipe_from_json(doc);
  CHECK(absent.is_ok());
  CHECK(absent.value().prebuilt.has_value());
  CHECK(absent.value().prebuilt->archive == berth::ToolArchive::kTarGz);

  Recipe raw = good();
  raw.language = "prebuilt";
  raw.build_steps.clear();
  raw.tools.clear();
  raw.prebuilt = berth::PrebuiltBinary{
      .url = "https://github.com/o/r/releases/download/v1/x-linux-amd64",
      .sha256 = std::string(64, 'c'),
      .member = "x",
      .archive = berth::ToolArchive::kRaw};
  CHECK(validate_recipe(raw).is_ok());  // a raw asset is a valid pin
  const nlohmann::json out = berth::recipe_to_json(raw);
  CHECK(out.at("prebuilt").at("archive") == "raw");
  const auto back = berth::recipe_from_json(out);
  CHECK(back.is_ok());
  CHECK(back.value().prebuilt->archive == berth::ToolArchive::kRaw);

  nlohmann::json bad = out;
  bad["prebuilt"]["archive"] = "zip";
  CHECK(berth::recipe_from_json(bad).is_err());
}