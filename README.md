# Torafirma Skill Router

> A local-first, single-binary router for discovering and loading large agent skill libraries without flooding the model context.

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4)
![Language](https://img.shields.io/badge/C%2B%2B-20-00599C)
![Tests](https://img.shields.io/badge/tests-47%2F47-passing)
![SQLite](https://img.shields.io/badge/SQLite-3.53.4-003B57)
![License](https://img.shields.io/badge/license-MIT-green)

Skill Router keeps one small interface skill in context and leaves the full skill library on disk. It indexes only metadata in SQLite, searches that compact index, and loads a full `SKILL.md` body only after a caller explicitly selects it.

## Why it exists

Large agent installations can contain hundreds or thousands of specialized skills. Injecting every skill description and body into every prompt is slow, expensive, and noisy. Skill Router reverses that model:

1. Index skill metadata locally.
2. Search by natural-language intent.
3. Return a small ranked candidate list.
4. Fetch only the chosen skill body.
5. Track transparent usage telemetry to improve deterministic ranking.

Search output stays bounded by `--top`; it does not grow with the size of the library.

## Highlights

- One portable Windows x64 executable
- Local-first operation with no remote service dependency
- SQLite FTS5 full-text search with Porter stemming
- Exact, fuzzy, FTS, and hybrid ranking modes
- CLI, interactive shell, loopback HTTP, and MCP stdio interfaces
- Explicit skill lifecycle states and drift detection
- Prepared SQLite statements and bounded query/file inputs
- Deterministic SHA-256 release manifest
- MIT licensed source
- Reproducible MSVC packaging script
- 47 automated C++ tests

## Windows release

[Download Skill Router 1.0.0 for Windows x64](releases/skill-router-windows-x64-1.0.0.zip)

ZIP SHA-256:

```text
FBB8925A621A22BE13EDE6B09BAD9CA0B90DD968B71CC79AB30DF2849338865F
```

The executable is currently unsigned. Verify the ZIP digest before running it.

## Quick start

Extract the archive into a writable directory, open PowerShell there, and add skills beneath `skill_library\`. Each skill directory needs a `SKILL.md` containing `name` and `description` frontmatter.

```powershell
powershell -ExecutionPolicy Bypass -File .\index-skills.ps1
.\skillrouter.exe search "windows cpp build" --top 5 --json
.\skillrouter.exe fetch skill-router
.\skillrouter.exe stats
```

## Search modes

```powershell
.\skillrouter.exe search "authentication" --mode hybrid
.\skillrouter.exe search "authentication" --mode exact
.\skillrouter.exe search "authentication" --mode fts
.\skillrouter.exe search "authentcation" --mode fuzzy
```

Hybrid mode is the default. Exact matches remain dominant while FTS stemming and bounded fuzzy matching improve recall.

## MCP integration

Skill Router can run as a newline-delimited JSON-RPC MCP server over stdio:

```powershell
.\skillrouter.exe mcp --db .\skill_index.db
```

It exposes search, fetch, statistics, graveyard telemetry, and `skill://` resources. See the [integration manual](skill-router/INTEGRATION_MANUAL.md) for the complete protocol reference.

## HTTP interface

The optional HTTP interface binds to IPv4 loopback only:

```powershell
.\skillrouter.exe serve --db .\skill_index.db --port 8090
```

Available endpoints include `/health`, `/stats`, `/graveyard`, `/search`, and `/fetch`.

The HTTP interface is intended for trusted local development. It does not provide authentication, so do not expose or forward the port and do not use it with confidential skill libraries on a shared machine.

## Build from source

Requirements:

- Windows x64
- Visual Studio 2022 C++ Build Tools
- PowerShell 5.1 or later

Build and test from a Visual Studio x64 Developer Command Prompt:

```powershell
cd .\skill-router
.\build_windows_msvc.bat
.\build\test_library.exe
```

Create a clean release:

```powershell
.\package-windows.ps1
```

The packager initializes the Visual Studio toolchain, builds SQLite with FTS5, runs all tests, stages an allowlisted payload, writes `SHA256SUMS.txt`, and creates a versioned ZIP under `dist\`.

## Repository layout

```text
.
|-- README.md
|-- LICENSE
|-- releases/
|   |-- SHA256SUMS.txt
|   `-- skill-router-windows-x64-1.0.0.zip
`-- skill-router/
    |-- main.cpp
    |-- skill_library.hpp
    |-- mcp_server.hpp
    |-- test_library.cpp
    |-- third_party/
    |-- skills/skill_router/
    |-- build_windows_msvc.bat
    |-- package-windows.ps1
    |-- index-skills.ps1
    `-- INTEGRATION_MANUAL.md
```

## Security posture

- The release contains no private skill library, generated database, backup, object file, debug symbol, credential, or machine-specific user path.
- SQLite is vendored at version 3.53.4 and compiled with FTS5.
- SQL values are passed through prepared statements.
- The Windows binary enables ASLR, DEP/NX, and high-entropy virtual addresses.
- Network serving is opt-in and loopback-only.
- MCP uses stdio and opens no network listener.
- Skill bodies are data, not executable code, but downstream agents must still treat third-party skill instructions as untrusted.
- Database files and indexed skill directories should come from trusted sources.
- The binary is not Authenticode-signed; use the published SHA-256 digest.

## Documentation

- [Skill Router README](skill-router/README.md)
- [Windows release guide](skill-router/WINDOWS_README.md)
- [Complete integration manual](skill-router/INTEGRATION_MANUAL.md)
- [Interface skill](skill-router/skills/skill_router/SKILL.md)

## License

MIT License. Copyright (c) 2026 Thomas Helm.

Created by Thomas Helm as part of Torafirma Systems.
