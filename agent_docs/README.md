# agent_docs/

This directory is the single home for all agent-facing documentation on
this port — a fork of NVIDIA's `dxvk-remix` focused on running modern
games through the Remix SDK API. Any agent (Claude, Gemini, Copilot,
Codex, or a human reading with their brain) should be able to find the
full fork-port story by reading files here.

## Layout

```
agent_docs/
├── README.md                                — this file
├── fork-touchpoints.md                      — authoritative inventory of
│                                              every upstream file the fork
│                                              touches, with Hook vs Inline
│                                              classification
├── rebase-measurement-2026-04-18.md         — Phase 4 fork-footprint
│                                              reduction measurement (54%)
├── specs/                                   — design specs, one per
│   │                                          workstream
│   ├── 2026-04-17-unity-fork-port-workstream-01-api-skinning-design.md
│   ├── 2026-04-18-fork-touchpoint-pattern-design.md
│   ├── 2026-04-18-unity-fork-port-workstream-02-tonemap-design.md
│   ├── 2026-04-18-unity-fork-port-workstream-05-hillaire-atmosphere-design.md
│   └── shelved/                             — specs no longer active
│       └── 2026-04-19-unity-fork-port-workstream-03-hdr-design.md
├── plans/                                   — implementation plans
│   ├── 2026-04-17-workstream-01-api-skinning.md
│   ├── 2026-04-18-fork-touchpoint-pattern-plan.md
│   ├── 2026-04-18-workstream-02-tonemap.md
│   ├── 2026-04-18-workstream-05-hillaire-atmosphere.md
│   └── shelved/
│       └── 2026-04-19-workstream-03-hdr.md
└── audits/                                  — divergence + correctness
    │                                          audits vs gmod reference
    └── 2026-04-19-gmod-divergence-audit.md
```

## What each folder is for

- **`specs/`** — "what we're building and why" — design-phase docs written
  before implementation. Read these when trying to understand *intent*.
- **`plans/`** — "how we'll build it" — step-by-step implementation plans,
  usually referencing a spec. Read these when picking up partially-completed
  work.
- **`audits/`** — "what's actually shipped vs what exists elsewhere" —
  point-in-time snapshots of divergence against upstream or gmod.
  Read these when scoping a new workstream.
- **`fork-touchpoints.md`** + **`rebase-measurement-*.md`** — standing
  references on the fork-discipline pattern. Read these when touching
  any upstream file.

## Shelved work

`specs/shelved/` and `plans/shelved/` hold material for cancelled /
paused workstreams. Kept rather than deleted so the thinking is preserved
if conditions change. Check the corresponding shelving commit for context
on *why* it was shelved.

## Workstreams status (2026-04-19)

| # | Name | Status | Branch |
|---|---|---|---|
| W1 | Remix API + HW skinning | shipped | `unity-workstream/01-api-skinning` |
| W2 | Tonemap operators | shipped | `unity-workstream/02-tonemap` |
| W3 | HDR | **shelved** (upstream gmod impl broken per author 2026-04-19) | `unity-workstream/03-hdr` (local) |
| W4 | Remix API correctness fixes (externalMesh + capture guards + log silence) | shipped | `unity-workstream/04-unity-native-fixes` |
| W5 | Hillaire atmosphere | shipped | `unity-workstream/05-hillaire-atmosphere` |
| W6 | Agent docs consolidation | in-flight | `unity-workstream/06-agent-docs-refactor` |

Shipping branch (downstream users pull this):
`kim2091/modern-games-sdk-api`.

## Workstream numbering note

The port uses an ad-hoc numbering where new workstreams pick the next
free number. W3 is shelved, so numbers aren't necessarily monotonic
in shipping order.

Branch prefix `unity-workstream/...` is historical — it tracks gmod's
`origin/unity` baseline rather than indicating an engine-specific
focus. The port itself is engine-agnostic as far as Remix API
integrations go.

## When you're new to this repo

1. Read the root `CLAUDE.md` first — it sets the ground rules.
2. Read `fork-touchpoints.md` — it's the touchpoint inventory you'll
   be updating on every upstream-file edit.
3. If you're scoping a new workstream, check `audits/` for the latest
   divergence pass, then look for a matching spec in `specs/`.
4. Don't commit to `main` — it's upstream. Branch off the latest
   shipped workstream tip (check the status table above).
