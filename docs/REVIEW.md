# Review

Standard operating procedure for reviewing a commit touching this
repository — someone else's patch, or your own before handing it to a human
maintainer — whether the review is done by a human or an AI agent.

This pairs with `vendor/mainline/docs/review.md` (relative to the AOSP tree
root): that document is the general review philosophy and Gerrit vote
cheatsheet for LineageOS mainline device repositories; this one
operationalizes the same checks specifically for `hardware/mainline/common`.

Go through all seven checks below for every commit; report every issue you
find in one pass instead of one round-trip per issue.

## 1. Code style and quality

- Complies with `docs/CODE_STYLE.md`: language conventions, error handling,
  formatting, SPDX header.
- Consistent with the style already used in the surrounding file and
  component, even where `docs/CODE_STYLE.md` doesn't spell it out.
- The formatter was actually run on the touched files (`docs/CODE_STYLE.md`
  → Formatting tools), not just eyeballed.

## 2. Consistency

Consistency cuts across the whole change, not just source formatting:

- **Documentation**: if behavior, naming, properties, or an invariant
  changes, the relevant `AGENTS.md`/`README.md` is updated in the same
  commit instead of left to drift (`docs/CODE_STYLE.md` → Documentation).
  Terminology in the docs matches what the code actually calls things, and
  cross-references between docs still point somewhere real.
- **Code behavior**: the same kind of input/situation is handled the same
  way across the component — don't fix one call path while leaving a
  symmetric one behaving differently, and don't add a special case that
  re-decides something already decided once elsewhere. Sibling
  implementations (other backends, other APEX flavors, per
  `docs/NAMING_CONVENTIONS.md`) should follow the same conventions unless a
  deviation is justified and, ideally, documented.
- **Commit message vs. diff**: the message describes what the diff actually
  does — no more, no less.
- Per `vendor/mainline/docs/review.md`, inconsistent style compared to
  other same/similar code or commit messages in the same project is itself
  a `Code-Review -1`.

## 3. Core principle of the affected component

- Re-read the component's own `AGENTS.md` (its invariants / hard rules) and,
  if present, `INITIAL_IMPLEMENTATION.md`'s `## Design` section.
- Reject anything that contradicts a stated invariant or the original
  design goal (e.g. "the frontend must not contain backend specific logic").
- A change that legitimately needs to revise an invariant must update that
  `AGENTS.md` in the same commit.

## 4. Other users' use cases

One device/config improving is not enough; check nothing else regresses:

- Does it hardcode something that used to be derived at runtime or
  configurable (see `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md` → Design
  principles)?
- Does it change a default, a property's meaning, or an on-disk/ABI format
  that other devices or configs already depend on?
- Does it only handle the reporter's own hardware at the expense of other
  hardware already supported elsewhere in the same component?
- If it does affect someone else, is there a workaround? Breaking another
  use case with no workaround is a hard blocker either way.

## 5. Correctness

- The change does what the commit message claims, not just something
  adjacent to it.
- Edge cases (missing hardware, empty lists, I/O and permission failures)
  are handled, not only the common path.
- No shortcuts that only work by accident: undefined behavior, unstated
  ordering assumptions, or a race that merely "usually" doesn't fire.

## 6. Testing by affected users

An AI agent must not compile or run tests itself (`docs/WORKFLOW.md`), so it
cannot verify a change that way either — verify by evidence instead:

- Does the commit message, or the contributor, state what was tested, on
  which hardware/config, and with what result?
- If the risk is concentrated on hardware/configs the reviewer doesn't have
  access to, was it actually tested by someone who does?
- No such evidence: ask for it. Do not approve or merge on faith.

## 7. Clarity and maintainability

- A human unfamiliar with this specific change can follow it from the
  commit message and diff alone.
- Commit message follows `docs/COMMIT_CONVENTIONS.md`, stays well under the
  ~38-line guidance in `vendor/mainline/docs/review.md` (excluding code
  snippets and trailers), and doesn't spend most of it explaining the
  obvious.
- One logical change per commit; split unrelated changes instead of
  bundling them.
- No unrelated/unnecessary diff noise (stray whitespace, reformatting
  untouched lines).

## Outcome

- All seven checks pass: approve.
- A failing check is a quick, well-understood fix: ask for that specific
  change, not a vague "needs work".
- Check 3 or 4 fails in a way that isn't a quick fix, or check 6 can't be
  satisfied: do not approve — this is the same bar as a `Code-Review -2` in
  `vendor/mainline/docs/review.md`'s cheatsheet.
