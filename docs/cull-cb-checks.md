# `cull_cb_checks`

## Source

`patch/src/features/performance/cb_cull.cpp`.
Research: [re/systems/casus-belli.md](re/systems/casus-belli.md).

## Why

Every day every AI country scans every other country twice (`AI_ScoreWarTargets` `0x0085DB40`,
`AI_ScoreDiplomaticTargets` `0x0085E0E0`) through `CBManager_IsAnyCBUsable` (`0x0092EDE0`), which walks every
CB type. For each type, `IsCBUsable` walks the CBs the attacker holds against the target, builds two
scopes and, for `always = yes` or held types, evaluates the whole `can_use`. Conditions that depend only
on the attacker (`tag = X`, `civilized = yes`, flags) are re-evaluated for every target.

## How

- The two AI call sites (`0x0085DCDA`, `0x0085E5FB`) go to a replacement of the loop; the UI, the console,
  the `casus_belli` trigger and war planning keep calling the original.
- Standard type layouts and native `IsRealType`/`CanUse` dispatch are checked before evaluation;
  unknown implementations leave the whole scan native.
- Held CB types are indexed once instead of searching the same linked list for every type. Types
  remain in manager order, including `always` types, and the scan stops at the first true result.
- Every actual `CanUse` runs natively with fresh attacker/THIS scopes. Unsupported trigger trees
  are not skipped. After a false evaluation, held membership is rebuilt before checking later types.
- No attacker predicate is pre-evaluated or cached for the day. Read-only does not imply defender
  independence or stability across commands and events. With `cull_cb_profiling`, logs report optimized
  versus native scans and the held-list builds/nodes actually visited.

## Shared purity whitelist

Event and decision speculation use `trigger_safety.h` and its audited native vtable/Evaluate table
in `trigger_safety_data.h`. The CB membership optimization does not speculate and does not need it.
New native findings and guard rationale are documented in
[Smedley's trigger-purity research](https://github.com/adrunkhuman/smedley_kernel/blob/d6c8e0eb7c1bb615ce943821204c92e542b50883/mappings/research/native-trigger-purity.md).

The inspector admits read-only leaves, recursively checked iterators and deterministic context
selectors. Flag lookups require native receivers and the CRT C-locale path; getters with lazy shared
initialization require already initialized objects. Numeric guards are checked on the executing
thread. Certify flag receiver domains only when a tree needs them, once per frozen run; no certificate
survives a native barrier. The table still admits 236 native classes, not five leaf types.

| Permission | Required evidence |
| --- | --- |
| Leading-gate rejection | Every skipped earlier child is deterministic and read-only; no trial of a random condition. Later children need not be safe. |
| Worker evaluation | The concrete evaluated tree is read-only under its runtime guards and the world remains frozen. |
| Random worker evaluation | Separate ownership, reproducible isolated streams and single-use consumption; not enabled by this branch. |
| Predicate caching | Complete dependencies and a valid lifetime/invalidation rule; neither read-only nor RNG-only behavior establishes this. |

`barrier_reason` distinguishes incoming-scope RNG, scope writes, shared-state writes, failed runtime
guards and unclosed paths. These are conservative reasons to stay native, not equivalent effects or
permissions. A path with unresolved effects is not asserted to mutate shared state.

Unknown or unclosed trees stay native. In particular, missing-variable insertion, vassal-cache
rebuilds, input-scope writes, `ally`/`local_enemy` selection and nested CB queries are not blanket
approvals. Purity is neither permission to cache across defenders nor a whole-day validity guarantee.
