# Code Style

Applies to every component in this repository unless its own `AGENTS.md` or
`INITIAL_IMPLEMENTATION.md` says otherwise.

## C++

- Google C++ Style Guide naming: `CamelCase` types and functions,
  `snake_case_` member variables (trailing underscore), `kCamelCase`
  constants.
- No exceptions: `try`/`catch` is forbidden. Report failures through the
  interface's native error type instead (`ndk::ScopedAStatus`, `status_t`,
  `RetCode`, or `std::optional`, depending on the API you're implementing).
- Prefer the C++ standard library and `libbase`
  (`system/libbase/include/android-base`) over raw C APIs for logging,
  properties, string parsing (`android::base::Parse*`), and file descriptors
  (`unique_fd`).
- Every new source file starts with the SPDX license header; copy it from an
  existing file in the same directory rather than retyping it.

## Formatting tools

Run a formatter on the files you actually touched before committing:

- C++: `clang-format -i --style=file <files>` using the repository's
  `.clang-format` (`hardware/mainline/common/.clang-format`), or the
  prebuilt toolchain binary if available, e.g.
  `prebuilts/clang/host/linux-x86/clang-r*/bin/clang-format`.
- Rust: `rustfmt <files>` using the repository's `rustfmt.toml`
  (`hardware/mainline/common/rustfmt.toml`).

Only format the files you changed. Do not go searching the tree for other
formatters or linters to run, and do not reformat unrelated files.

## Documentation

Keep the relevant `README.md` (including any per-backend/per-module README)
in sync with behavior, property, or ABI changes you make.
