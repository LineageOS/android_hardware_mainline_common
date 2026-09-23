# AGENTS.md - hardware/mainline/common

This repository hosts a collection of independent HAL and utility
implementations for devices running a mainline Linux kernel. Each component
lives under its own directory (mostly `interfaces/<domain>/<name>/`, plus a
few standalone tools such as `grub/`, `bdaddr/`, `tablet2multitouch/`). Most
of them have their own `AGENTS.md` with module-specific guidance
(architecture, invariants, build targets, properties) — read that file first
when working inside such a directory.

## Shared standards live in `docs/`

Repository-wide conventions that apply to every component are split by topic
under `docs/`, both for AI agents and for human contributors, so you only
have to read what's relevant to the task at hand:

- `docs/CODE_STYLE.md` — language style, error handling, formatting tools.
- `docs/COMMIT_CONVENTIONS.md` — commit subject/body/trailer format.
- `docs/WORKFLOW.md` — what an AI agent may and may not do (builds, tests,
  references).
- `docs/NAMING_CONVENTIONS.md` — executable, service, APEX, log tag, and
  property naming patterns for a new component.
- `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md` — shared requirements,
  references, and guidelines for bringing up a brand-new component.
- `docs/REVIEW.md` — the SOP for reviewing a contributed commit, whether
  it's someone else's patch or your own before it goes to a human.

Read the relevant doc(s) above before writing code or crafting a commit
message. A directory's own `AGENTS.md` and `INITIAL_IMPLEMENTATION.md` (where
present) take precedence over `docs/` when they say something more specific
or different for that component.
