# Commit Conventions

Applies to every commit in this repository.

Wrap every paragraph so no line exceeds 72 characters. This does not apply to
code snippets, URLs, or other content that should not be wrapped.

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

A well-known leading path segment may also be replaced with its documented
alias below, to shorten the subject further without losing meaning:

| Segment      | Alias  |
|--------------|--------|
| `interfaces` | `intf` |

For example, `interfaces/audio/mainline` may be written as
`intf/audio/mainline`. Only use an alias from this table — don't invent a
one-off abbreviation, so commit subjects stay searchable and consistent (see
`docs/REVIEW.md` → Consistency). Add a new row here first if another segment
is worth shortening the same way.

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

## Applying the message

Write the whole subject/body/trailers as a single piece of text and hand it
to git as-is, instead of reconstructing it through several `-m` flags:

```
git commit -F - <<'EOF'
mainline/common: <path>: <imperative summary>

<body, one or more paragraphs>

Assisted-by: LLM
Assisted-by: <Agent>:<Model ID>
EOF
```

(a plain file works the same way: `git commit -F <path/to/message-file>`.)

- Quote the heredoc delimiter (`<<'EOF'`, not `<<EOF`): commit bodies
  routinely quote identifiers in backticks, e.g. `` `AGENTS.md` `` or
  `` `docs/REVIEW.md` ``; an unquoted heredoc (or a double-quoted `-m`
  string) lets the shell expand those as command substitutions instead of
  passing them through literally. The same goes for `$` and `!` (history
  expansion in interactive shells).
- `git commit -m "subject" -m "paragraph one" -m "paragraph two"` puts each
  `-m` in its own paragraph (blank-line separated), which is fine for a
  short single-paragraph body, but gets unwieldy once there's a
  multi-paragraph body plus trailers — the trailers need their own blank
  line before them too, i.e. yet another `-m` that's easy to forget.
  Reconstructing a several-line body this way, one shell argument per
  paragraph, is also how quoting mistakes creep in; writing it out once as
  a single block doesn't have that failure mode.
- Composing it as one block first still doesn't excuse skipping the review
  pass: check the actual result with `git log -1` / `git show` for wrapping
  and trailer placement, the same as you would after any other method.
- This is about *authoring* a not-yet-existing commit. Amending one already
  merged is never allowed regardless of method — see `docs/FIXUPS.md`. For
  one not yet pushed, `git commit --amend -F -` follows the same heredoc
  rule as above.
