# Skill Router Integration Patterns

This document shows how the same `skillrouter.exe` can serve individual agents, multiple concurrent agents, human operators, scripts, and local services from one centralized skill library.

The key property is separation of concerns:

- skill bodies remain on disk;
- only compact metadata is indexed;
- each agent searches and fetches only the skill it needs;
- agents do not load or mutate one another's active context;
- shared ranking telemetry is centralized and auditable.

## 1. Single-agent automatic routing

```mermaid
flowchart LR
    U[User task] --> A[Agent]
    A -->|MCP tool call| R[skillrouter.exe]
    R --> I[(SQLite metadata index)]
    R --> L[(Skill library on disk)]
    I --> R
    L -->|Selected SKILL.md only| R
    R -->|Search result and fetched skill| A
    A --> O[Task output]
```

The user states the underlying task. The agent decides whether specialist capability may help, invokes the router internally, loads one selected skill, and continues without exposing a manual skill-selection step.

## 2. Centralized library serving multiple agents

```mermaid
flowchart TB
    subgraph Agents[Concurrent agents]
        A1[Agent A\nCode]
        A2[Agent B\nResearch]
        A3[Agent C\nOperations]
        A4[Agent D\nReview]
    end

    A1 -->|search / fetch| R[Central skillrouter service]
    A2 -->|search / fetch| R
    A3 -->|search / fetch| R
    A4 -->|search / fetch| R

    R --> I[(Shared SQLite metadata index)]
    R --> L[(Central skill library)]
    R --> T[(Shared telemetry)]

    I --> R
    L -->|Only each selected skill body| R

    R -->|Skill X| A1
    R -->|Skill Y| A2
    R -->|Skill Z| A3
    R -->|Skill X| A4
```

Multiple agents can use one governed library simultaneously. Each receives only its selected `SKILL.md`; no agent is required to ingest the complete catalogue, and one agent's active skill context is not injected into another agent's context.

Concurrency concerns are concentrated in the shared storage and transport layer rather than in prompt composition. Integrators should still use normal filesystem and process-isolation controls when agents may modify library files.

## 3. One library, multiple transport interfaces

```mermaid
flowchart LR
    subgraph Clients
        M[MCP-capable agent]
        S[Human operator]
        C[CLI script or CI]
        H[Local application]
    end

    M -->|stdio MCP| R[skillrouter.exe]
    S -->|interactive shell| R
    C -->|subcommands and JSON| R
    H -->|loopback HTTP| R

    R --> I[(One index)]
    R --> L[(One skill library)]
    R --> T[(One telemetry stream)]
```

The MCP server, interactive shell, command-line interface, and loopback HTTP API are different front ends over the same routing engine and lifecycle state.

## 4. Per-agent process isolation with a shared library

```mermaid
flowchart TB
    L[(Read-mostly central skill library)]
    I[(Central or replicated metadata index)]

    A1[Agent A] --> R1[Router process A]
    A2[Agent B] --> R2[Router process B]
    A3[Agent C] --> R3[Router process C]

    R1 --> L
    R2 --> L
    R3 --> L

    R1 --> I
    R2 --> I
    R3 --> I
```

Use one router process per agent when process-level fault isolation is more important than maintaining one long-lived service. The skill library should normally be treated as read-only during agent execution; indexing and library administration can be assigned to a separate operator process.

## 5. Central service with governed publication

```mermaid
flowchart LR
    D[Skill author] --> V[Validation and review]
    V -->|admitted| L[(Central skill library)]
    V -->|rejected| Q[Quarantine]
    L --> X[Indexing process]
    X --> I[(SQLite metadata index)]
    I --> R[Router service]
    L --> R
    R --> A1[Agent fleet]
    R --> A2[Interactive shell]
    R --> A3[Local integrations]
```

This pattern separates skill publication from skill consumption. New or modified skills pass validation before entering the active library, reducing the chance that one agent publishes malformed or unsafe instructions directly into the shared capability surface.

## 6. Mixed local development pattern

```mermaid
flowchart TB
    Dev[Developer]
    Shell[Interactive shell]
    Tests[CLI smoke tests]
    Agent[MCP agent]
    App[Local HTTP client]
    Router[skillrouter.exe]
    Index[(skill_index.db)]
    Library[(skill_library)]

    Dev --> Shell
    Dev --> Tests
    Shell --> Router
    Tests --> Router
    Agent --> Router
    App --> Router
    Router --> Index
    Router --> Library
```

This is the simplest development setup: one executable and one library are exercised through every supported interface, making it easier to verify that ranking, lifecycle state, and telemetry behave consistently.

## Operational guidance

| Concern | Recommended pattern |
|---|---|
| Lowest integration complexity | One MCP router process per agent host |
| Centralized capability governance | One shared read-mostly library with a controlled publication path |
| Strong process isolation | Separate router process per agent |
| Shared telemetry and ranking | One central service or a coordinated shared database |
| Human inspection and administration | Interactive shell |
| CI, validation, and scripted maintenance | CLI with JSON output |
| Custom local tooling | Loopback HTTP API |

## Conflict model

Skill Router reduces context-level conflict because skills are selected and loaded on demand instead of placing the entire catalogue into every model context. It does not claim to eliminate all distributed-systems conflicts.

The main remaining conflict surfaces are conventional and controllable:

- concurrent writes to skill files;
- concurrent administrative lifecycle changes;
- database write contention under unusually high process concurrency;
- different agents selecting different valid skills for the same ambiguous intent;
- downstream conflicts caused by the skills themselves.

Treat the active skill library as read-mostly, centralize publication and lifecycle administration, and use separate working directories or process boundaries where agents may modify files.
