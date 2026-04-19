# CLAUDE.md — agent entry point for this dxvk-remix port

This repo is a port of NVIDIA's `dxvk-remix` focused on enabling
**modern games to run through the Remix SDK API** — derived from the
gmod-rtx community fork, carrying API surface, capture/replacement,
hw-skinning, tonemap, and atmosphere work needed for API-driven
game integrations. Unity is one such integration path among others;
the port itself is not Unity-specific.

Agent-facing docs live in **`agent_docs/`**. Start there:
**[`agent_docs/README.md`](agent_docs/README.md)**.

## Repo structure — fork + PR model

This project uses a classic GitHub **fork + PR** workflow, with two
kinds of repo hosting the same codebase:

**Canonical repo** — [`github.com/RemixProjGroup/dxvk-remix`](https://github.com/RemixProjGroup/dxvk-remix).
The PR target. Default branch is `modern-games-sdk-api`.
**Hosts only `modern-games-sdk-api` (+ its history, +`CLAUDE.md`,
+`agent_docs/`, +`.claude/skills/`). No W-branches are created here.**

**Personal forks** — anyone contributing forks the canonical repo.
Each contributor's fork holds their own W-branches, scratch work,
and a personal tracking copy of `modern-games-sdk-api`. The canonical
maintainer (Kim2091) also works from a personal fork
(`github.com/Kim2091/dxvk-remix`) for their own W-branches, then
syncs the canonical separately.

### How contributing works

1. **Fork** `RemixProjGroup/dxvk-remix` on GitHub.
2. **Clone** your fork locally. Optionally add the canonical repo as
   a remote: `git remote add canonical https://github.com/RemixProjGroup/dxvk-remix.git`.
3. **Branch** for each piece of work using the `unity-workstream/NN-<name>`
   pattern. Pick the next free N — numbering is ad-hoc, W3 is shelved.
4. **Commit** in small pieces with clear messages. No AI co-author
   trailers unless explicitly requested.
5. **Compile-check** via the `rtx-build` skill (or the equivalent
   PowerShell command) before declaring a branch done. Exit 0 + zero
   compile errors is the bar.
6. **Fast-forward** your fork's `modern-games-sdk-api` to your
   W-branch tip when the branch is ready to ship.
7. **Push** both your W-branch and your updated `modern-games-sdk-api`
   to your fork. Fast-forward only — never `--force` on
   `modern-games-sdk-api`.
8. **Open a PR** from your fork's `modern-games-sdk-api` →
   canonical's `modern-games-sdk-api`.

W-branches are contributor-local. They do NOT live on the canonical
repo.

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

## Branch conventions (personal fork)

- `main` — NVIDIA upstream mirror. Never commit port work here.
- `modern-games-sdk-api` — your fork's tracking copy of canonical's
  default branch. Fast-forward this to a W-branch tip when that
  W-branch is ready to ship; then open a PR to canonical.
- `unity-workstream/NN-<name>` — one branch per workstream. Branch
  prefix is historical — it tracks gmod's `origin/unity` baseline,
  not an engine-specific intent.
  - W1 = Remix API + HW skinning
  - W2 = tonemap operators
  - W3 = HDR (shelved 2026-04-19 — upstream gmod impl broken per author)
  - W4 = Remix API correctness fixes (externalMesh + capture guards + log silence)
  - W5 = Hillaire atmosphere
  - W6 = agent-docs consolidation
  - W7 = contributing / repo-model docs
- `unity-port-planning` — legacy planning-only branch. Being phased
  out now that specs/plans/audits live in `agent_docs/` on the
  shipping lineage. Kept read-only for historical reference.

## End-of-project checklist

1. **Build** via the `rtx-build` skill (pre-cleans `nv-private/` +
   `tests/rtx/dxvk_rt_testing/`, then `PerformBuild -release`).
2. **Ship** only when exit code 0 and zero compile errors.
3. **Author** commits as the human contributor in the repo's git
   config. No AI co-author trailers unless explicitly requested.
4. **Fast-forward only.** Never `--force`, never `--force-with-lease`
   on `modern-games-sdk-api`. If a push is rejected as non-ff, stop
   and investigate.
5. **Ask** before any push to a remote with downstream readers —
   that includes canonical (`RemixProjGroup/dxvk-remix`) and any
   maintainer's personal fork (`kim2091/dxvk-remix`) that contributors
   pull from.
6. **Open a PR** to canonical `modern-games-sdk-api` rather than
   direct-pushing, even if you have write access — PR review is the
   discipline checkpoint.

## Project-local skills

Reusable skills for this codebase live in **`.claude/skills/`**. Invoke
via the `Skill` tool by name:

- **`rtx-build`** — wraps the proper meson/ninja build with the
  required `nv-private/` + `tests/rtx/dxvk_rt_testing/` pre-cleanup,
  runs in the background, reports exit code + error count. Use
  whenever the user asks to build or compile-check.

## Don't

- Don't push to `origin` (NVIDIA upstream) — that's read-only.
- Don't force-push anything, ever.
- Don't skip hooks (`--no-verify`) unless explicitly asked.
- Don't add a feature, refactor, or "cleanup" beyond the task scope.
- Don't edit files under `submodules/` or `external/` — those are
  third-party pins.
- Don't create W-branches on the canonical repo. They belong on
  contributor forks only.

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
