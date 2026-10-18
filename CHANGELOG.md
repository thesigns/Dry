# Changelog

All notable changes to dry are listed here. Versions follow [Semantic Versioning](https://semver.org).

## [Unreleased]

- `/show` wraps paragraphs at spaces to 75 characters and separates paragraphs with a blank line. A word longer than that (e.g. a URL) stays whole.

## [0.1.0] – 2026-10-18

First release.

- Single-file diary encrypted with libsodium (Argon2id + XChaCha20-Poly1305), saved atomically after every paragraph.
- Commands: `/start`, `/end`, `/undo`, `/list`, `/show`, `/find`, `/export`, `/passwd`, `/version`, `/help`, `/exit`.
- `/find` searches case-insensitively, Polish letters included, and lists matching days.
- `/export` writes an unencrypted Markdown file after a warning.
- A diary can be open in only one process at a time.
- `dry --version` prints the version.
- Windows build is static (no DLLs). The Linux code path has not been compiled yet.
