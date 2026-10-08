# berthpanel — local-first deploy panel for curated self-hosted apps.
# Hand-written build, no CMake/Meson (docs/dev.md §3).
#
# Targets: make / debug, release, asan, test, format, clean.
# Overridable: CXX CXXSTD WARNFLAGS OPTFLAGS PREFIX DESTDIR CONTAINER.

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
CONTAINER ?= ghcr.io/archlinux/archlinux@sha256:DIGEST_PINNED_IN_STAGE_8.2

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
 	src/berth/recipes.cpp \
 	src/berth/userrecipes.cpp \
 	src/berth/detect.cpp \
 	src/berth/catalog.cpp \
	src/berth/deploy.cpp \
	src/berth/service_ops.cpp \
	src/util/fs.cpp \
	src/util/tmpdir.cpp \
	src/util/which.cpp \
	src/util/gzip.cpp \
	src/util/subprocess.cpp \
	src/tui/chrome.cpp \
	src/tui/view_catalog.cpp \
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
	tests/recipes_test.cpp \
 	tests/catalog_test.cpp \
 	tests/userrecipes_test.cpp \
 	tests/detect_test.cpp \
 	tests/deploy_test.cpp \
 	tests/cancel_test.cpp \
 	tests/gzip_test.cpp \
	tests/ring_test.cpp \
	tests/tui_view_test.cpp

INCLUDES := -Iinclude -Isrc -I$(BUILD)/generated -isystem third_party -isystem third_party/mbedtls/include -isystem third_party/ftxui/include -isystem third_party/ftxui/src
LDLIBS   := -lz -pthread
DEPFLAGS := -MMD -MP

# Reproducible release (dev.md §3): fixed epoch, no absolute build paths in
# the binary, stable random seed, deterministic build-id. The release script
# exports the real SOURCE_DATE_EPOCH; the default keeps bare `make release`
# stable too.
export SOURCE_DATE_EPOCH ?= 0
REPROFLAGS := -ffile-prefix-map=$(CURDIR)=. \
              -fdebug-prefix-map=$(CURDIR)=. \
              -frandom-seed=berthpanel \
              -Wl,--build-id=sha1

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
	$$(CXX) $$($(1)_FLAGS) $(REPROFLAGS) $$(filter %.o,$$^) $(LDLIBS) -o $$@

$$($(1)_DIR)/obj $$($(1)_DIR):
	mkdir -p $$@

-include $$($(1)_OBJ:.o=.d) $$($(1)_VOBJ:.o=.d) $$($(1)_VCXXOBJ:.o=.d) $$($(1)_GOBJ:.o=.d)
endef

$(eval $(call mode_template,debug,$(OPTFLAGS),-O2))
$(eval $(call mode_template,release,-O2 -DNDEBUG $(REPROFLAGS),-O2))
$(eval $(call mode_template,asan,$(OPTFLAGS) -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer,-O2 -fsanitize=address -fsanitize=undefined))

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

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_CASE_OBJ): $(TEST_DIR)/obj/%.o: tests/%.cpp $(VERSION_H) | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_LIB_OBJ): $(TEST_DIR)/obj/%.o: src/%.cpp $(VERSION_H) | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_VENDOR_OBJ): $(TEST_DIR)/obj/%.o: third_party/%.c | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CC) -O2 -Ithird_party/mbedtls/include $(DEPFLAGS) -c $< -o $@

$(TEST_TRUST_OBJ): $(TEST_DIR)/obj/%.o: $(GEN_TRUST_CPP) | $(TEST_DIR)/obj
	@mkdir -p $(@D)
	$(CXX) $(INCLUDES) $(CXXSTD) $(WARNFLAGS) $(OPTFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJ) $(TEST_VENDOR_OBJ) $(TEST_TRUST_OBJ) | $(TEST_DIR)
	$(CXX) $(filter %.o,$^) $(LDLIBS) -o $@

$(TEST_DIR)/obj $(TEST_DIR):
	mkdir -p $@

-include $(TEST_OBJ:.o=.d) $(TEST_VENDOR_OBJ:.o=.d) $(TEST_TRUST_OBJ:.o=.d)

# --- format: clang-format rewrites; deliberately NOT a CI gate (dev.md §12.7) ---

FORMAT_FILES := $(SRC) $(LIB_SRC) $(TEST_SRC)

.PHONY: format
format:
	clang-format -i $(FORMAT_FILES)

# --- clean ---

.PHONY: clean
clean:
	rm -rf $(BUILD)
