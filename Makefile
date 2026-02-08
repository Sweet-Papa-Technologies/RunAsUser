CC_MACOS    = clang
CC_WINDOWS  = x86_64-w64-mingw32-gcc

CFLAGS_COMMON = -O2 -Wall -Wextra -Werror

# macOS: universal binary (arm64 + x86_64), minimum deployment target 11.0
CFLAGS_MACOS  = $(CFLAGS_COMMON) -arch arm64 -arch x86_64 -mmacosx-version-min=11.0
LDFLAGS_MACOS = -framework SystemConfiguration -framework CoreFoundation

# Windows: static CRT (no vcredist dependency), link required system libs
CFLAGS_WINDOWS  = $(CFLAGS_COMMON) -static
LDFLAGS_WINDOWS = -lwtsapi32 -luserenv -ladvapi32

BUILD_DIR = build

.PHONY: all macos windows clean

all: macos windows

macos: $(BUILD_DIR)/runasuser
windows: $(BUILD_DIR)/runasuser.exe

$(BUILD_DIR)/runasuser: src/macos/main.c | $(BUILD_DIR)
	$(CC_MACOS) $(CFLAGS_MACOS) -o $@ $< $(LDFLAGS_MACOS)
	@echo "Built: $@ (universal binary)"
	@file $@

$(BUILD_DIR)/runasuser.exe: src/windows/main.c | $(BUILD_DIR)
	$(CC_WINDOWS) $(CFLAGS_WINDOWS) -o $@ $< $(LDFLAGS_WINDOWS)
	@echo "Built: $@ (static Windows binary)"
	@file $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
