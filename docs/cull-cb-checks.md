# `cull_cb_checks`

## Source

`patch/src/features/performance/cb_cull.cpp`, safety checks in `patch/src/features/performance/trigger_safety.h`.
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
- Every real type must have the expected layout, standard `CanUse` and a pure `can_use` tree
  (see below). Otherwise the whole scan stays native, without a trigger pre-pass.
- Once per attacker and day, the attacker-only direct children of each eligible type's `can_use` are
  evaluated; a type with a false one is dead for that attacker for the day.
- Per target, `CanUse` runs for the live `always` and held types, stopping at the first true. Each call
  gets fresh scopes (attacker; `+0x24` -> THIS = defender), matching native per-type construction.
- Skipping and reordering are restricted to pure trees: an `ally` scope check can advance scope RNG
  even when false (`scope+0x40`, helper `0x009B7910`).

## Shared purity whitelist

Only `AND`/`OR`/`NOT` trees containing `always`, `year`, `month`, `ai` and literal `tag` qualify.
Native Evaluate slots are checked; unknown classes, context/flag predicates and THIS/FROM tags fall
back to native evaluation. Coverage is deliberately small, especially for modded trigger trees.
