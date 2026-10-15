# `cull_decision_checks`

## Source

`patch/src/features/performance/decision_cull.cpp`. Research: [re/systems/decisions.md](re/systems/decisions.md).

## Why

Most AI decision potentials fail. A leading `tag = USA` can rule out a decision for France without
running later conditions, even if those later conditions are too complex or stateful to speculate.
A failed earlier decision can also change the shared scope or world: filtering an entire country's
list in advance can therefore make later eligibility stale.

## How

- Keep the native manager and B-then-A loop. Check gates when native `AI_DecisionUpdate` reaches each
  potential, against its actual live scope and world—not a cached country list. Diplomacy and UI
  calls pass through; native slot checks, `allow`, `ai_will_do`, RNG and commands remain unchanged.
- A standard-slot-6 decision can fail early on a leading literal tag, AI or flag gate. Search only
  through deterministic read-only children; stop at the first unsupported child. Later complex
  conditions do not prevent an earlier safe gate from rejecting the potential.
- Never trial a random condition during culling. If no gate rules the decision out, run its native
  potential once. Random/stateful conditions before a gate are not skipped or rerolled.
- A serial barrier affects only the current decision. The next potential is reconsidered from the
  resulting live state, so one early unsupported decision does not disable all later work.
- Logs report reached potentials, gate rejections and sequence-building cost. Per-call dispatch and
  classification replace the old filtered-list path; a net speedup needs live measurement.
