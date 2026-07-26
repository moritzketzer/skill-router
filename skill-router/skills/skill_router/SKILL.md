---
name: skill-router
description: "Use first when a task might benefit from specialized skills in the large migrated skill library. Searches the SQLite index with the installed skillrouter.exe and fetches only the selected SKILL.md body on demand, avoiding full-library context loading."
---
# skill_router

The interface skill. In a library of arbitrary size, this is the **only**
skill whose name and description a model needs to hold in context. Every
other skill lives on disk, indexed (not loaded) in SQLite, discovered
on-demand through this protocol.

## Why this exists

A skill list that scales to thousands of entries cannot be pasted into every
context window - the token cost would dominate the conversation before any
real work happened. `skill_router` inverts the relationship: instead of the
model holding a list of skills, it holds one interface and *asks* for what
it needs, exactly when it needs it.

## Protocol (the only thing to remember)

```
register a skill-library ".\skill_library"

```
skillrouter search "<query>"        -> ranked candidates: skill_id + one-line description + score
skillrouter fetch  <skill_id>       -> the skill's full SKILL.md body (the ONLY place content loads)
```

Installed Windows command:

```
.\skillrouter.exe search "<query>" --db .\skill_index.db
.\skillrouter.exe fetch <skill_id> --db .\skill_index.db
```

Everything else (`register`, `index`, `stats`, `graveyard`, `deprecate`,
`archive`, `shell`, `serve`) is for library operators, not a per-turn model
loop - see `INTEGRATION_MANUAL.md` in this package for the full reference.

## Worked example

```
$ skillrouter search "optimize CUDA kernel memory access"
gpu-compute-cuda  (score 9, INDEXED)
    Master-level CUDA and GPU compute engineering for high-performance
    parallel computation, writing CUDA kernels and optimizing GPU memory
    access patterns.

$ skillrouter fetch gpu-compute-cuda
---
name: gpu-compute-cuda
description: "..."
---
# gpu-compute-cuda
... (full skill body - only now does this enter context) ...
```

`search` cost is bounded by `top_n` (a handful of short lines), never by how
many skills exist in the library. Measured on a real 31-skill demo corpus
built from this project's own skill descriptions: a `search` call returns
~1.5% of the total bytes across all 31 skill bodies - and that ratio shrinks
further as the library grows, since `search` output size depends only on
`top_n`, not on library size.

## What the router tracks, and why it improves over time

Every `search` logs one `SUGGESTED` event per returned candidate; every
`fetch` logs one `FETCHED` event. From that:

- **Ranking boost**: skills that convert suggestions into fetches for
  similar queries rank incrementally higher over time (deterministic, bounded
  - see `skill_library.hpp`'s `search()` for the exact formula). Not a
  learned model; a transparent, auditable telemetry boost.
- **Graveyard candidates**: skills suggested often but almost never fetched
  surface via `skillrouter graveyard` - the "extract value from failure /
  do not let every branch remain alive" signal, computed from real usage
  rather than guessed.
- **Drift detection**: if a skill's file changes on disk without going
  through `register`, the next `index` pass detects the content-hash
  mismatch and re-indexes it, routing through an explicit `STALE` state
  rather than silently going out of sync.

## State machine

`REGISTERED -> INDEXED -> ACTIVE` (first fetch) `-> STALE` (on-disk drift,
explicit, not silent) `-> INDEXED` (re-index resolves it) `-> DEPRECATED`
(ranked down, still fetchable) `-> ARCHIVED` (excluded from search by
default, still fetchable with `--include-archived`).

## Full reference

See `INTEGRATION_MANUAL.md` for: complete CLI reference, HTTP API, the
harness hook points (`SESSION_START` / `ON_SKILL_MENTION` / `SESSION_END`),
the ranking formula, build instructions (Linux + genuine cross-compiled
Windows `.exe`, both verified), and honest scope notes on what this system
does and does not (yet) wire into.
