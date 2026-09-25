# Scope

Applies whenever you're handed a specific bug to fix or asked to implement
something new. Written for both AI agents and human contributors.

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

The examples in a request demonstrate the requirement; they do not
necessarily define its full boundary. Phrases such as "and similar cases"
explicitly require identifying the shared rule rather than copying the
listed cases into the implementation.

## Before writing code

- Restate the report as a rule ("X must never be true when Y"), not as the
  one repro path it was described through. A rule tells you every place to
  check; a single repro path only tells you where to look once.
- Read the component for every place that rule can currently be violated,
  not only the spot the report walked through — the same helper called from
  more than one place, the same condition duplicated per device role /
  card / backend, the same property read in more than one file.
- Define the acceptance surface before coding. List the relevant input
  categories, entry points, supported configurations, and expected valid,
  invalid, boundary, and unavailable/error cases. Include both behavior that
  must occur and behavior that must not occur. This list is a design aid,
  not necessarily something to paste into the final commit message.
- Inspect the existing target corpus, not just the examples in the request.
  The implementation must account for every applicable established form and
  entry path already present. Keep the search inside the relevant
  component/tree rather than searching all of AOSP broadly.
- When the request names a reference implementation, inspect it end to end:
  its entry point, integration, core behavior, validation, supporting files,
  and failure handling. Extract the underlying pattern and adapt it to this
  repository; do not copy only the most visible file, and do not copy details
  that are specific to the reference's environment.
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
- Base applicability on the real category, not a closed list copied from
  today's examples, unless that closed list is an explicit requirement.
- This applies equally when writing new code: prefer deriving behavior at
  runtime or making it configurable over hardcoding for the one target
  you happen to have in front of you
  (`docs/INITIAL_IMPLEMENTATION_GUIDELINES.md` → Design principles).

## Complete the feature, not only its core helper

- Trace the feature from its trigger or caller to its observable result. A
  core helper that is never invoked, or whose result does not reach the
  observable behavior, is not implemented end to end.
- Include the supporting pieces needed for the feature to be maintainable:
  integration, required metadata and license headers, documentation,
  generated-artifact handling, and regression tests where the project has a
  test mechanism. Do not add ceremonial files that the feature does not
  actually need.
- Derive regression cases from the acceptance surface, not only from the
  reported example. Cover representative valid and invalid cases,
  boundaries, malformed/error input, and at least one case proving ordinary
  valid input is not rejected. An AI agent may write these tests but must
  leave running them to the maintainer per `docs/WORKFLOW.md`.
- If enforcing a new invariant exposes pre-existing violations, inventory
  the full set before deciding the patch structure. Prefer a separate
  prerequisite cleanup commit followed by the implementation commit, so the
  new behavior starts against a clean tree and neither commit hides the
  other. Do not weaken the general rule merely to accommodate existing
  violations.

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

Also compare the finished diff with the acceptance surface you wrote before
coding. Every category should be implemented, deliberately rejected, or
explicitly deferred; being absent because the prompt did not include an
example is not a decision.
