# CLAUDE.md — agent entry point for this dxvk-remix port

This repo is a port of NVIDIA's `dxvk-remix` focused on enabling
**modern games to run through the Remix SDK API** — derived from the
gmod-rtx community fork, carrying API surface, capture/replacement,
hw-skinning, tonemap, and atmosphere work needed for API-driven
game integrations. Unity is one such integration path among others;
the port itself is not Unity-specific.

Agent-facing docs live in **`agent_docs/`**. Start there:
**[`agent_docs/README.md`](agent_docs/README.md)**.

## Fork discipline — read before editing upstream files

The port uses a **fork-touchpoint pattern** to minimize rebase cost against
upstream NVIDIA `dxvk-remix`. The full design lives in
[`agent_docs/specs/2026-04-18-fork-touchpoint-pattern-design.md`](agent_docs/specs/2026-04-18-fork-touchpoint-pattern-design.md).
Short version:

- **Prefer hooks over inline tweaks.** When adding fork logic to an
  upstream file, extract the body into a `src/dxvk/rtx_render/rtx_fork_*.cpp`
  module and leave a one-line dispatch into `fork_hooks::…` in the upstream
  file. Fork-owned file naming: `rtx_fork_<subsystem>.cpp/h`.
- **Inline tweaks are allowed but capped.** If a change is truly small
  (< 20 LOC) and structurally can't be a hook (e.g. struct-field addition,
  enum bit), inline is fine. Anything bigger should be refactored.
- **Fridge-list invariant.** Every commit that touches an upstream file
  MUST also update [`agent_docs/fork-touchpoints.md`](agent_docs/fork-touchpoints.md)
  in the same commit. The PR template enforces this.
- **Audit script.** Run `scripts/audit-fork-touchpoints.sh` before
  committing upstream edits.

## Branch conventions

- `main` — upstream NVIDIA mirror. Never commit port work here.
- `unity-workstream/NN-<name>` — one branch per workstream. Branch prefix
  is historical — it tracks gmod's `origin/unity` baseline, not an
  engine-specific intent. (W1 = Remix API + HW skinning,
  W2 = tonemap operators, W4 = Remix API correctness fixes,
  W5 = Hillaire atmosphere, W6 = agent-docs refactor; W3 HDR shelved.)
- `unity-port-planning` — specs/plans-only working branch
  (being consolidated into `agent_docs/` as of W6).
- `modern-games-sdk-api` on `kim2091` remote — the **shipping** branch.
  Downstream users clone this. Fast-forward only, never force-push.

## End-of-project checklist

1. Build with `build_dxvk_release.ps1` pattern (cleanup `nv-private/` +
   `tests/rtx/dxvk_rt_testing/` first, then `PerformBuild -release`).
2. Ship only when exit code 0 and zero compile errors.
3. Ask before pushing to `kim2091/modern-games-sdk-api` — it's visible to
   downstream users. Fast-forward push only.
4. Commits authored as `Kim2091 <jpavatargirl@gmail.com>`. No AI
   co-author trailers unless explicitly requested.

## Project-local skills

Reusable skills for this codebase live in **`.claude/skills/`**. Invoke
via the `Skill` tool by name:

- **`rtx-build`** — wraps the proper meson/ninja build with the
  required `nv-private/` + `tests/rtx/dxvk_rt_testing/` pre-cleanup,
  runs in the background, reports exit code + error count. Use
  whenever the user asks to build or compile-check.

## Don't

- Don't push to `origin` (NVIDIA upstream) — only `kim2091` and `fork`
  are writable remotes.
- Don't force-push anything.
- Don't skip hooks (`--no-verify`) unless explicitly asked.
- Don't add a feature, refactor, or "cleanup" beyond the task scope.
- Don't edit files under `submodules/` or `external/` — those are
  third-party pins.

## Gmod reference repo

Read-only source for porting features from the upstream gmod-rtx
community fork:

```
c:/Users/mystery/Projects/dx11_remix/dxvk-remix-gmod
```

Branches of note: `origin/unity` (the port's baseline — named after the
gmod branch, carries the Remix API / capture / hw-skinning / atmosphere
work that matters for modern-game integrations), `origin/gmod-ex`
(Garry's Mod game-specific — usually out-of-scope). Never commit or
push to this repo.

See [`agent_docs/audits/`](agent_docs/audits/) for the most recent
divergence audits.
