# skill_router Integration Manual

> **Deployment note (pi-agent).** This is the portable `pi-skill-router` package.
> It exposes `skillrouter.exe`
> as the package bin (`package.json`) and indexes the real `skill_library/` via
> `index-skills.ps1` into `skill_index.db`. The engine/CLI/HTTP/MCP reference
> below is authoritative for this build; the specific numbers in the empirical
> section (§11) and the file-manifest layout (§14) describe the reference source
> tree, not this deployment's skill counts or its pi-package extra files
> (`package.json`, `skills/`, `index-skills.ps1`, `build_windows_msvc.bat`,
> `meta_skill_package/`). Build here with `build_windows_msvc.bat` (MSVC); the
> shipped `skillrouter.exe` may instead be a statically-linked MinGW build -
> both are the same source. See `README.md` for the deployment quickstart.

**Status:** core engine (incl. FTS5 stemmed + fuzzy hybrid search) + MCP
interface 47/47 tests green; CLI verified live
(search/fetch/index/stats/graveyard/deprecate/archive/shell/serve/mcp, all
subcommands); HTTP API verified live including hardening edge cases; MCP
server verified live over stdio against the real binary (initialize,
tools/list, tools/call, resources/list, resources/read, ping, plus every
error path); Windows `.exe` genuinely cross-compiled (statically linked) and
execution-verified under Wine (index/search/fetch/stats all confirmed running
correctly, not just "file(1) says PE32+"); frontmatter validated against
XML-tag-shaped placeholder text after a real instance of it was found and
fixed (see section 12, item 4).

---

## 1. What this is, and what it is not

**What it is:** a complete, working system that lets a model or harness use
a skill library of arbitrary size (10, 10,000, more) while holding only ONE
skill's name+description in context - `skill_router` itself. Every other
skill lives on disk as an ordinary `SKILL.md`, indexed (not loaded) in
SQLite, and is fetched into context only at the moment something explicitly
requests it by `skill_id`.

**What it is not:** a modification to Claude's actual production skill-
loading infrastructure. That system isn't something buildable from here -
this manual is honest about that boundary. What's delivered instead is the
complete standalone router (engine + CLI + HTTP API) and a working harness
integration pattern (`harness_demo.py`) that any agent loop capable of
shelling out or making HTTP calls can wire in directly.

## 2. Architecture

```
                 ONE skill in context: skill_router
                          |
                          v
   loose query --> skillrouter search --> ranked [{skill_id, description, score}]
                          |                      (small, ~constant size)
                          |
                 (caller decides to use one)
                          |
                          v
                 skillrouter fetch <skill_id> --> full SKILL.md body
                          |                      (the ONE place content loads)
                          v
              10,000 skills on disk, indexed
              (not loaded) in SQLite: skill_id,
              description, keywords, path,
              content_hash, state, telemetry
```

Three files make the whole system:
- `skill_library.hpp` - the engine (SQLite-backed index, frontmatter parser,
  deterministic scoring with telemetry boost, explicit state machine).
- `mcp_server.hpp` - the Model Context Protocol interface: a small self-
  contained JSON parser plus a pure, transport-agnostic JSON-RPC dispatcher
  that maps MCP tools/resources onto the engine (see section 9).
- `main.cpp` - `skillrouter`, the consolidated CLI + HTTP API + MCP stdio
  binary.

Plus `third_party/sqlite3.{h,c}` (vendored amalgamation, compiled separately
as C and linked) - the only dependency, and it's bundled, not installed.

## 3. Build

```bash
cd skill_router
make            # builds ./skillrouter (Linux/native)
make test       # builds + runs test_library -> 47/47
make windows    # cross-compiles skillrouter.exe (genuine, static, Wine-verified)
```

No CMake, no package manager beyond a C++20 compiler + (for Windows)
`mingw-w64`. The vendored `sqlite3.c` is compiled with
`-DSQLITE_ENABLE_FTS5` (plus `-DSQLITE_THREADSAFE=1`) to enable the full-text
index used by hybrid search (section 6); the engine still builds and runs
without it, degrading to exact + fuzzy search. `sqlite3.c` is C, not C++ - it's compiled by `gcc`/`x86_64-w64-
mingw32-gcc` into its own object file and linked, never `#include`d into a
C++ translation unit (it uses `new` as a plain identifier and relies on
implicit `void*` conversions that don't compile as C++ - this was hit and
fixed during development, see section 12).

## 4. Data model

One `skills` table (STRICT), one `search_log` table (append-only):

| skills column | meaning |
|---|---|
| `skill_id` | canonical name (frontmatter `name`), unique |
| `description` | frontmatter `description` - the only text ever surfaced by `search` |
| `keywords` | derived (or curated) comma-separated terms, highest scoring weight |
| `path` | filesystem path to the real `SKILL.md` - body is read from here at fetch time, never cached in the DB |
| `content_hash` | for drift detection on re-index |
| `state` | the lifecycle state machine, see section 5 |
| `search_count` / `fetch_count` | raw telemetry counters |
| `last_searched_at` / `last_fetched_at` | timestamps |

`search_log` logs one row per `SUGGESTED` (per candidate returned), `FETCHED`,
or `USED` event - the raw signal behind the ranking boost and the graveyard
report.

A third object, `skills_fts`, is an FTS5 external-content virtual table over
`skills(skill_id, description, keywords)` with `content='skills'`,
`content_rowid='id'`, `tokenize='porter unicode61'`. It stores only the inverted
token index (the column *values* are read back from `skills`), and three
triggers - `skills_fts_ai`/`_ad`/`_au`, the last scoped to
`UPDATE OF skill_id, description, keywords` - keep it in sync automatically. It
is created best-effort: if FTS5 is unavailable it is simply absent and search
falls back to exact + fuzzy. See section 6.

**The DB never stores a skill's body.** Only `path` is stored; `fetch_body()`
reads the file fresh every time. This is deliberate: it's what makes `search`
and `stats` cost independent of how large any individual skill is, and it
means editing a skill's `SKILL.md` directly is always reflected (once
re-indexed) without any special sync step.

## 5. State machine

```
REGISTERED -> INDEXED -> ACTIVE (first fetch)
                 |
                 v
              STALE (on-disk content_hash drifted, detected explicitly)
                 |
                 v
              INDEXED (re-index resolves it, preserving telemetry)

INDEXED/ACTIVE -> DEPRECATED (ranked down 0.3x, still fetchable)
INDEXED/ACTIVE -> ARCHIVED   (excluded from search by default, still fetchable with --include-archived)
```

Every transition is an explicit, guarded function call (`set_state`,
`mark_stale_if_drifted`, the implicit `Indexed/Registered -> Active` inside
`fetch_body`) - never an implicit side effect of an unrelated operation.

## 6. Ranking (hybrid: exact + FTS5 + fuzzy)

Search blends three matching engines into one `base` score. The mode is
selectable (`SearchMode` / `--mode` / `&mode=` / MCP `mode`), defaulting to
`hybrid`; `exact`, `fts`, and `fuzzy` isolate a single engine.

1. **exact** - per query token, an exact token match in `keywords` scores 3.0,
   in the skill name 2.0, in `description` 1.0 (first tier wins per token, not
   additive across tiers). This is the spine of the score.
2. **FTS5 stemmed full-text** - a SQLite FTS5 index over `skill_id` +
   `description` + `keywords`, using the **Porter stemmer** so morphological
   variants and prefixes match (`train`~`training`, `optimizer`~`optimizers`).
   It is an *external-content* virtual table backed by the `skills` table and
   kept in sync by triggers scoped to the indexed columns (so telemetry-only
   updates don't churn it) - there is no second copy of the data. Query tokens
   become an OR of quoted prefix terms (`"tok"*`); results are ranked by `bm25`
   (keyword column weighted highest), and normalized per query into `[0.5, 1.0]`.
3. **fuzzy** - bounded Levenshtein edit distance between each *exact-missed*
   query token and the skill's token pool (max distance 1 for tokens <= 4 chars,
   else 2), contributing `1 - distance/len`, averaged over the query tokens.

```
base  = exact + 0.6 * fts_norm + 0.35 * fuzzy      (fts_norm, fuzzy in [0,1])
```

The FTS and fuzzy weights (0.6, 0.35) sum to **0.95 < 1.0**, one exact tier - a
deliberate invariant: FTS/fuzzy add *recall* (surfacing skills with no exact
token hit, e.g. a stemmed or misspelled query, and breaking ties among equal
exact scores) but can **never overturn a clear exact-token winner**. Exact stays
dominant (`test_exact_mode_still_dominates_hybrid_ranking`). If a SQLite build
lacks FTS5 (compiled without `-DSQLITE_ENABLE_FTS5`), `fts_enabled_` stays false
and the engine degrades to exact + fuzzy rather than failing.

Then the same **deterministic, bounded telemetry boost** and deprecation penalty
apply on top of `base`:

```
conversion = fetch_count / search_count   (0 if never searched)
boost = 1.0 + min(0.5, conversion * 0.5 + log1p(fetch_count) * 0.05)
score = base * boost
score *= 0.3 if state == DEPRECATED
```

The boost is not a learned model - it's a transparent, auditable formula that
nudges historically-useful skills upward over time, capped so a handful of old
fetches can never drown out a strong fresh match (verified by
`test_conversion_boost_reorders_over_time`, which starts two skills tied and
shows the boost overcoming the tie only after real fetch history accrues).

## 7. CLI reference

```
skillrouter                             (no args)     live interactive shell + event dashboard
skillrouter register <SKILL.md path>   [--db PATH]
skillrouter index    <root dir>        [--db PATH]
skillrouter search   "<query>"         [--db PATH] [--top N] [--json] [--include-archived] [--mode hybrid|exact|fts|fuzzy]
skillrouter fetch    <skill_id>        [--db PATH] [--context "<query>"]
skillrouter use       <skill_id>       [--db PATH] [--context "<query>"]   # ON_SKILL_MENTION hook
skillrouter stats     [--db PATH]
skillrouter graveyard [--db PATH] [--min-searches N]
skillrouter deprecate <skill_id> [--db PATH]
skillrouter archive   <skill_id> [--db PATH]
skillrouter shell     [--db PATH]                       interactive REPL + live dashboard
skillrouter serve     [--db PATH] [--port 8090]         HTTP API
skillrouter mcp       [--db PATH]                        MCP server (JSON-RPC/stdio)
```

`--db` defaults to `skill_index.db` in the working directory.

Running `skillrouter` with **no subcommand** drops straight into the same
interactive shell as `skillrouter shell` (against the default DB). The shell
renders a **live status dashboard** at the top of every prompt cycle - skill
count by lifecycle state, telemetry (searches / fetches / conversion), and
the tail of the `search_log` event stream - so `SUGGESTED`/`FETCHED`/`USED`
events and state transitions appear as you work. Its commands: `search`,
`fetch`, `events [N]`, `status`, `stats`, `graveyard`, `deprecate`,
`archive`, `help`, `quit`. The dashboard is index-only (it never reads a
skill body), so it stays cheap regardless of library size. `skillrouter -h`
/ `--help` still prints this subcommand usage for scripting.

## 8. HTTP API

`skillrouter serve --db PATH --port 8090` - loopback-only (matches ctxmgr's
posture: this is a local tool, not a public service).

| Method | Path | Response |
|---|---|---|
| GET | `/health` | `{"ok":true,"version":"1.0"}` |
| GET | `/stats` | telemetry + state counts |
| GET | `/graveyard` | low-conversion candidates |
| GET | `/search?q=...` | ranked hits (name+description only) |
| GET | `/fetch?id=...` | full skill body (text/plain), 404 if absent |

Hardening carried over from the ctxmgr project's lessons, applied from the
start here rather than re-discovered: whole-connection `try/catch` (a
malformed request degrades to 400, never crashes the server), 2 MiB request
cap (413 beyond it), `sigaction`-based shutdown on POSIX (plain `signal()`
sets `SA_RESTART` on Linux and silently eats `SIGTERM`, which was the actual
bug found in that earlier project - documented in code comments here so it
isn't rediscovered).

## 9. MCP interface - serve the full library to any model

`skillrouter mcp [--db PATH]` speaks the **Model Context Protocol** over its
stdio transport: newline-delimited JSON-RPC 2.0, one message per line,
requests on stdin, responses on stdout, diagnostics on stderr. This is the
lowest-common-denominator MCP transport every client supports, so *any*
MCP-capable model host can use the full registered library by pointing at
this one binary - no network, no extra services, matching the project's
zero-dependency posture. Register it with a client, e.g. Claude Code:

```bash
claude mcp add skillrouter -- /abs/path/to/skillrouter mcp --db /abs/path/to/skill_index.db
```

(Or the equivalent `command`/`args` entry in any MCP client's config -
`command: skillrouter`, `args: ["mcp", "--db", "<path>"]`.)

**The core property crosses the protocol boundary intact.** The library
index is exposed two complementary ways, and in both, listing/searching is
index-only (name+description, ~constant per skill) while a skill *body* loads
in exactly one place - the same `SkillLibrary::fetch_body()` the CLI and HTTP
API already funnel through. Telemetry (`SUGGESTED`/`FETCHED`) accrues through
MCP calls identically to the CLI.

**As MCP tools** (for an agent that reasons then acts):

| tool | arguments | maps to |
|---|---|---|
| `skill_search` | `query` (req), `top`, `mode` (hybrid/exact/fts/fuzzy), `include_archived` | `search()` - hybrid ranked `[{skill_id, description, score, state}]`, never a body |
| `skill_fetch` | `skill_id` (req), `context` | `fetch_body()` - the ONE place a full body loads |
| `skill_stats` | - | `telemetry()` + `state_counts()` |
| `skill_graveyard` | `min_searches` | `graveyard_candidates()` - suggested-often, never-fetched |

**As MCP resources** (for a client that browses a resource list):
`resources/list` enumerates the full library, one `skill://<skill_id>`
resource per registered skill (name+description only - listing a
10,000-skill library stays cheap); `resources/read` on a `skill://` URI
returns that skill's full body via `fetch_body()`.

**Methods implemented:** `initialize` (advertises `tools` + `resources`
capabilities, protocol revision `2024-11-05`), `ping`, `tools/list`,
`tools/call`, `resources/list`, `resources/read`, `resources/templates/list`,
and notifications (e.g. `notifications/initialized`), which per JSON-RPC
receive no reply.

**Error discipline** (mirrors the HTTP API's "degrade, never crash" posture):
malformed JSON -> JSON-RPC parse error `-32700` (null id); unknown method ->
`-32601`; bad params (missing `name`/`uri`, non-`skill://` scheme) ->
`-32602`/`-32600`; a resource read of an unknown skill -> `-32002`
(resource-not-found). A *tool* failure, by MCP convention, is reported inside
the tool result with `"isError":true` rather than as a protocol error - so a
`skill_fetch` of a missing id returns a normal result the model can read and
react to, not a transport-level fault.

The protocol logic lives in `mcp_server.hpp` - pure and transport-agnostic (a
request string in, a response string out), so the entire surface is
unit-tested in-process without any sockets or pipes (18 of the 40 tests).
`main.cpp`'s `cmd_mcp` is only the stdio read/write loop (with `_setmode`
binary-mode guards on Windows so CRLF translation can't corrupt the newline
framing). The one new capability the header needed beyond the existing
hand-rolled JSON *output* (`json_escape`) is JSON *input*: a small,
self-contained recursive-descent parser (objects, arrays, strings with
`\uXXXX` + surrogate pairs, numbers, literals), added rather than pulling in a
third-party JSON library, to hold the stdlib-first, single-dependency line.

## 10. Harness integration (the three hook points)

`harness_demo.py` runs all three against the real binary (not mocked):

- **SESSION_START** - take the user's message, call `search`, format a
  small "relevant skills" block (name+description only) for the harness to
  prepend to context. Cost is bounded by `--top`, not library size.
- **ON_SKILL_MENTION** - after the model responds, a cheap substring check:
  did it reference a suggested `skill_id`? If so, call `use` - soft
  telemetry distinct from `FETCHED` ("was it actually leaned on" vs "was it
  opened").
- **SESSION_END** - re-run `index` to pick up drift, then `stats` +
  `graveyard` for operator review.

Run it: `python3 harness_demo.py` - indexes the centralized
`skill_library` (this project's own real skill descriptions), runs 3 simulated
turns,
and prints exactly what would be injected into context at each stage plus
the final telemetry/graveyard report.

## 11. Empirical proof of the core property

Measured on the centralized  `skill_library/` (this project's own real
skill descriptions, plus the `skill_router` interface skill itself):

- Total bytes across all skill bodies: **19,117**
- Bytes returned by one `search --top 1` call (name+description+score only): **230**
- Ratio: **1.20%** of full-library size - and this ratio *shrinks further*
  as the library grows, since `search` output size depends only on `--top`,
  never on how many skills exist.

## 12. Real bugs found and fixed during this build

Kept here deliberately, matching this project's own doctrine of preserving
lessons rather than burying them:

1. **`sqlite3.c` is C, not C++.** Attempting to `#include` the amalgamation
   directly into a C++ translation unit fails (`new` used as a plain
   identifier, implicit `void*` conversions). Fixed by compiling it
   separately with a C compiler and linking the object file - this is why
   the Makefile has a dedicated `sqlite3.o` / `sqlite3_win.o` rule.
2. **Windows cross-compile needs portable code, not `#ifdef` patches on
   POSIX-only headers.** `dirent.h`/`sys/stat.h` don't exist on MinGW; fixed
   by switching the directory walk to `std::filesystem::recursive_directory_
   iterator` (portable, and arguably cleaner on both platforms - a case
   where portability forced a genuine improvement, not just a workaround).
   Sockets and shutdown handling are guarded with `#ifdef _WIN32` (Winsock2
   vs POSIX sockets; plain `signal()` on Windows since `sigaction` doesn't
   exist there, vs the deliberate `sigaction`-without-`SA_RESTART` fix on
   POSIX).
3. **Test-fixture vocabulary overlap, not an engine bug.** An early version
   of the graveyard test used two skill descriptions that accidentally
   shared the words "matching task", causing both to legitimately (if
   weakly) match a query intended to isolate just one. Root-caused with a
   standalone debug program before touching anything (confirmed the engine's
   scoring was correct); fixed by using disjoint vocabulary in the fixture,
   not by changing the engine.
4. **Frontmatter `description` containing XML/HTML-tag-shaped placeholder
   text.** The interface skill (`skill_library/skill_router/SKILL.md`)
   originally used `"<query>"` and `<skill_id>` as prose placeholders in its
   `description`
   field - some skill-loading systems parse or render frontmatter in a
   context where that reads as literal markup, not prose, and reject it.
   Fixed the description to use word-based placeholders ("your query text"
   instead of `<query>`), and - more importantly - added `looks_like_xml_tag()`
   to the engine, enforced inside `register_skill()`, so this error class is
   now caught automatically at registration time with an actionable message,
   rather than requiring another manual audit next time. The detector is
   deliberately conservative (`test_xml_tag_detector_does_not_flag_
   comparisons_or_prose` guards against false-positiving on "score < 3" or
   "a < b" style text) - it only flags substrings that are actually
   tag-shaped (`<word>`, `</word>`), never bare comparison operators.

## 13. Security & operational notes

- Loopback-only HTTP bind, matching every other tool in this project.
- The MCP server exposes no network surface at all - it is stdio only, so it
  is reachable exactly by the local process a client launches it as (the same
  trust boundary as running the CLI). Its JSON-RPC handling carries the HTTP
  API's whole-request exception discipline forward: any malformed input
  degrades to a JSON-RPC error object, never a crash of the loop.
- Table/column names never come from user input - the schema is fixed and
  hardcoded; all values are bound via prepared statements.
- Skill bodies are read from disk at fetch time with no size cap on the
  file *content* returned (a caller-side concern by design - a skill body
  is expected to be human-authored, not attacker-controlled, in the way
  arbitrary web content would be); `register_skill` does cap at 8 MiB to
  reject obviously-wrong input during indexing.
- `search`/`stats`/`graveyard` never read skill bodies at all - their cost
  and memory footprint are bounded by the index, independent of how large
  any individual skill file is.

## 14. File manifest

```
skill_router/                 (the project directory)
  skill_library.hpp        engine (single header)
  mcp_server.hpp           MCP interface: JSON parser + pure JSON-RPC dispatcher (single header)
  main.cpp                 skillrouter CLI + HTTP API + MCP stdio loop (portable: Linux + Windows)
  test_library.cpp         47 unit tests (engine + FTS5/fuzzy + MCP)
  Makefile                 native + `make windows` cross-compile target
  third_party/
    sqlite3.h / sqlite3.c  vendored amalgamation (only dependency, bundled)
    skill_library/                the centralized library - every skill in one folder
    README.md              in-folder docs: SKILL.md format + how any agent augments itself
    skill_router/          the interface skill, indexed like any other + its extended manifest
      SKILL.md
      skill.yaml
    gpu-compute-cuda/SKILL.md, api-design-principles/SKILL.md, ... (the rest)
  skill_library_backup/     full directory backup of skill_library/ (regenerated)
  skill_library.zip         portable archive of skill_library/ (regenerated)
  skillrouter_backup.py     stdlib-only backup/zip regenerator
  harness_demo.py           SESSION_START / ON_SKILL_MENTION / SESSION_END demo
  README.md
  INTEGRATION_MANUAL.md      this document
```

(Note: `skill_library.hpp` is the engine *header*; `skill_library/` is the
skill *folder*. Different things, related name - the header indexes the folder.)
