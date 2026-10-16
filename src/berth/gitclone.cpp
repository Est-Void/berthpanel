#include "gitclone.h"

#include <optional>
#include <string>
#include <vector>

#include "../core/atomic.h"
#include "../util/strutil.h"
#include "../util/subprocess.h"
#include "../util/tmpdir.h"
#include "../util/which.h"
#include "net/fetch.h"
#include "validate.h"

namespace berth {

namespace {

class Askpass {
 public:
  [[nodiscard]] static Result<Askpass> create(const std::string& token);
  [[nodiscard]] const std::filesystem::path& dir() const { return dir_.path(); }

 private:
  explicit Askpass(TempDir dir) : dir_(std::move(dir)) {}
  TempDir dir_;
};

Result<Askpass> Askpass::create(const std::string& token) {
  Result<TempDir> made = TempDir::create("askpass");
  if (made.is_err())
    return std::move(made).error();
  const std::filesystem::path dir = made.value().path();

  const Result<void> token_written =
      atomic_write(dir / "token", token, kPermPrivate);
  if (token_written.is_err())
    return std::move(token_written).error();

  const std::string script =
      "#!/bin/sh\n"
      "case \"$1\" in\n"
      "  *[Uu]sername*) printf '%s\\n' 'x-access-token' ;;\n"
      "  *) exec cat " +
      dir.string() +
      "/token ;;\n"
      "esac\n";
  const Result<void> script_written = atomic_write(
      dir / "askpass", script,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec);
  if (script_written.is_err())
    return std::move(script_written).error();
  return Askpass(std::move(made).value());
}

Result<void> run_git(const std::vector<std::string>& args,
                     const CloneRequest& request,
                     const std::filesystem::path& askpass_dir) {
  Result<std::string> binary = resolve_executable("git");
  if (binary.is_err())
    return std::move(binary).error();

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
  const Result<RunOutput> out = run(argv, limits);
  if (out.is_err())
    return std::move(out).error();
  if (out.value().exit_code != 0) {
    return Error("gitclone: git " + args.front() + " failed with exit " +
                 std::to_string(out.value().exit_code) + ": " +
                 out.value().err);
  }
  return {};
}

Result<void> fetch_pinned_commit(const CloneRequest& request,
                                 const std::filesystem::path& dest,
                                 const std::filesystem::path& askpass_dir) {
  const Result<void> init =
      run_git({"init", "--quiet", dest.string()}, request, askpass_dir);
  if (init.is_err())
    return init;

  const Result<void> remote =
      run_git({"-C", dest.string(), "remote", "add", "origin", request.url},
              request, askpass_dir);
  if (remote.is_err())
    return remote;

  const Result<void> fetched =
      run_git({"-C", dest.string(), "fetch", "--depth", "1", "--quiet",
               "origin", request.commit},
              request, askpass_dir);
  if (fetched.is_err())
    return fetched;

  return run_git({"-C", dest.string(), "-c", "advice.detachedHead=false",
                  "checkout", "--quiet", "--detach", "FETCH_HEAD"},
                 request, askpass_dir);
}

Result<void> verify_head_is(const CloneRequest& request,
                            const std::filesystem::path& dest) {
  const Result<std::string> git_bin = resolve_executable("git");
  if (git_bin.is_err())
    return std::move(git_bin).error();

  RunLimits limits;
  limits.timeout = request.timeout;
  limits.cancel = request.cancel;
  const Result<RunOutput> head =
      run({git_bin.value(), "-C", dest.string(), "rev-parse", "HEAD"}, limits);
  if (head.is_err())
    return std::move(head).error();

  const std::string got(trim(head.value().out));
  if (got != request.commit) {
    return Error("gitclone: HEAD is " + got + " but " + request.commit +
                 " was requested; refusing a moved branch");
  }
  return {};
}

Result<void> check_dest_is_free(const std::filesystem::path& dest) {
  std::error_code ec;
  if (!std::filesystem::exists(dest, ec))
    return ensure_dir(dest.parent_path());
  std::filesystem::directory_iterator it(dest, ec);
  if (!ec && it != std::filesystem::directory_iterator()) {
    return Error("gitclone: destination exists and is not empty: " +
                 dest.string());
  }
  return ensure_dir(dest.parent_path());
}

// No token: GIT_ASKPASS stays unset, so only a credential helper can answer.
class TokenSource {
 public:
  [[nodiscard]] static Result<TokenSource> for_token(const std::string* token);
  [[nodiscard]] const std::filesystem::path& dir() const;

 private:
  explicit TokenSource(Askpass askpass) : askpass_(std::move(askpass)) {}
  explicit TokenSource() = default;

  std::optional<Askpass> askpass_;
};

Result<TokenSource> TokenSource::for_token(const std::string* token) {
  if (token == nullptr)
    return TokenSource();
  Result<Askpass> made = Askpass::create(*token);
  if (made.is_err())
    return std::move(made).error();
  return TokenSource(std::move(made).value());
}

const std::filesystem::path& TokenSource::dir() const {
  static const std::filesystem::path kNoDir;
  return askpass_.has_value() ? askpass_->dir() : kNoDir;
}

bool ref_is_option_shaped(const std::string_view ref) {
  return !ref.empty() && ref != "HEAD" &&
         (ref.front() == '-' || ref.find("..") != std::string_view::npos);
}

struct RefCandidates {
  std::string head;
  std::string named;
  std::string peeled;
};

RefCandidates scan_ls_remote(const std::string& out, const std::string& ref) {
  RefCandidates found;
  for (const std::string& raw : split(out, "\n")) {
    const std::size_t tab = raw.find('\t');
    if (tab == std::string::npos)
      continue;
    const std::string sha = raw.substr(0, tab);
    const std::string name = raw.substr(tab + 1);
    if (name == "HEAD") {
      found.head = sha;
    } else if (name == "refs/" + ref || name == "refs/heads/" + ref ||
               name == "refs/tags/" + ref) {
      found.named = sha;
    } else if (name == "refs/tags/" + ref + "^{}") {
      found.peeled = sha;
    }
  }
  return found;
}

// The peeled ref^{} line is the annotated-tag commit; HEAD is only a fallback
// for an unnamed ref (§6.6).
std::string choose_ref_sha(const RefCandidates& found, const std::string& ref) {
  if (!found.peeled.empty())
    return found.peeled;
  if (!found.named.empty())
    return found.named;
  if (ref.empty() || ref == "HEAD")
    return found.head;
  return {};
}

}
bool token_allowed_for_url(const std::string& url,
                           const std::string_view host) {
  const Result<HttpUrl> parsed = parse_http_url(url);
  if (parsed.is_err())
    return false;
  const HttpUrl& target = parsed.value();
  if (target.scheme != "https")
    return false;
  return target.host == host;
}

Result<void> clone_pinned(const CloneRequest& request) {
  const Result<void> sha_ok = validate_commit_sha(request.commit);
  if (sha_ok.is_err())
    return std::move(sha_ok).error();
  if (request.url.empty())
    return Error("gitclone: empty url");

  if (request.token != nullptr &&
      !token_allowed_for_url(request.url, request.token_host)) {
    return Error(
        "gitclone: refusing to attach a token to a url whose host "
        "is not exactly " +
        std::string(request.token_host));
  }

  const Result<void> dest_free = check_dest_is_free(request.dest_dir);
  if (dest_free.is_err())
    return dest_free;

  const Result<TokenSource> token = TokenSource::for_token(request.token);
  if (token.is_err())
    return std::move(token).error();

  const Result<void> fetched =
      fetch_pinned_commit(request, request.dest_dir, token.value().dir());
  if (fetched.is_err())
    return fetched;
  return verify_head_is(request, request.dest_dir);
}

Result<void> clone_pinned_github(const std::string& owner_repo,
                                 const std::string& commit,
                                 const std::filesystem::path& dest_dir,
                                 const std::string* token,
                                 const std::chrono::milliseconds timeout,
                                 std::atomic<bool>* cancel) {
  const Result<void> repo_ok = validate_owner_repo(owner_repo);
  if (repo_ok.is_err())
    return std::move(repo_ok).error();

  CloneRequest request;
  request.url =
      "https://" + std::string(kTokenHost) + "/" + owner_repo + ".git";
  request.commit = commit;
  request.dest_dir = dest_dir;
  request.token = token;
  request.timeout = timeout;
  request.cancel = cancel;
  return clone_pinned(request);
}

Result<std::string> github_owner_repo_from_url(const std::string& url) {
  const Result<HttpUrl> parsed = parse_http_url(url);
  if (parsed.is_err()) {
    return Error("gitclone: not a valid url: '" + url +
                 "': " + parsed.error().ctx());
  }
  if (parsed.value().scheme != "https" || parsed.value().host != kTokenHost) {
    return Error("gitclone: only https://" + std::string(kTokenHost) +
                 " is accepted: '" + url + "'");
  }
  std::string path = parsed.value().path_query;
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  if (path.size() > 4 && path.substr(path.size() - 4) == ".git") {
    path.erase(path.size() - 4);
  }
  const Result<void> repo = validate_owner_repo(path);
  if (repo.is_err()) {
    return Error("gitclone: url path is not owner/repo: '" + path +
                 "': " + repo.error().ctx());
  }
  return path;
}

Result<std::string> resolve_ref_sha(const std::string& url,
                                    const std::string& ref,
                                    const std::chrono::milliseconds timeout,
                                    std::atomic<bool>* cancel) {
  if (url.empty())
    return Error("gitclone: resolve_ref_sha: empty url");
  // The ref goes into argv: option-shaped or whitespace values are refused.
  if (ref.find_first_of(" \t\n\r") != std::string::npos) {
    return Error("gitclone: ref contains whitespace: '" + ref + "'");
  }
  if (ref_is_option_shaped(ref)) {
    return Error("gitclone: ref looks like an option or a range: '" + ref +
                 "'");
  }

  const Result<std::string> binary = resolve_executable("git");
  if (binary.is_err())
    return std::move(binary).error();

  RunLimits limits;
  limits.timeout = timeout;
  limits.cancel = cancel;
  const std::vector<std::string> env = {
      "GIT_TERMINAL_PROMPT=0",
      "GIT_CONFIG_NOSYSTEM=1",
  };
  limits.extra_env = &env;

  // No ref pattern: `ls-remote <url> v1` matches by suffix and hides the
  // peeled "refs/tags/v1^{}" line an annotated tag must resolve to.
  const Result<RunOutput> out =
      run({std::move(binary).value(), "ls-remote", url}, limits);
  if (out.is_err())
    return std::move(out).error();
  if (out.value().exit_code != 0) {
    return Error("gitclone: ls-remote " + url + " " + ref +
                 " failed with exit " + std::to_string(out.value().exit_code) +
                 ": " + out.value().err);
  }

  const std::string chosen =
      choose_ref_sha(scan_ls_remote(out.value().out, ref), ref);
  if (chosen.empty()) {
    return Error("gitclone: ls-remote found no ref '" + ref + "' in " + url);
  }
  const Result<void> sha_ok = validate_commit_sha(chosen);
  if (sha_ok.is_err()) {
    return Error("gitclone: ls-remote produced a non-SHA for '" + ref +
                 "' in " + url + ": " + chosen);
  }
  return chosen;
}

}
