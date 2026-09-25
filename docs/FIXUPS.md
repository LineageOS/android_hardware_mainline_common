# Fixups

Standard operating procedure for closing a gap in a commit that is *already
merged* into this repository's history — typically a human contributor's
commit that met the bar for correctness and testing but skipped something
else `docs/` requires. Written for both AI agents and human contributors.

This is a Gerrit-backed history: every merged commit carries a `Change-Id`
and has already gone through `docs/REVIEW.md`. Once it's merged, do not
rewrite it — no `commit --amend`, `rebase` past it, or `push --force`, even
if you're the one who wrote it and even for a trivial typo in its own
message. Close the gap with new, forward-only commit(s) on top instead.

## When this applies

- A human contributor's commit changed behavior correctly, but the
  documentation, formatting, or another process requirement in `docs/`
  wasn't kept in sync. `Assisted-by` trailers are only required for
  AI-assisted commits (`docs/COMMIT_CONVENTIONS.md`), so a human commit
  missing them is not itself a gap.
- You notice while working on unrelated code nearby — the most common
  trigger; fix it rather than let it linger.
- You're specifically asked to audit a commit or range for `docs/`
  compliance after the fact.

Do not go looking for this proactively across the whole history
(`docs/INITIAL_IMPLEMENTATION_GUIDELINES.md` → "do not search broadly");
this is for gaps you actually ran into.

## Finding the gap

Apply `docs/REVIEW.md`'s seven checks to the merged commit as if you were
reviewing it before merge. Gaps found this way cluster into two very
different kinds:

1. **Compliance gaps** — checks 1 (code style), 2 (consistency, in
   particular the documentation bullet), 7 (clarity: one logical change per
   commit, message format). The behavior itself is fine; only the process
   was skipped: formatter never run on the touched files, `AGENTS.md` /
   `README.md` left describing the old behavior, a commit that bundled two
   unrelated changes, etc.
2. **Actual bugs or regressions** — checks 3 (core principle), 4 (other
   users' use cases), 5 (correctness). Re-reading the merged commit turns up
   a real behavior problem, not just a missed doc update. Fix these the
   normal way: an ordinary commit describing the bug and the fix, exactly
   like any other bug fix. It happens to be triggered by revisiting an old
   commit, but it is not a "fixup" for that reason and shouldn't be
   undersold as one.

Check 6 (testing) cannot be satisfied retroactively by you — see "What not
to do" below.

## Making the fixup

- One commit per kind of gap, not one catch-up commit that does everything.
  Check 7's "one logical change per commit" applies here just as much as to
  new features: a commit that reformats *and* rewrites documentation is
  harder to review than two small ones.
- Prefer content before mechanics: a documentation/behavior-description
  catch-up first, then a separate pure-formatting pass. That keeps the
  formatting commit a true no-functional-change diff instead of one that
  also carries prose changes on the same lines.
- A formatting-only fixup changes nothing else — don't fold in an unrelated
  tweak just because you're already touching the file.
- Follow `docs/COMMIT_CONVENTIONS.md` for the subject/body/trailers like any
  other commit. There is no special `fixup!` subject prefix here; that git
  convention is for commits you intend to squash locally before they're ever
  pushed, which doesn't apply once history is already merged.
- Say what was missing and which prior commit(s) left it that way in the
  body (e.g. "The last three commits changed how X is built, but the
  documentation still described the old behavior"), so a future reader can
  tell why the fixup exists without archaeology.
- Keep the `Assisted-by` trailers accurate for the fixup commit itself,
  regardless of whether the original commit had them.

## What not to do

- Don't amend, rebase, or force-push a commit that's already merged/pushed.
- Don't invent test evidence to satisfy check 6 retroactively; if a merged
  commit lacks it, say so in the fixup's body (or ask the contributor)
  instead of asserting untested behavior was verified.
- Don't rewrite the original author's commit message or attribution — the
  fixup is a new commit under your own authorship and trailers.
- Don't bundle a compliance fixup and a genuine bug fix into the same
  commit even if you found both while looking at the same merged commit —
  they carry different review weight (a doc catch-up is close to
  auto-approve, a behavior change is not).

## Worked example

`a4906812..d1120014` in this repository. Three commits by a human
contributor (`57f9b6b8`, `6d03782b`, `80d8121e`) reworked mix port
capability handling and output promotion in `interfaces/audio/mainline`,
without updating `README.md` / `AGENTS.md` for the new behavior and without
a `clang-format` pass. The next two commits are the fixups:

- `7dad06e0` — documentation catch-up only (`README.md`, `AGENTS.md`, two
  stale code comments); no behavior change.
- `1210c4b2` — formatter-only pass over the files the three commits touched;
  no behavior change.

The three commits after those (`6d303458`, `a379e191`, `d1120014`) are
ordinary bug fixes found while re-reading the same code with checks 4/5 in
mind (a mix port that could end up with no profiles, channel counts that
weren't intersected, an over-broad promotion list) — each is its own commit,
on its own merits, not framed as a "fixup".
