BUILD_DIR  ?= build
BUILD_TYPE ?= Release
GENERATOR  ?= Ninja
JOBS       ?=
CMAKE_ARGS ?=

# Plugins are off by default in CMake, but a normal dev build wants them.
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DPLUGINS_ALL=ON $(CMAKE_ARGS)

ifeq ($(JOBS),)
BUILD_FLAGS := --parallel
else
BUILD_FLAGS := --parallel $(JOBS)
endif

STAMP := $(BUILD_DIR)/CMakeCache.txt

.PHONY: all build configure reconfigure run ui clean distclean install uninstall deps test help

all: build

$(STAMP):
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_FLAGS)

configure: $(STAMP)

# Re-run configure even if the cache already exists, to pick up changed options.
reconfigure:
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) $(BUILD_FLAGS)

run: build
	cd $(BUILD_DIR) && ./gobdump

ui: build
	cd $(BUILD_DIR) && ./gobdump-ui

test: configure
	cmake --build $(BUILD_DIR) $(BUILD_FLAGS)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

install: build
	cmake --install $(BUILD_DIR)

uninstall: configure
	cmake --build $(BUILD_DIR) --target uninstall

clean:
	@if [ -f "$(STAMP)" ]; then cmake --build $(BUILD_DIR) --target clean; \
	else echo "Nothing to clean, $(BUILD_DIR) is not configured."; fi

distclean:
	@if [ -f "$(STAMP)" ]; then cmake --build $(BUILD_DIR) --target distclean; \
	else echo "Nothing to clean, $(BUILD_DIR) is not configured."; fi

deps:
	./windows/Setup-MSYS2.sh

help:
	@echo "Targets:"
	@echo "  build       configure if needed, then build   (default)"
	@echo "  run         build, then run the CLI"
	@echo "  ui          build, then run the GUI"
	@echo "  test        build, then run ctest"
	@echo "  install     install to CMAKE_INSTALL_PREFIX"
	@echo "  uninstall   remove installed files"
	@echo "  clean       remove build outputs, keep the cache"
	@echo "  distclean   empty $(BUILD_DIR) entirely"
	@echo "  deps        install dependencies (MSYS2 only)"
	@echo "  reconfigure re-run cmake configure"
	@echo
	@echo "Variables: BUILD_DIR=$(BUILD_DIR) BUILD_TYPE=$(BUILD_TYPE) GENERATOR=$(GENERATOR) JOBS=$(JOBS)"
	@echo "Example:   make BUILD_TYPE=Debug BUILD_DIR=build-debug"
