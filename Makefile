CXX      := clang++
CXXFLAGS := $(shell llvm-config --cxxflags)
LDFLAGS  := $(shell llvm-config --ldflags)
LIBS     := $(shell llvm-config --libs)

# Directories
SRC_DIR     := src
INCLUDE_DIR := include
BUILD_DIR   := build

TARGET := $(BUILD_DIR)/schedulerPass.so

SRCS := src/schedulerPass.cpp

# Build Rule
$(TARGET): $(SRCS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) \
		-shared -fPIC \
		$(LDFLAGS) $(LIBS) \
		-o $(TARGET) $(SRCS)

# Register allocator driver: a small llc-equivalent binary, not a plugin --
# llc has no runtime-loadable extension point for register allocators (see
# src/schedRegAllocDriver.cpp for why). Executable, so no -shared -fPIC.
DRIVER_TARGET := $(BUILD_DIR)/schedRegAllocDriver
DRIVER_SRCS   := src/schedRegAllocDriver.cpp

$(DRIVER_TARGET): $(DRIVER_SRCS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -fuse-ld=lld \
		$(DRIVER_SRCS) \
		$(LDFLAGS) $(LIBS) -lzstd -lz \
		-o $(DRIVER_TARGET)

# NOTE: $(TARGET) above stays make's default goal (bare `make`, as used by
# ir-tests.yml and this repo's own docs, must keep building only the plugin).
# `all` is opt-in -- run it (or test-driver-parity, which depends on both
# targets directly) explicitly to also build the driver.
all: $(TARGET) $(DRIVER_TARGET)

# Clean rule
clean:
	rm -rf $(BUILD_DIR)

test: $(TARGET)
	bash tests/run_ir_tests.sh

test-driver-parity: $(TARGET) $(DRIVER_TARGET)
	bash tests/run_driver_parity.sh

.PHONY: clean test test-driver-parity all
