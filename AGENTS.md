# AGENTS.md — berthpanel

> Status: **alpha** (`v0.1.0-alpha`) — code complete, release work is docs/notes only.
> The full spec lives in `docs/dev.md`, which is **gitignored** (`docs/*`) — read it locally, never expect it on GitHub.

## Source of truth
- Product pitch: `README.md`. Developer spec: `docs/dev.md` (§6 invariants, §8 catalog, §10 release, §11 traps are inherited, must-not-regress; rest is plan).
- Staged roadmap: `docs/plan.md` (local) — phase/stage order, per-stage done criteria; follow it when implementing.
- `docs/dev.md` §12 records the **settled decisions** and the v2 deferrals (catalog frozen at wave 1; github.com-only git egress; user-recipe editing added; multi-unit stacks and automatic toolchain provisioning → v2). Read it before assuming anything is settled.

## Build (in place — see dev.md §3)
- C++20 baseline (GCC 13+ / Clang 16+), hand-written `Makefile`, no CMake/Meson. TUI: FTXUI. TLS: vendored mbedTLS (static). Vendored: miniz, nlohmann/json. System: zlib, `git`, `systemd --user`, `python3 >= 3.10`.
- Targets: `make` → `build/debug/berthpanel`; `make release`, `make test`, `make check` (read-only gate: lint+test, never rewrites), `make asan`, `make musl` (pinned Arch container **by digest**; currently needs a musl-native C++ toolchain via `MUSL_CXX` — the default `g++` + musl-specs path strips the C++ include paths and fails on the vendored C++), `make clean`.
- Warnings as errors incl. `-Wold-style-cast -Wconversion`; no `-fno-exceptions` (exceptions only for programmer error, never across module boundaries).

## Conventions that differ from defaults
- Errors: single `Result<T>` alias (`berth::Result`, `src/util/result.h`) for recoverable failures; every error carries `ctx()` context; **never discard a `Result`**; invariant violations abort, not `Result`.
- Subprocesses: argv arrays only — `system()`/`popen()` banned, never join/split on whitespace (breaks on spaces in paths). Every call takes argv + timeout. Nothing on the UI thread blocks (`systemctl`, `stat`, walks go to workers via message queue).
- Naming: `berth::` namespaces, `snake_case` files/functions, `PascalCase` types, `kCamelCase` consts, trailing `_` members, `#pragma once`.
- Layout: `src/main.cpp` (CLI incl. `__deploy/__cleanup/__serve-static/__egress-probe`), `paths.cpp` = single XDG truth, `berth/` = deploy engine, `tui/` = panel. **Layering: `berth` must not depend on `tui`.**
- Name is load-bearing in 7 places, change atomically: binary `berthpanel`, XDG state `~/.local/state/berthpanel/`, data `~/.local/share/berthpanel/services/`, unit prefix `berthpanel-`, `berthpanel-cleanup.timer/.service`, `BERTHPANEL_*` env.

## Security invariants (do not regress — see dev.md §6, §11)
- Allowlisted egress only (`kAllowedEndpoints`, per-hop check, no redirect following); every request → `net.log`. Sandbox (`systemd-run --user`) mandatory at grade FULL, no env-settable override. State it honestly: blast-radius reducer, not a trust boundary.
- Tokens via `GIT_ASKPASS` only (exact host match); private files 0600 via `atomic.cpp` (`O_EXCL` temp + rename, never bare `ofstream`); `tar` reader fails loudly on unsafe entries; one shared `validate_unit_name` for all unit paths; empty env vars = unset; corrupt JSON = loud backup, never silent default.
- Catalog recipes: pinned commit SHAs, argv build steps, mandatory `memory_max`/`tasks_max`, loopback-only bind; tests in same file enforce limits/tools/bind.
- Tests: hand-rolled runner (no dep), prefer pure functions; env-var tests need the global `env_test_lock()` + serialized run; network tests opt-in only, never in CI. Manual: `--show-endpoints`, `--selftest` (temp dir with random suffix), `--verify-sandbox`, `__cleanup`.
