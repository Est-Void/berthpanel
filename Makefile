# berthpanel — local-first deploy panel for curated self-hosted apps.
# Hand-written build, no CMake/Meson (docs/dev.md §3).
#
# Targets: make / debug, release, repro, test, check, asan, musl, format, clean.
# Overridable: CXX CXXSTD WARNFLAGS OPTFLAGS PREFIX DESTDIR CONTAINER CLANG_TIDY.

CXX       ?= g++
CC        ?= cc
CXXSTD    ?= -std=c++20
WARNFLAGS ?= -Wall -Wextra -Wpedantic -Werror \
             -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor \
             -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
             -Wdouble-promotion -Wformat=2 -Wnull-dereference
OPTFLAGS  ?= -O0 -g
PREFIX    ?= /usr/local
DESTDIR   ?=
# Pinned by DIGEST, never by tag (dev.md §3). This Makefile is the single
# source of the digest; .github/workflows/ci.yml builds through `make musl`
# and inherits it, so there is nothing to keep in sync. A floating tag in a
# supply-chain-sensitive build is the whole attack.
CONTAINER ?= ghcr.io/archlinux/archlinux@sha256:67f575e6ae9319883231be5e32468bb88d4afcfab281b5ac014bd03f0571f3bc

BUILD     := build
VERSION_H := $(BUILD)/generated/version.h

.DEFAULT_GOAL := all

# Explicit source list, never a wildcard (dev.md §3: parallel make must not
# be able to reorder objects; this list is the order). LIB_SRC is linked
# into the main binary AND the test binary; SRC stays the CLI shell
# (layering: berth never links tui).
# SRC is the shell that links the binary only. app.cpp lives here and not in
# LIB_SRC because it is the one module that includes FTXUI: the test binary
# must stay buildable without the vendored TUI library (dev.md §9).
SRC := \
	src/main.cpp \
	src/tui/panel.cpp \
	src/tui/screens.cpp \
	src/tui/app.cpp

LIB_SRC := \
	src/core/paths.cpp \
	src/core/atomic.cpp \
	src/core/json.cpp \
	src/core/state.cpp \
	src/core/flags.cpp \
	src/core/deploylog.cpp \
	src/core/netlog.cpp \
	src/core/netstatus.cpp \
	src/core/names.cpp \
	src/core/applog.cpp \
	src/core/allowlist.cpp \
	src/berth/net/client.cpp \
	src/berth/net/doh.cpp \
	src/berth/net/egress.cpp \
	src/berth/net/fetch.cpp \
	src/berth/net/github_api.cpp \
	src/berth/net/protocol.cpp \
	src/berth/net/resolver.cpp \
	src/berth/net/socket.cpp \
	src/berth/net/tar.cpp \
	src/berth/net/tls.cpp \
	src/berth/net/url.cpp \
	src/berth/validate.cpp \
	src/berth/port.cpp \
	src/berth/lock.cpp \
	src/berth/journal.cpp \
	src/berth/units.cpp \
	src/berth/secrets.cpp \
	src/berth/gitclone.cpp \
	src/berth/toolcheck.cpp \
	src/berth/toolbox.cpp \
	src/berth/goenv.cpp \
	src/berth/preflight.cpp \
	src/berth/prefetch.cpp \
 	src/berth/sandbox.cpp \
 	src/berth/cancel.cpp \
 	src/berth/selftest.cpp \
 	src/berth/recipes.cpp \
 	src/berth/userrecipes.cpp \
 	src/berth/recipe_edit.cpp \
 	src/berth/stacks.cpp \
 	src/berth/detect.cpp \
	src/berth/wizard.cpp \
 	src/berth/catalog.cpp \
	src/berth/deploy.cpp \
	src/berth/service_ops.cpp \
	src/berth/serve.cpp \
	src/berth/status.cpp \
	src/berth/proxy.cpp \
	src/berth/backup.cpp \
	src/analyzer/ports.cpp \
	src/analyzer/scan.cpp \
	src/util/fs.cpp \
	src/util/tmpdir.cpp \
	src/util/which.cpp \
	src/util/gzip.cpp \
	src/util/subprocess.cpp \
	src/util/tail.cpp \
	src/tui/chrome.cpp \
	src/tui/ops.cpp \
	src/tui/view_catalog.cpp \
	src/tui/view_deploy.cpp \
	src/tui/view_edit.cpp \
	src/tui/view_refresh.cpp \
	src/tui/view_scan.cpp \
	src/tui/view_logs.cpp \
	src/tui/view_services.cpp

# Vendored C sources, compiled per mode (relaxed warnings on purpose; see
# the vendor rule below). Manifests with pins live next to each source.
# mbedTLS is a pinned tree: `sort` over its wildcard is deterministic.
VENDOR_SRC := \
	third_party/sha2/sha256.c
MBEDTLS_SRC := $(sort $(wildcard third_party/mbedtls/library/*.c))
ALL_VENDOR_SRC := $(VENDOR_SRC) $(MBEDTLS_SRC)

# Vendored C++ (FTXUI, dev.md §2): only the three module dirs are vendored,
# and `sort` over the wildcards keeps parallel make deterministic.
FTXUI_SRC := $(sort \
	$(wildcard third_party/ftxui/src/ftxui/dom/*.cpp) \
	$(wildcard third_party/ftxui/src/ftxui/screen/*.cpp) \
	$(wildcard third_party/ftxui/src/ftxui/component/*.cpp))
ALL_VENDOR_CXX_SRC := $(FTXUI_SRC)

TRUST_PEM := third_party/trust/cacert.pem
GEN_TRUST_CPP := $(BUILD)/generated/trust_store.cpp

TEST_SRC := \
	tests/test_main.cpp \
	tests/json_smoke.cpp \
	tests/result_test.cpp \
	tests/strutil_test.cpp \
	tests/subprocess_test.cpp \
	tests/sha256_test.cpp \
	tests/paths_test.cpp \
	tests/atomic_test.cpp \
	tests/json_test.cpp \
	tests/state_test.cpp \
	tests/jsonout_test.cpp \
	tests/flags_test.cpp \
	tests/netlog_test.cpp \
	tests/netstatus_test.cpp \
	tests/mbedtls_smoke.cpp \
	tests/httpclient_test.cpp \
	tests/resolver_test.cpp \
	tests/egress_test.cpp \
	tests/rawfetch_test.cpp \
	tests/validate_test.cpp \
	tests/port_test.cpp \
	tests/lock_test.cpp \
	tests/journal_test.cpp \
	tests/fs_test.cpp \
	tests/recipe_draft_test.cpp \
	tests/units_test.cpp \
	tests/service_ops_test.cpp \
	tests/secrets_test.cpp \
	tests/which_test.cpp \
	tests/gitclone_test.cpp \
	tests/toolcheck_test.cpp \
	tests/toolbox_test.cpp \
	tests/goenv_test.cpp \
	tests/preflight_test.cpp \
	tests/prefetch_test.cpp \
	tests/sandbox_test.cpp \
	tests/selftest_test.cpp \
	tests/recipes_test.cpp \
 	tests/catalog_test.cpp \
 	tests/userrecipes_test.cpp \
 	tests/recipe_edit_test.cpp \
 	tests/stacks_test.cpp \
 	tests/detect_test.cpp \
	tests/wizard_test.cpp \
 	tests/deploy_test.cpp \
 	tests/deploy_stack_test.cpp \
 	tests/cancel_test.cpp \
 	tests/gzip_test.cpp \
	tests/ring_test.cpp \
	tests/ops_test.cpp \
	tests/tail_test.cpp \
	tests/logs_view_test.cpp \
	tests/ports_test.cpp \
	tests/scan_view_test.cpp \
	tests/deploy_view_test.cpp \
	tests/tui_view_test.cpp \
	tests/edit_view_test.cpp \
	tests/refresh_view_test.cpp \
	tests/serve_test.cpp \
	tests/status_test.cpp \
	tests/proxy_test.cpp \
	tests/backup_test.cpp \
	tests/musl_test.cpp \
	tests/release_test.cpp \
	tests/installer_test.cpp \
	tests/layering_test.cpp

INCLUDES := -Iinclude -Isrc -I$(BUILD)/generated -isystem third_party -isystem third_party/mbedtls/include -isystem third_party/ftxui/include -isystem third_party/ftxui/src
LDLIBS   := -lz -pthread
DEPFLAGS := -MMD -MP

# Reproducible release (dev.md §3): fixed epoch, no absolute build paths in
# the binary, stable random seed, deterministic build-id. The release script
# exports the real SOURCE_DATE_EPOCH; the default keeps bare `make release`
# stable too. Split compile/link so `-frandom-seed` lands where GCC uses it
# (compile) and the linker flags land at link, each exactly once.
#
#   -Wl,--build-id=sha1  deterministic id; never let the linker default to a
#                        random one.
#   -Wl,-s               strip (dev.md §3: `make release` is "optimised,
#                        stripped"); deterministic, and keeps the sha1 note.
export SOURCE_DATE_EPOCH ?= 0
REPRO_CFLAGS := -ffile-prefix-map=$(CURDIR)=. \
                -fdebug-prefix-map=$(CURDIR)=. \
                -frandom-seed=berthpanel
REPRO_LDFLAGS := $(REPRO_CFLAGS) -Wl,--build-id=sha1 -Wl,-s

# --- generated version header (dev.md §3: version comes from git, and there
# is no __DATE__/__TIME__ anywhere). Rewritten only when the value changes,
# so incremental builds stay quiet.

$(VERSION_H): FORCE
	@mkdir -p $(@D)
	@ver=$$(git describe --tags --always --dirty 2>/dev/null || printf v0.0.0-unknown); \
	sha=$$(git rev-parse --short HEAD 2>/dev/null || printf none); \
	printf '#pragma once\n' > $@.tmp; \
	printf '// Generated from git by the Makefile. Do not edit.\n' >> $@.tmp; \
	printf '#define BERTH_VERSION "%s"\n' "$$ver" >> $@.tmp; \
	printf '#define BERTH_COMMIT "%s"\n' "$$sha" >> $@.tmp; \
	cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

.PHONY: FORCE
FORCE:

# --- generated trust store (dev.md §2: embedded Mozilla roots, no system
# CA paths). Deterministic generator; regenerated only when the pinned PEM
# or the script changes.

$(GEN_TRUST_CPP): $(TRUST_PEM) scripts/gen-trust-store.py
	@mkdir -p $(@D)
	python3 scripts/gen-trust-store.py $(TRUST_PEM) $@

# --- build modes ---

define mode_template
$(1)_DIR   := $(BUILD)/$(1)
$(1)_OBJ   := $$(addprefix $$($(1)_DIR)/obj/,$$(patsubst src/%.cpp,%.o,$$(SRC) $$(LIB_SRC)))
$(1)_VOBJ  := $$(addprefix $$($(1)_DIR)/obj/,$$(patsubst third_party/%.c,%.o,$$(ALL_VENDOR_SRC)))
$(1)_VCXXOBJ := $$(addprefix $$($(1)_DIR)/obj/,$$(patsubst third_party/%.cpp,%.o,$$(ALL_VENDOR_CXX_SRC)))
$(1)_GOBJ  := $$($(1)_DIR)/obj/trust_store.o
$(1)_BIN   := $$($(1)_DIR)/berthpanel
$(1)_FLAGS := $(2)
$(1)_VFLAGS := $(3)
$(1)_LDFLAGS := $(4)

.PHONY: $(1)
$(1): $$($(1)_BIN)

$$($(1)_OBJ): $$($(1)_DIR)/obj/%.o: src/%.cpp $(VERSION_H) | $$($(1)_DIR)/obj
	@mkdir -p $$(@D)
	$$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $$($(1)_FLAGS) $(DEPFLAGS) -c $$< -o $$@

# Vendored C: no WARNFLAGS gate (third-party code must not gate on our
# warning set); dep files ARE generated — 109 mbedTLS objects must not
# recompile on every invocation. Mode-aware so sanitizers still see it.
$$($(1)_VOBJ): $$($(1)_DIR)/obj/%.o: third_party/%.c | $$($(1)_DIR)/obj
	@mkdir -p $$(@D)
	$$(CC) $$($(1)_VFLAGS) -Ithird_party/mbedtls/include $(DEPFLAGS) -c $$< -o $$@

# Vendored C++ (FTXUI, dev.md §2): -w, because third-party code must not
# gate on our -Werror set, and -isystem so their headers never trip our
# warning flags either. Mode-aware, so sanitizers still see it.
$$($(1)_VCXXOBJ): $$($(1)_DIR)/obj/%.o: third_party/%.cpp | $$($(1)_DIR)/obj
	@mkdir -p $$(@D)
	$(CXX) $(CXXSTD) -isystem third_party/ftxui/include -isystem third_party/ftxui/src $($(1)_VFLAGS) -w $$(DEPFLAGS) -c $$< -o $$@

# The generated trust store compiles with our strict C++ flags; the
# generator emits plain, warning-free code (dev.md §2: no system CA paths).
$$($(1)_GOBJ): $$($(1)_DIR)/obj/%.o: $(GEN_TRUST_CPP) | $$($(1)_DIR)/obj
	@mkdir -p $$(@D)
	$$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $$($(1)_FLAGS) $(DEPFLAGS) -c $$< -o $$@

$$($(1)_BIN): $$($(1)_OBJ) $$($(1)_VOBJ) $$($(1)_VCXXOBJ) $$($(1)_GOBJ) | $$($(1)_DIR)
	$$(CXX) $$($(1)_FLAGS) $$($(1)_LDFLAGS) $$(filter %.o,$$^) $(LDLIBS) -o $$@

$$($(1)_DIR)/obj $$($(1)_DIR):
	mkdir -p $$@

-include $$($(1)_OBJ:.o=.d) $$($(1)_VOBJ:.o=.d) $$($(1)_VCXXOBJ:.o=.d) $$($(1)_GOBJ:.o=.d)
endef

ASAN_FLAGS := -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer

$(eval $(call mode_template,debug,$(OPTFLAGS),-O2,))
$(eval $(call mode_template,release,-O2 -DNDEBUG $(REPRO_CFLAGS),-O2,$(REPRO_LDFLAGS)))
$(eval $(call mode_template,asan,$(OPTFLAGS) $(ASAN_FLAGS),-O2 $(ASAN_FLAGS),))

.PHONY: all
all: debug

# --- tests: hand-rolled runner, single-threaded by design (dev.md §9) ---

TEST_DIR := $(BUILD)/test
TEST_BIN := $(TEST_DIR)/berthpanel-tests
TEST_CASE_OBJ := $(addprefix $(TEST_DIR)/obj/,$(patsubst tests/%.cpp,%.o,$(TEST_SRC)))
TEST_LIB_OBJ := $(addprefix $(TEST_DIR)/obj/,$(patsubst src/%.cpp,%.o,$(LIB_SRC)))
TEST_VENDOR_OBJ := $(addprefix $(TEST_DIR)/obj/,$(patsubst third_party/%.c,%.o,$(ALL_VENDOR_SRC)))
TEST_TRUST_OBJ := $(TEST_DIR)/obj/trust_store.o
TEST_OBJ := $(TEST_CASE_OBJ) $(TEST_LIB_OBJ)

# Sanitizer flags for the test objects. Empty for `make test`; `make asan`
# re-runs this makefile with SANITIZE set and a separate TEST_DIR, so the test
# binary is instrumented exactly like the `asan` binary. Without this the
# sanitizers covered only build/asan/berthpanel and every test-path leak or UB
# was invisible (dev.md §6.10 names ASan/UBSan as THE gate).
SANITIZE ?=
# Leak detection is NOT left implicit: detect_leaks=1 is set explicitly so a
# green suite cannot mean "the leak detector was compiled out". UBSan halts on
# the first error so a report is not buried under later output.
ASAN_ENV ?=
ASAN_TEST_ENV := ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

.PHONY: test
test: $(TEST_BIN)
	$(ASAN_ENV) $(TEST_BIN)

$(TEST_CASE_OBJ): $(TEST_DIR)/obj/%.o: tests/%.cpp $(VERSION_H) | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS) $(SANITIZE) $(DEPFLAGS) -c $< -o $@

$(TEST_LIB_OBJ): $(TEST_DIR)/obj/%.o: src/%.cpp $(VERSION_H) | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS) $(SANITIZE) $(DEPFLAGS) -c $< -o $@

$(TEST_VENDOR_OBJ): $(TEST_DIR)/obj/%.o: third_party/%.c | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CC) -O2 $(SANITIZE) -Ithird_party/mbedtls/include $(DEPFLAGS) -c $< -o $@

$(TEST_TRUST_OBJ): $(TEST_DIR)/obj/%.o: $(GEN_TRUST_CPP) | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS) $(SANITIZE) $(DEPFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJ) $(TEST_VENDOR_OBJ) $(TEST_TRUST_OBJ) | $(TEST_DIR)
	$(CXX) $(SANITIZE) $(filter %.o,$^) $(LDLIBS) -o $@

$(TEST_DIR)/obj $(TEST_DIR):
	mkdir -p $@

-include $(TEST_OBJ:.o=.d) $(TEST_VENDOR_OBJ:.o=.d) $(TEST_TRUST_OBJ:.o=.d)

# `make asan` must mean "the whole suite ran under ASan+UBSan+LSan", not just
# that an instrumented binary exists: build the instrumented test binary and
# run it. A separate TEST_DIR keeps the plain test objects intact (switching
# SANITIZE without it would leave half-instrumented objects behind).
.PHONY: asan-test
asan-test:
	$(MAKE) test TEST_DIR=$(BUILD)/asan-test SANITIZE="$(ASAN_FLAGS)" ASAN_ENV="$(ASAN_TEST_ENV)"

asan: asan-test

# --- musl: reproducible static release in the pinned Arch container (dev.md
# §3, stage 8.2). CONTAINER is pinned by DIGEST near the top of this file;
# a floating tag in a supply-chain-sensitive build is exactly the attack
# (dev.md §11).
#
# Overridable: CONTAINER (must keep the @sha256: pin), CONTAINER_RUNTIME,
# MUSL_OUT, MUSL_CC, MUSL_CXX, MUSL_PACKAGES. See scripts/musl-build.sh.

CONTAINER_RUNTIME ?=

.PHONY: musl
musl:
	CONTAINER="$(CONTAINER)" \
	CONTAINER_RUNTIME="$(CONTAINER_RUNTIME)" \
	MUSL_OUT="$(CURDIR)/$(BUILD)/musl" \
	MUSL_CC="$(MUSL_CC)" \
	MUSL_CXX="$(MUSL_CXX)" \
	MUSL_PACKAGES="$(MUSL_PACKAGES)" \
	scripts/musl-build.sh

# --- check: read-only gate (clang-tidy + tests). Formatting is `make format`
# (dev.md §3, §12.7). Nothing here rewrites the tree: the only generated file
# is build/compile_commands.json, under build/ and gitignored. A missing
# clang-tidy is a loud failure, never a silent pass (§11).

CLANG_TIDY ?= clang-tidy
COMPILE_DB := $(BUILD)/compile_commands.json

# Bug-oriented categories only, so the gate stays signal, not noise. The
# exclusions are checks whose style advice is noise here or is already owned by
# -Werror; every remaining finding was fixed, not silenced:
#   bugprone-easily-swappable-parameters (every all-`(int,int)` callback),
#   bugprone-exception-escape          (fights the Result-return style),
#   bugprone-narrowing-conversions     (-Wconversion owns this),
#   bugprone-signed-bitwise            (byte/flag work: `c & 0x0F` is int-promoted),
#   bugprone-unchecked-optional-access (optional is checked before .value()),
#   bugprone-unused-return-value       ([[nodiscard]] on Result does this),
#   performance-enum-size              (micro; the enums are API types),
#   performance-no-automatic-move      (deliberate `const` locals; copy != bug),
#   performance-unnecessary-copy-init  (intentional lifetime/`shared_ptr` keeps),
#   performance-unnecessary-value-param (FTXUI/`std::function` are by-value APIs),
#   performance-prefer-single-char-overloads, performance-faster-string-find
#                                       (micro `+= "\n"` style),
#   clang-analyzer-optin.performance.Padding (struct layout hint, not a bug).
TIDY_CHECKS := -*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-exception-escape,-bugprone-narrowing-conversions,-bugprone-signed-bitwise,-bugprone-unchecked-optional-access,-bugprone-unused-return-value,performance-*,-performance-enum-size,-performance-no-automatic-move,-performance-unnecessary-copy-initialization,-performance-unnecessary-value-param,-performance-prefer-single-char-overloads,-performance-faster-string-find,clang-analyzer-*,-clang-analyzer-optin.performance.Padding,modernize-use-nullptr,modernize-use-override,modernize-use-bool-literals,modernize-make-unique

TIDY_FILES := $(SRC) $(LIB_SRC)

.PHONY: compile-db
compile-db: $(COMPILE_DB)

$(COMPILE_DB): $(VERSION_H) $(GEN_TRUST_CPP) Makefile scripts/gen-compile-db.py
	@mkdir -p $(@D)
	python3 scripts/gen-compile-db.py --cxx "$(CXX)" --cwd "$(CURDIR)" --flags "$(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS)" --out $@ $(SRC) $(LIB_SRC)

.PHONY: check
check:
	@command -v $(CLANG_TIDY) >/dev/null 2>&1 || { echo >&2 "berthpanel: make check: clang-tidy not found on PATH; refusing a silent pass (dev.md §11)"; exit 1; }
	@command -v python3 >/dev/null 2>&1 || { echo >&2 "berthpanel: make check: python3 is required to generate compile_commands.json"; exit 1; }
	$(MAKE) $(COMPILE_DB)
	$(CLANG_TIDY) -p $(BUILD) --quiet --warnings-as-errors='*' --checks='$(TIDY_CHECKS)' $(TIDY_FILES)
	python3 scripts/comment-lint.py
	$(MAKE) test

# --- format: clang-format rewrites; deliberately NOT a CI gate (dev.md §12.7) ---

FORMAT_FILES := $(SRC) $(LIB_SRC) $(TEST_SRC)

.PHONY: format
format:
	clang-format -i $(FORMAT_FILES)

# --- repro: local reproducibility check (dev.md §3, stage 8.1) ---
# Builds the release artifact twice from clean and requires byte-identical
# output, then asserts no absolute build-dir path leaked in. `make check`
# stays read-only; this target only writes under build/.

.PHONY: repro
repro:
	scripts/repro-check.sh

# --- clean ---

.PHONY: clean
clean:
	rm -rf $(BUILD)
