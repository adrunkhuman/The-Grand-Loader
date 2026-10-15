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

- Each AI country gets its own decision lists without those decisions. `AI_DecisionUpdate` (called at
  `0x0081EA9B`) reads the global manager `DAT_01319730`; a wrapper points it at a private copy holding the
  country's lists for the duration of the call. `AI_DiplomacyUpdate`'s two getter calls (`0x0085D34F`,
  `0x0085D459`) get the same lists. The loops run unchanged on fewer entries, in the same order.
- Only decisions with the standard slot 6 and an entirely
  [pure potential](cull-cb-checks.md#shared-purity-whitelist) may be removed by a failing literal tag
  or AI gate. Unsupported potentials, including flag predicates, stay in their original positions.
- Lists are rebuilt when `clr_country_flag` (that country) or `clr_global_flag` runs (hooks on their
  `Execute` vtable slots), when a save is loaded, every 90 game days, and when the decision list changes.
- A false check can still advance scope RNG and affect later decisions; only pure potentials are
  culled. The player's alert, the UI, `allow`, `ai_will_do` and effects are untouched.
