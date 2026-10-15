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
  exact order. Known root layouts use a private vtable copy. Contiguous runs passing the
  [purity whitelist](cull-cb-checks.md#shared-purity-whitelist) enter worker batches; unsupported
  checks run once natively and form serial barriers, rather than disabling the whole scan.
- When the scan asks for a trigger, the next chunk of 1024 entries is evaluated on the worker pool with
  copies of the scan's own scope. Rebind only a genuine THIS self-reference at `+0x24`; retain
  external aliases and scalar seeds/IDs unchanged. Each call is answered from its single-use result
  as long as it matches the listed sequence; an unlisted call runs the
  original code and disables the fast path for the rest of the scan.
- Results are discarded after a serial barrier or a true trigger, before its MTTH modifiers run—even
  if no event fires. The next batch uses the resulting live scope and world. Firing also invalidates
  results. MTTH, random draws and firing stay on the game thread in their native order.
- `2` = shadow mode: the game keeps its own evaluation; pure runs are compared and logged. Unsupported
  checks are not evaluated an extra time for diagnostics. RNG-only paths are distinguished from scope
  writes and shared-state writes, but stay native until stream isolation and single-use random
  results have their own proof.
- Coverage logs report reached/served triggers, speculative work, guard failures and serial-barrier
  reasons. These counters support future live measurements; static script coverage is not runtime
  fast-path coverage or a speedup measurement.
