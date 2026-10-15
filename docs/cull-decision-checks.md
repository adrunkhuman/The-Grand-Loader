# `cull_decision_checks`

## Source

`patch/src/features/performance/decision_cull.cpp`. Research: [re/systems/decisions.md](re/systems/decisions.md).

## Why

Once per AI country per day, `AI_DecisionUpdate` (`0x00821190`) evaluates the `potential` of every decision
(TGC: 1651 decisions × ~320 countries ≈ 529 k evaluations a day, ~12 % of the day). Many of them are
false before they start:

- a decision with a top-level `tag = X`, evaluated for a country other than X (~213 k a day);
- a decision with a top-level `ai = no`, evaluated for an AI country.

A hook on each evaluation cannot win: the evaluations cost tens of nanoseconds, about as much as the hook.
The work has to disappear from the loop.

## How

- Immediately before `AI_DecisionUpdate` (`0x0081EA9B`), build private lists in native B-then-A order
  and point the global manager `DAT_01319730` at that copy for the call. No filtered list survives
  another AI update, command or flag/controller change. Diplomacy, player alerts and UI stay native.
- A standard-slot-6 decision may be removed by a failing leading literal tag, AI or flag gate. Walk
  its potential in order and stop at the first child outside the
  [purity whitelist](cull-cb-checks.md#shared-purity-whitelist); never search past a stateful check
  for a later gate. Recheck runtime guards when building the lists.
- A retained decision with unsupported potential, allow or AI modifiers ends speculative culling.
  Keep it and the entire remaining suffix, because it may affect later checks. Native evaluation,
  `ai_will_do`, RNG and effects are not replaced.
