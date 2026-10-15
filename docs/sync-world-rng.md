# `sync_world_rng`

## Source

`patch/src/features/multiplayer/world_rng.cpp`; reverse engineering in
[re/systems/multiplayer.md](re/systems/multiplayer.md), "Random number generators".

## Why

Besides the global generator that the daily check watches, the world object carries its own
MT19937, seeded from the clock when the world is created and never written to the save. Two machines
therefore never share it, and the `world_rng` element of the state checksum differed on day 2 of the
first two-instance test. It is read by code that every machine runs in lockstep: the daily roll that
decides whether an AI country thinks today, and the roll by which an AI country accepts or refuses a
diplomatic action when the command executes. It is also read by code that runs on one machine only:
the AI's per-frame jobs on the host, and the tooltip of a diplomatic action, which runs the very same
acceptance roll locally. So in vanilla the AI's diplomatic answers drift apart between players from
the first day, and hovering a tooltip moves one machine's generator further.

## How

- Nine `CALL` sites and the `GetAIRand` tail `JMP` route to one dispatcher. Command execution takes
  draws from the global, counted generator, except during the intra-day application pump
  (`0x00685727` -> `0x009DF2B0`), which can update local UI while the executing flag remains set.
  Pump draws (including nested pumps), AI frame jobs and other draws outside command execution use
  a private generator seeded from the clock. Installation rolls back if a required site cannot be patched.
- Private output stays nonnegative, matching native callers' signed-division contract.
- The world generator itself is left alone; these ten sites no longer consume it.
- **Needs live multiplayer verification:** run long ticks with different peace/war-goal dialogs open
  on each peer. The pump exclusion is supported by static inspection, not an in-game reproduction.
- Single-player results change: the AI's daily timing and diplomatic answers now come from the
  global sequence. Every player of a multiplayer game must use the same value.
