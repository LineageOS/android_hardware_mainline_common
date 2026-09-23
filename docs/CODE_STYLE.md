# Code Style

Applies to every component in this repository unless its own `AGENTS.md` or
`INITIAL_IMPLEMENTATION.md` says otherwise. Written for both AI agents and
human contributors.

## Language

C++ is preferred, since it's what most of this repository is written in and
what most of the AOSP reference HAL implementations use, but it is not a hard
requirement — pick whatever language best fits the component (this
repository already carries a root `rustfmt.toml` for Rust). The C++-specific
rules below apply when you do use C++; use the equivalent idiomatic
conventions of whatever language you pick otherwise (e.g. `Result`/`Option`
and no `panic!()` on recoverable errors in Rust).

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

## Copyright header

Every new source file, in any language, starts with the SPDX license header;
copy it from an existing file in the same directory rather than retyping it.
For a directory's first file(s), use:

`Android.bp`:
```
//
// SPDX-FileCopyrightText: The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
//
```

Source files:
```
/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
```

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
and `AGENTS.md` in sync with behavior, property, or ABI changes you make.
