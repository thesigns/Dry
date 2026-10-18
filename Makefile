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

.PHONY: all clean
