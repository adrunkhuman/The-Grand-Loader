# `parallel_decisions`

## Source

`patch/src/features/performance/decision_cull.cpp`, `scope_snapshot.h`, `patch/src/parallel/worker_pool.cpp`.
Research: [re/systems/parallel-triggers.md](re/systems/parallel-triggers.md).

## Why

Native `AI_DecisionUpdate` tests `potential`, then `allow`, then `ai_will_do` and a chance roll. Earlier
native checks can change inputs for later decisions. One country-wide precomputed result list is not
valid across those changes, but safe runs between them can still be batched.

## How

- Requires `cull_decision_checks = 1` and `fast_ai_queue = 1`. On reaching a potential, batch up to
  1024 consecutive standard-slot-6 decisions whose potentials pass the
  [guarded read-only certificate](cull-cb-checks.md#shared-purity-whitelist).
- Workers copy the actual live scope, including its seed and external THIS alias. Native daily AI
  setup gives THIS a separate companion scope (copy at `0x008211D2`, assignment at `0x008211F4`);
  reconstructing THIS=self is not equivalent. Only a
  genuine self-reference is rebound in a copy. No speculative scope constructor or initialization.
- Consume each result once at its native potential call. Unsupported potentials run once on the game
  thread, and batching resumes afterward. A custom slot implementation is also a barrier, including
  one that skips its potential entirely.
- A true potential invalidates later results **before** native `allow` and `ai_will_do`. Those blocks
  need no worker certificate because they remain serial. Recompute the next run from the resulting
  live state, even if `allow` or the chance roll fails and no decision is selected.
- Randomized predicates remain native. No independent reroll of a surviving random trial, altered
  stream seeding, or cached random outcome is introduced. No results survive another AI update.
- `2` = shadow: native results drive the game; compare only certified potentials. Unsupported checks
  are not run an extra time. Logs distinguish reached/served potentials, speculative work, runtime
  guard failures and the first serial-barrier reason. Coverage and speedup still need live TGC/MP
  measurement; script-tree eligibility alone is not a benchmark.
