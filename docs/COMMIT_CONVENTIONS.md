# Commit Conventions

Applies to every commit in this repository.

## Subject

```
mainline/common: <path>: <imperative summary>
```

`<path>` is the *shortest* path that still unambiguously identifies the
component — trim leading directories that add no disambiguation. For example
use `grub_boot_control`, not `grub/grub_boot_control` (no other component is
named `grub_boot_control`), but use `interfaces/audio/mainline` in full,
since `mainline` alone is ambiguous with `interfaces/vibrator/mainline` and
`interfaces/sensors/mainline`. Check the target directory's `AGENTS.md` if
you're unsure of the exact prefix it uses.

## Body

Explain what changed and why, in full sentences. Wrap normally. Multiple
paragraphs are fine for non-trivial changes.

## Trailers

End every commit message with both of these trailers, in this order:

```
Assisted-by: LLM
Assisted-by: <Agent>:<Model ID>
```

- `Assisted-by: LLM` is mandatory on every commit produced with AI
  assistance, per the LineageOS rule requiring it.
- `Assisted-by: <Agent>:<Model ID>` identifies the specific tool and model,
  e.g. `Assisted-by: OpenCode:anthropic/claude-sonnet-5`.

`Change-Id` is appended automatically by the local `commit-msg` hook; do not
add it by hand.
