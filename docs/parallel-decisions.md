# `parallel_decisions`

## Source

`patch/src/features/performance/decision_cull.cpp`, `patch/src/parallel/worker_pool.cpp`. Research:
[re/systems/parallel-triggers.md](re/systems/parallel-triggers.md).

## Why

Once per AI country per day, `AI_DecisionUpdate` (`0x00821190`) tests `potential` and `allow` of every
decision in its list, then `ai_will_do`, a `rand()` roll and, for a chosen decision, a command that the
game only executes later. Checks can be computed in advance only when evaluation is pure; a failed
check may otherwise advance the shared scope RNG and change later decisions.

## How

- Requires `cull_decision_checks = 1` and `fast_ai_queue = 1`.
- Only the leading run of decisions with standard slot 6 and pure `potential`, `allow` and all
  `ai_will_do` modifier children enters a batch. The
  [purity whitelist](cull-cb-checks.md#shared-purity-whitelist) is checked under current runtime guards.
  The first retained unsupported decision and the complete suffix remain native, including all A
  entries if the barrier is in B.
- A worker pool (logical processors − 1, at most 7) uses the game thread's FPU state and native scope
  setup. Compute one country's prefix immediately before its AI update—not future countries at frame
  start, where intervening native work could make results stale.
- Remove failed pure-prefix checks and retain survivors in original B-then-A order. The game rechecks
  survivors and executes `ai_will_do`, `rand()` and commands itself. No speculative result survives
  the native update.
- `2` = shadow mode: keep original lists and compare only the pure prefix. Unsupported suffix checks
  are never run an extra time. Early unsafe entries can still limit optimization coverage.
