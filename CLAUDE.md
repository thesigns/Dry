# Dry

`dry` is a portable console diary written in C. It stores everything in a single file encrypted with libsodium. It targets Windows (w64devkit / MinGW-w64) and Linux. macOS is out of scope.

## Build and run

```
make            # Windows → build/dry.exe (static, no DLLs); Linux → build/dry
make clean
build/dry.exe kuba
```

- **Windows:** libsodium (official MinGW package, `libsodium-win64` folder; 1.0.22 used locally) is **not** in the repo. The Makefile expects it in `third_party/libsodium` (git-ignored) or at `make SODIUM=<path>`, and links it statically with `-DSODIUM_STATIC -static`. Setup steps are in `README.md`.
- **Linux:** needs `libsodium-dev`. Flags come from `pkg-config`.
- **Linux status:** the Linux code path (`src/platform_posix.c`) is written but has never been compiled or run. Treat it as untested.
- The Makefile picks the platform from `$(OS)` (`Windows_NT`). It compiles every `src/*.c`. Each platform file wraps its whole body in `#ifdef _WIN32` / `#ifndef _WIN32`, so only one of them contributes code.
- Compiler flags are `-std=c17 -O2 -Wall -Wextra -Wpedantic`. The build must stay warning-free.

## Usage

`dry <name>` opens `<name>.dry` from the directory containing the executable. If the argument contains a path, it is used as given. `.dry` is appended only when the name has no extension.

- **Existing file:** asks for the password, with 3 attempts.
- **Missing file:** asks `Create a new one? [y/N]`, then for a new password twice.

After opening, a prompt (`dry>`, or `YYYY-MM-DD>` while writing) accepts these commands:

| Command | Meaning |
|---|---|
| `/start` | Start today's entry, or append a new timestamped segment to it |
| `/end` | Stop writing |
| `/undo` | Remove the last paragraph of today's entry; empty segments and entries are dropped |
| `/list [N]` | Last N entries (default 10): `#00001 2026-10-17 -- preview (...)` |
| `/show [N \| YYYY-MM-DD]` | Full entry; with no argument, today's entry |
| `/passwd` | Change the password: asks for the current one, then the new one twice; re-keys with a fresh salt |
| `/help` | Show help |
| `/exit` | Quit; `/quit`, EOF and Ctrl+C also quit |

While writing, each line is one paragraph and is saved immediately. A line starting with `//` stores a literal leading `/`. An unknown `/command` is never stored as text. On exit, the screen and the terminal scrollback are cleared.

## Source layout

- `src/main.c`: argument handling, path resolution, password prompts, the command loop, and all user-facing messages.
- `src/diary.c/.h`: in-memory model and (de)serialization of the plaintext payload. The model is Diary → Entry (one per date, sequential `id`) → Segment (`HH:MM`, one per `/start`) → paragraphs (UTF-8 strings).
- `src/store.c/.h`: the encrypted file format, key derivation, encryption, and atomic save.
- `src/platform.h` with `platform_win.c` and `platform_posix.c`: all OS-specific code, namely console I/O, password input without echo, UTF-8 argv, exe directory, file open/replace/sync, the lock file, the Ctrl+C handler, and `localtime`.

## Cryptography and file format

- **KDF:** Argon2id (`crypto_pwhash`, MODERATE limits) with a random 16-byte salt. The key lives in `sodium_malloc` memory.
- **Cipher:** `crypto_aead_xchacha20poly1305_ietf`. The whole plaintext is re-encrypted with a fresh random nonce on every save. The full 64-byte header is the AAD.
- **Header (little-endian):** `"DRY\x1A"` | version u8 = 1 | KDF id u8 = 1 | reserved u16 | opslimit u64 | memlimit u64 | salt[16] | nonce[24]. The ciphertext and 16-byte MAC follow.
- **Payload (`diary.c`, version byte = 1):** counts and lengths are u32 LE. Dates are stored as `YYYY-MM-DD` text and times as `HH:MM` text, both local time, so moving a diary between time zones never shifts an entry to another day.
- **Locking:** while a diary is open, `<path>.lock` is held exclusively (Windows: share mode 0 + `FILE_FLAG_DELETE_ON_CLOSE`, so the file disappears on exit; Linux: `fcntl` lock, the file stays). A second process refuses to open the diary. The lock is on a separate file because every save replaces the diary file.
- **Saving:** the file is written to `<path>.tmp`, flushed to disk, then swapped in with `MoveFileExW` or `rename`.
- **Changing the format:** any format change must bump the relevant version byte and keep existing `.dry` files readable. Never `fwrite` structs; integers are always serialized byte by byte.

## Conventions

- **Language:** code comments and all user-facing messages are in English. Diary content is arbitrary UTF-8.
- **Strings:** all strings inside the program are UTF-8. On Windows, console I/O goes through `ReadConsoleW`/`WriteConsoleW` with conversion, because `fgets` breaks on non-ASCII input. Do not use `printf` for user output; use `put()`/`out()` in `main.c`, which call `platform_write`.
- **OS-specific code** belongs only in the `platform_*` files, behind the `platform.h` interface.
- **Sensitive data:** zero plaintext buffers (input lines, serialized payload, passwords) with `sodium_memzero` before freeing them. Passwords and keys go in `sodium_malloc` memory. Avoid `realloc` on plaintext buffers.
- **Untrusted input:** treat the decrypted payload and the file header as untrusted. Bounds-check every read, and reject absurd counts and Argon2 limits.
- **Style:** 4-space indentation, opening brace on its own line for functions, `snake_case` names, and small `static` helpers.

## Testing

There is no test suite yet. Smoke-test by piping input. When stdin is not a console, the program reads plain UTF-8 lines and passwords are echoed. Use a scratch directory, not the project folder:

```
printf 'y\npw\npw\n/start\nHello\n/end\n/list\n/exit\n' | build/dry.exe /tmp/t/test
```

The following can only be verified manually in a real terminal: hidden password input, typed non-ASCII characters, Ctrl+C, and clearing the screen on exit.

## Not implemented yet

- `/export`
