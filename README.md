# dry

A small, portable console diary in C. The whole diary lives in a single file encrypted with [libsodium](https://libsodium.org): Argon2id derives the key from your password, and XChaCha20-Poly1305 encrypts and authenticates the contents.

```
> dry kuba
Password:
Diary kuba opened (entries: 2). Help: /help
dry> /list
#00001 2026-10-17 -- Felt a bit down today, maybe I need (...)
#00002 2026-10-18 -- Started writing a program called dry (...)
dry> /start
Appending to entry #00002 from 2026-10-18. Enter ends a paragraph, /end stops writing.
2026-10-18> It works!
2026-10-18> /end
dry> /exit
```

## Usage

`dry <name>` opens `<name>.dry` from the directory containing the program. If the file does not exist, dry offers to create it. You can also pass a full path.

| Command | Meaning |
|---|---|
| `/start` | Start today's entry, or continue it (adds a new timestamped section) |
| `/end` | Stop writing |
| `/undo` | Remove the last paragraph of today's entry |
| `/list [N]` | Show the last N entries (default 10) |
| `/show [N \| YYYY-MM-DD]` | Show a full entry; with no argument, shows today's |
| `/help` | Show help |
| `/exit` | Quit |

While writing, each line you enter is one paragraph and is saved to disk immediately. To start a paragraph with `/`, type `//`.

There is no password recovery. If you forget the password, the diary cannot be read.

## Building

dry needs **libsodium**, which is not included in this repository.

### Windows (w64devkit / MinGW-w64)

1. Download the MinGW build of libsodium, for example `libsodium-1.0.22-mingw.tar.gz` from <https://download.libsodium.org/libsodium/releases/>.
2. Copy the contents of its `libsodium-win64` folder so that you end up with:
   ```
   third_party/libsodium/include/sodium.h
   third_party/libsodium/lib/libsodium.a
   ```
   Alternatively, keep libsodium anywhere you like and pass its location: `make SODIUM=C:/libs/libsodium-win64`.
3. Run `make`.

The result is `build/dry.exe`, a single statically linked executable with no DLLs. Copy it wherever you want to keep your diaries.

### Linux

```
sudo apt install build-essential libsodium-dev pkg-config
make
```

The result is `build/dry`. The Linux build has not been tested yet.

## License

dry is released under the [MIT License](LICENSE). Copyright (c) 2026 Jakub W. Adamczyk.

libsodium is a separate project distributed under the ISC license.
