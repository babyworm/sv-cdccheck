# sv-cdccheck Makefile — convenience wrapper around CMake
# Usage: make deps && make build

BUILD_DIR     := build
BUILD_DIR_DBG := build-debug
INSTALL_PREFIX ?= $(HOME)/.local
JOBS          ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

CMAKE_COMMON  := -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
CMAKE_RELEASE := $(CMAKE_COMMON) -DCMAKE_BUILD_TYPE=Release
CMAKE_DEBUG   := $(CMAKE_COMMON) -DCMAKE_BUILD_TYPE=Debug

.PHONY: all deps build debug test install clean help rust-check

all: build

# --- Dependency fetch (triggers CMake configure which runs FetchContent) ---
deps:
	@if [ -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		cached_src=$$(grep 'CMAKE_HOME_DIRECTORY' $(BUILD_DIR)/CMakeCache.txt 2>/dev/null | cut -d= -f2); \
		if [ -n "$$cached_src" ] && [ "$$cached_src" != "$$(pwd)" ]; then \
			echo "==> Stale CMake cache detected (was: $$cached_src), cleaning..."; \
			rm -rf $(BUILD_DIR); \
		fi; \
	fi
	@echo "==> Configuring + fetching dependencies (slang v10.0, fmt, Catch2)..."
	cmake -S . -B $(BUILD_DIR) $(CMAKE_RELEASE) -DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX)
	@echo "==> Dependencies ready in $(BUILD_DIR)/_deps/"

# --- Build targets ---
build: deps
	@echo "==> Building sv-cdccheck (Release, $(JOBS) jobs)..."
	cmake --build $(BUILD_DIR) -j$(JOBS)

debug:
	@echo "==> Configuring debug build..."
	cmake -S . -B $(BUILD_DIR_DBG) $(CMAKE_DEBUG)
	@echo "==> Building sv-cdccheck (Debug, $(JOBS) jobs)..."
	cmake --build $(BUILD_DIR_DBG) -j$(JOBS)

# --- Test ---
test: build
	@echo "==> Running tests..."
	cd $(BUILD_DIR) && ctest --output-on-failure

# --- Install ---
install: build
	@echo "==> Installing to $(INSTALL_PREFIX)/bin/sv-cdccheck..."
	cmake --install $(BUILD_DIR) --prefix $(INSTALL_PREFIX) --strip

# --- Clean ---
clean:
	@echo "==> Cleaning build directories..."
	rm -rf $(BUILD_DIR) $(BUILD_DIR_DBG)

# --- Rust experimental track ---
rust-check:
	@echo "==> Checking slang-rs availability..."
	@command -v cargo >/dev/null 2>&1 || { echo "ERROR: cargo not found"; exit 1; }
	@echo "--- crates.io search ---"
	cargo search slang-rs 2>/dev/null || echo "(search requires network)"
	cargo search svlang-sys 2>/dev/null || echo "(search requires network)"
	@echo ""
	@echo "Available Rust options for slang integration:"
	@echo "  slang-rs     — Official Rust bindings (xlsynth/slang-rs)"
	@echo "  svlang-sys   — Low-level FFI bindings"
	@echo "  sv-parser    — Pure Rust SV parser (no elaboration)"
	@echo ""
	@echo "To experiment: cargo init --name sv-cdccheck-rs experiments/rust"
	@echo "               cd experiments/rust && cargo add slang-rs"

# --- Help ---
help:
	@echo "sv-cdccheck build targets:"
	@echo "  make deps        Fetch slang + dependencies via CMake FetchContent"
	@echo "  make build       Release build (default)"
	@echo "  make debug       Debug build"
	@echo "  make test        Run test suite"
	@echo "  make install     Install to ~/.local/bin (override: INSTALL_PREFIX=...)"
	@echo "  make clean       Remove build directories"
	@echo "  make rust-check  Check Rust/slang-rs availability"
	@echo ""
	@echo "Variables:"
	@echo "  JOBS=N           Parallel build jobs (default: auto-detect)"
	@echo "  INSTALL_PREFIX   Install path (default: ~/.local)"
