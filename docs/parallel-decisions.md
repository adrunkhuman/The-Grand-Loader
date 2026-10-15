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
- Every retained decision's slot 6 must be the standard implementation; `potential`, `allow` and all
  `ai_will_do` modifier trees must pass the [purity whitelist](cull-cb-checks.md#shared-purity-whitelist).
  Otherwise both country lists stay serial, including in shadow mode; complex mod trees often fall back.
- A worker pool (logical processors − 1, at most 7) runs batches with the game thread's FPU state.
- At the start of the frame in which `fast_ai_queue` drains the AI queue, the countries waiting in it are
  read from the queue, and the workers evaluate `potential` and `allow` of every decision of each
  country's list, with the same scope the game builds, in native B-then-A list order.
- When the game then runs `AI_DecisionUpdate` for a country, it gets only the decisions that passed, in the
  original order; `ai_will_do`, the `rand()` roll and the command run in the game's own code, so the same
  rolls happen. A result is used only for the same frame, AI object and list version.
- `2` = shadow mode: the game keeps its own evaluation; only eligible countries are compared and logged.
