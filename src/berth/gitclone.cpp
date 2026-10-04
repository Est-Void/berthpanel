#include "gitclone.h"

#include <stdlib.h>

#include <string>
#include <vector>

#include "../atomic.h"
#include "fs.h"
#include "../util/subprocess.h"
#include "../util/which.h"
#include "httpclient.h"
#include "validate.h"

namespace berth {

namespace {

struct Askpass {
  std::filesystem::path dir;
  ~Askpass() {
    if (!dir.empty()) {
      Result<std::size_t> ignored = remove_tree(dir);
      static_cast<void>(ignored);
    }
  }
};

Result<std::filesystem::path> make_askpass(const std::string& token) {
  std::string tmpl = (std::string("/tmp/berthpanel-askpass-XXXXXX"));
  const char* made = ::mkdtemp(tmpl.data());
  if (made == nullptr) return Error("gitclone: mkdtemp failed");
  const std::filesystem::path dir(made);
  Result<void> chmod_dir = ensure_dir(dir);
  if (chmod_dir.is_err()) return std::move(chmod_dir).error();

  const std::filesystem::path token_file = dir / "token";
  Result<void> written = atomic_write(token_file, token, kPermPrivate);
  if (written.is_err()) return std::move(written).error();

  const std::string script = "#!/bin/sh\n"
                             "case \"$1\" in\n"
                             "  *[Uu]sername*) printf '%s\\n' 'x-access-token' ;;\n"
                             "  *) exec cat " +
                             dir.string() + "/token ;;\n"
                             "esac\n";
  const std::filesystem::path script_path = dir / "askpass";
  Result<void> script_written =
      atomic_write(script_path, script,
                   std::filesystem::perms::owner_read |
                       std::filesystem::perms::owner_write |
                       std::filesystem::perms::owner_exec);
  if (script_written.is_err()) return std::move(script_written).error();
  return dir;
}

Result<void> run_git(const std::vector<std::string>& args,
                     const CloneRequest& request,
                     const std::filesystem::path& askpass_dir) {
  Result<std::string> binary = resolve_executable("git");
  if (binary.is_err()) return std::move(binary).error();

  std::vector<std::string> env = {
      "GIT_TERMINAL_PROMPT=0",
      "GIT_CONFIG_NOSYSTEM=1",
  };
  if (!askpass_dir.empty()) {
    env.push_back("GIT_ASKPASS=" + (askpass_dir / "askpass").string());
  }

  RunLimits limits;
  limits.timeout = request.timeout;
  limits.cancel = request.cancel;
  limits.extra_env = &env;

  std::vector<std::string> argv{std::move(binary).value()};
  argv.insert(argv.end(), args.begin(), args.end());
  Result<RunOutput> out = run(argv, limits);
  if (out.is_err()) return std::move(out).error();
  if (out.value().exit_code != 0) {
    return Error("gitclone: git " + args.front() + " failed with exit " +
                 std::to_string(out.value().exit_code) + ": " + out.value().err);
  }
  return {};
}

}  // namespace

bool token_allowed_for_url(const std::string& url, const std::string_view host) {
  const Result<HttpUrl> parsed = parse_http_url(url);
  if (parsed.is_err()) return false;
  const HttpUrl& target = parsed.value();
  if (target.scheme != "https") return false;
  return target.host == host;
}

Result<void> clone_pinned(const CloneRequest& request) {
  Result<void> sha_ok = validate_commit_sha(request.commit);
  if (sha_ok.is_err()) return std::move(sha_ok).error();
  if (request.url.empty()) return Error("gitclone: empty url");

  if (request.token != nullptr &&
      !token_allowed_for_url(request.url, request.token_host)) {
    return Error("gitclone: refusing to attach a token to a url whose host "
                 "is not exactly " +
                 std::string(request.token_host));
  }

  std::error_code ec;
  if (std::filesystem::exists(request.dest_dir, ec)) {
    std::filesystem::directory_iterator it(request.dest_dir, ec);
    if (!ec && it != std::filesystem::directory_iterator()) {
      return Error("gitclone: destination exists and is not empty: " +
                   request.dest_dir.string());
    }
    Result<void> removed = ensure_dir(request.dest_dir.parent_path());
    if (removed.is_err()) return removed;
  } else {
    Result<void> made = ensure_dir(request.dest_dir.parent_path());
    if (made.is_err()) return std::move(made).error();
  }

  Askpass askpass;
  if (request.token != nullptr) {
    Result<std::filesystem::path> dir = make_askpass(*request.token);
    if (dir.is_err()) return std::move(dir).error();
    askpass.dir = std::move(dir).value();
  }

  const std::filesystem::path dest = request.dest_dir;
  Result<void> init = run_git({"init", "--quiet", dest.string()}, request,
                              askpass.dir);
  if (init.is_err()) return init;
  Result<void> remote =
      run_git({"-C", dest.string(), "remote", "add", "origin", request.url},
              request, askpass.dir);
  if (remote.is_err()) return remote;
  Result<void> fetch = run_git({"-C", dest.string(), "fetch", "--depth", "1",
                                "--quiet", "origin", request.commit},
                               request, askpass.dir);
  if (fetch.is_err()) return fetch;
  Result<void> checkout =
      run_git({"-C", dest.string(), "-c", "advice.detachedHead=false",
               "checkout", "--quiet", "--detach", "FETCH_HEAD"},
              request, askpass.dir);
  if (checkout.is_err()) return checkout;

  Result<std::string> git_bin = resolve_executable("git");
  if (git_bin.is_err()) return std::move(git_bin).error();
  RunLimits limits;
  limits.timeout = request.timeout;
  limits.cancel = request.cancel;
  Result<RunOutput> head =
      run({git_bin.value(), "-C", dest.string(), "rev-parse", "HEAD"}, limits);
  if (head.is_err()) return std::move(head).error();
  std::string got = head.value().out;
  while (!got.empty() && (got.back() == '\n' || got.back() == ' ')) {
    got.pop_back();
  }
  if (got != request.commit) {
    return Error("gitclone: HEAD is " + got + " but " + request.commit +
                 " was requested; refusing a moved branch");
  }
  return {};
}

Result<void> clone_pinned_github(
    const std::string& owner_repo, const std::string& commit,
    const std::filesystem::path& dest_dir, const std::string* token,
    const std::chrono::milliseconds timeout, std::atomic<bool>* cancel) {
  Result<void> repo_ok = validate_owner_repo(owner_repo);
  if (repo_ok.is_err()) return std::move(repo_ok).error();
  CloneRequest request;
  request.url = "https://" + std::string(kTokenHost) + "/" + owner_repo + ".git";
  request.commit = commit;
  request.dest_dir = dest_dir;
  request.token = token;
  request.timeout = timeout;
  request.cancel = cancel;
  return clone_pinned(request);
}

}  // namespace berth
