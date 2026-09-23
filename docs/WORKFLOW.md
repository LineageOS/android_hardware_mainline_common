# Workflow

Applies to every component in this repository unless its own `AGENTS.md` or
`INITIAL_IMPLEMENTATION.md` says otherwise.

- Do not start a build, compile, run tests, flash, or deploy anything. The
  human maintainer builds and reports back results and errors.
- Running a formatter (`clang-format`, `rustfmt`) on the files you touched is
  expected and allowed; see `docs/CODE_STYLE.md`. This is not "starting a
  build."
- Do not browse outside the AOSP source tree for reference material.
- Before presenting a commit to a human maintainer, or when asked to review
  someone else's commit, follow `docs/REVIEW.md`.
- If a directory contains `INITIAL_IMPLEMENTATION.md`, treat it as the
  authoritative source for that component's one-time bring-up constraints
  (reference paths, prohibited actions, review checklist). Do not edit it,
  and follow it over this document when it is more specific.
