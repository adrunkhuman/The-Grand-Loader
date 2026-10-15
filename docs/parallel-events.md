# `parallel_events`

## Source

`patch/src/features/performance/events_parallel.cpp`, `patch/src/parallel/worker_pool.cpp`. Research:
[re/systems/parallel-triggers.md](re/systems/parallel-triggers.md), [re/systems/events.md](re/systems/events.md).

## Why

Every day `EventManager_DailyScan` (`0x008A7CD0`) evaluates the `trigger` of every candidate event of every
country (odd days) or owned province (even days); on true it computes the MTTH, draws a random number and
may fire the event. Some triggers advance scope RNG even when false; firing an event runs its effects
at once and changes the game.

## How

- Before the scan, every (trigger, country or province) pair it will evaluate is listed in the scan's
  exact order. All trees must pass the [purity whitelist](cull-cb-checks.md#shared-purity-whitelist)
  before any vtables are changed or workers run. Otherwise the whole scan stays native, including
  shadow mode. Complex mod scans often fall back. Eligible blocks use a private copy of their vtable.
- When the scan asks for a trigger, the next chunk of 1024 entries is evaluated on the worker pool with
  copies of the scan's own scope (pointers inside the scope, such as THIS at `+0x24`, are moved into each
  copy), and each call is answered from it as long as it is exactly the next entry; anything else runs the
  original code and disables the fast path for the rest of the scan.
- When an event fires, the results computed before it are dropped and recomputed. The MTTH, the random
  draw and the firing stay on the game thread, in order.
- `2` = shadow mode: the game keeps its own evaluation; only eligible scans are compared and logged.
