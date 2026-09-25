# Scope

Applies whenever you're handed a specific bug to fix, or asked to build a
new component from scratch. Written for both AI agents and human
contributors.

## The failure mode this addresses

A fix that only touches the exact call site, device, or config the report
happened to describe, while the identical logic (or the same missing check)
sits untouched somewhere else in the same component, isn't really a fix —
it just narrows "broken for everyone" down to "broken for everyone else".
The equivalent mistake when building something new is designing narrowly
around today's one device/config instead of the general case; that half is
already covered by `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md` → Design
principles. This document is the same instinct applied to changes to
existing code too, and to how you get there in both cases.

## Before writing the fix

- Restate the report as a rule ("X must never be true when Y"), not as the
  one repro path it was described through. A rule tells you every place to
  check; a single repro path only tells you where to look once.
- Read the component for every place that rule can currently be violated,
  not only the spot the report walked through — the same helper called from
  more than one place, the same condition duplicated per device role /
  card / backend, the same property read in more than one file.
- For a new component, check how sibling implementations in this repository
  (other backends of the same domain, other HALs under `interfaces/`)
  already handle the same kind of situation, per
  `docs/NAMING_CONVENTIONS.md`, before designing this one's — don't
  rediscover a narrower version of something already solved elsewhere.

## While writing it

- Ask whether the reported instance is representative of a bigger category
  (a specific device standing in for "any device without a dedicated
  speaker", one format standing in for "any format outside the augmented
  set") or is genuinely the only case there is. Fix the category when it
  is representative, not just the sample.
- Prefer the generic fix over a special case that matches only the exact
  thing in the report. A per-device/per-property `if` bolted on next to
  existing generic logic is usually the sign the actual root cause was
  never addressed.
- This applies equally when writing new code: prefer deriving behavior at
  runtime or making it configurable over hardcoding for the one target
  you happen to have in front of you
  (`docs/INITIAL_IMPLEMENTATION_GUIDELINES.md` → Design principles).

## When the same issue turns up more than once

- Fixing every occurrence in the same commit is fine when they share one
  root cause and the diff stays reviewable (`docs/REVIEW.md` → Clarity);
  list each fixed spot in the commit body so the reviewer isn't left to
  rediscover them.
- If the full fix is large enough to bury the original request, or reaches
  outside what was actually asked, say so and ask before doing it, instead
  of silently expanding scope (`docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`
  → "when very unsure, ask").
- If you noticed the same bug elsewhere but aren't fixing it now (told not
  to, or it's out of scope for this change), say that explicitly in the
  commit body or your reply — a future reader shouldn't have to guess
  whether it was missed or knowingly deferred.

## Self-review

`docs/REVIEW.md` checks 2 (Consistency: "don't fix one call path while
leaving a symmetric one behaving differently") and 4 (other users' use
cases) exist to catch exactly this if it slips through. Run them against
your own diff, not only someone else's, before presenting it.
