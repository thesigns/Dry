# dry – encrypted diary
#   Windows (w64devkit): make  → build/dry.exe (libsodium linked statically, see README.md)
#                        make SODIUM=C:/path/to/libsodium-win64  – custom libsodium location
#   Linux:               make  → build/dry     (requires the libsodium-dev package)

CC     = gcc
CFLAGS = -std=c17 -O2 -Wall -Wextra -Wpedantic
SRC    = $(wildcard src/*.c)
OBJ    = $(patsubst src/%.c,build/%.o,$(SRC))
HDR    = $(wildcard src/*.h)

ifeq ($(OS),Windows_NT)
  TARGET  = build/dry.exe
  SODIUM ?= third_party/libsodium
  ifeq ($(filter clean,$(MAKECMDGOALS)),)
    ifeq ($(wildcard $(SODIUM)/lib/libsodium.a),)
      $(error libsodium not found in "$(SODIUM)" – see README.md)
    endif
  endif
  CFLAGS += -I$(SODIUM)/include -DSODIUM_STATIC
  LDFLAGS = -static
  LDLIBS  = $(SODIUM)/lib/libsodium.a
else
  TARGET  = build/dry
  CFLAGS += $(shell pkg-config --cflags libsodium)
  LDLIBS  = $(shell pkg-config --libs libsodium)
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -f build/*.o $(TARGET)

# Release archive: make dist → build/dry-<version>-windows-x64.zip (Linux: .tar.gz)
VERSION  := $(shell sed -n 's/.*DRY_VERSION "\(.*\)".*/\1/p' src/version.h)
DOCS      = README.md LICENSE CHANGELOG.md
ifeq ($(OS),Windows_NT)
  DIST    = dry-$(VERSION)-windows-x64
  ARCHIVE = build/$(DIST).zip
else
  DIST    = dry-$(VERSION)-linux-$(shell uname -m)
  ARCHIVE = build/$(DIST).tar.gz
endif

dist: $(TARGET) $(DOCS)
	rm -rf build/dist $(ARCHIVE)
	mkdir -p build/dist/$(DIST)
	cp $(TARGET) $(DOCS) build/dist/$(DIST)/
ifeq ($(OS),Windows_NT)
	cd build/dist && zip -qr ../$(DIST).zip $(DIST)
else
	tar -C build/dist -czf $(ARCHIVE) $(DIST)
endif
	rm -rf build/dist
	@echo "Created $(ARCHIVE)"

.PHONY: all clean dist
