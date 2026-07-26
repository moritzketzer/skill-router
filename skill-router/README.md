# pi-skill-router — interface + router for a skill library of any size

Local Pi package exposing the Windows `skillrouter` binary and an indexed skill
library. A skill library that scales past a few dozen entries can't have every
skill's name + description pasted into every context window. This inverts that:
one skill (`skill_router`) stays in context; every other skill lives on disk,
is **indexed (not loaded)** in SQLite, and is fetched by `skill_id` only when
something explicitly decides to use it. `search` cost is bounded by `--top`,
not by how many skills exist.

## This deployment

```
skill-router/
  skillrouter.exe            the package bin (package.json -> ./skillrouter.exe)
  skill_library/             the real skill library (one dir per skill, each with SKILL.md)
  skills/                    pi skills exposed to the agent (incl. the skill_router interface skill)
  skill_index.db             the SQLite index (built from skill_library/ by index-skills.ps1)
  index-skills.ps1           (re)index skill_library/ into skill_index.db
  build_windows_msvc.bat     build skillrouter.exe with MSVC
  main.cpp, skill_library.hpp, mcp_server.hpp, test_library.cpp, Makefile, third_party/
  INTEGRATION_MANUAL.md      full reference (architecture, ranking, every interface)
```

## Build

```bat
REM MSVC (Developer Command Prompt). Compiles SQLite with -DSQLITE_ENABLE_FTS5
REM (required for hybrid search's full-text index) and publishes ./skillrouter.exe.
build_windows_msvc.bat
```

A statically-linked MinGW build is equivalent: `make windows` (produces a
self-contained `skillrouter.exe`, no DLLs needed). Either way, `make test` /
`build\test_library.exe` runs the suite (**47/47**).

## Package for Windows

From a PowerShell prompt with the Visual Studio C++ build tools available:

```powershell
.\package-windows.ps1
```

This performs a clean build and test, stages only the runtime files, writes a
SHA-256 checksum manifest, and creates a versioned ZIP under `dist\`.

## Index the library

```powershell
.\index-skills.ps1        # scans skill_library/ -> skill_index.db (default paths)
```

Re-run any time skills change: new ones are added, edited ones re-indexed (drift
detected via a content hash, telemetry preserved), unchanged ones skipped. The
index stores only `skill_id`, `description`, `keywords`, `path`, and telemetry —
**never a skill body** (read fresh from disk on fetch), so index/search cost is
independent of how large any skill is.

> First run of the updated binary against an existing `skill_index.db`
> transparently builds the FTS5 full-text index (a one-time self-heal); no manual
> re-index is required just to enable stemmed search.

## Use it (search → fetch)

```bat
skillrouter search "authenticating users with oauth" --top 5
REM  -> ranked [{skill_id, description, score, state}] — never a body

skillrouter fetch <skill_id>
REM  -> that skill's full SKILL.md — the one moment it enters context
```

Add `--json` for machine-readable output.

### Hybrid search (new): exact + FTS5 + fuzzy

Search blends three engines, selectable with `--mode` (default `hybrid`):

- **exact** — token match (keywords 3.0 / name 2.0 / description 1.0). The spine.
- **fts** — SQLite **FTS5** with the **Porter stemmer**, so morphological variants
  and prefixes match (`train`↔`training`, `authenticating`↔`authentication`).
- **fuzzy** — bounded edit distance for typos (`cryptografy` → `cryptography`).

FTS and fuzzy are weighted below one exact tier, so they add recall (finding
skills a differently-worded or misspelled query would otherwise miss) without
ever overturning a clear exact match. `--mode exact|fts|fuzzy` isolates one
engine; `exact` reproduces the original pre-FTS behavior. If a build lacks FTS5,
the engine degrades gracefully to exact + fuzzy.

## Interfaces

The same engine is reachable four ways — telemetry and ranking behave identically:

- **CLI** — `search`, `fetch`, `index`, `stats`, `graveyard`, `register`, `use`,
  `deprecate`, `archive`.
- **Live shell (new)** — run `skillrouter` with **no arguments** for an
  interactive REPL with a live status + event dashboard (skills by state,
  telemetry, and the tail of the search/fetch event stream, refreshed each
  prompt). `skillrouter --help` prints scripting usage instead.
- **HTTP API** — `skillrouter serve` (loopback only): `/health`, `/stats`,
  `/graveyard`, `/search?q=..&mode=..`, `/fetch?id=..`.
- **MCP (new)** — `skillrouter mcp` speaks the Model Context Protocol over stdio,
  so any MCP host can use the whole library as tools (`skill_search`,
  `skill_fetch`, `skill_stats`, `skill_graveyard`) and resources
  (`skill://<skill_id>`). Register e.g.:
  `claude mcp add skillrouter -- <abs-path>\skillrouter.exe mcp --db <abs-path>\skill_index.db`

`--db` defaults to `skill_index.db` in the working directory on every subcommand.

Full reference: **`INTEGRATION_MANUAL.md`** (architecture, data model, the exact
ranking formula, complete CLI/HTTP/MCP reference, lifecycle state machine, and
the security notes).

## License

MIT License. Copyright (c) 2026 Thomas Helm. See `LICENSE`.
