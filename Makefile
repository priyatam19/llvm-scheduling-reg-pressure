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

# Clean rule
clean:
	rm -rf $(BUILD_DIR)

.PHONY: clean