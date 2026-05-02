# CLAUDE.md — agent entry point for Remix Plus

This repo is **Remix Plus** — a port of NVIDIA's `dxvk-remix` focused
on enabling **modern games to run through the Remix SDK API**, derived
from the gmod-rtx community fork. It carries API surface, capture/
replacement, hw-skinning, tonemap, and atmosphere work needed for
API-driven game integrations. Unity is one such integration path
among others; the port itself is not engine-specific.

Contributor-facing docs live in **`docs/`**. The contribution flow
and fork discipline are documented in
**[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md)**. The authoritative
inventory of every upstream file the fork touches is
**[`docs/fork-touchpoints.md`](docs/fork-touchpoints.md)**.

## Repo structure — fork + PR model

Classic GitHub **fork + PR** workflow:

**Canonical repo** — [`github.com/RemixProjGroup/dxvk-remix`](https://github.com/RemixProjGroup/dxvk-remix).
The PR target. Default branch is `modern-games-sdk-api`.

**Personal forks** — anyone contributing forks the canonical repo.
Each contributor's fork holds their own feature branches and a
personal tracking copy of `modern-games-sdk-api`.

For the full contribution flow (setup, branch conventions, build,
discipline, PR submission), point contributors at
[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md).

## Fork discipline — read before editing upstream files

The port uses a **fork-touchpoint pattern** to minimize rebase cost
against upstream NVIDIA `dxvk-remix`. The pattern is documented in
[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md). Short version:

- **Prefer hooks over inline tweaks.** When adding fork logic to an
  upstream file, extract the body into a `src/dxvk/rtx_render/rtx_fork_*.cpp`
  module and leave a one-line dispatch into `fork_hooks::…` in the upstream
  file. Fork-owned file naming: `rtx_fork_<subsystem>.cpp/h`.
- **Inline tweaks are allowed but capped.** If a change is truly small
  (< 20 LOC) and structurally can't be a hook (e.g. struct-field addition,
  enum bit), inline is fine. Anything bigger should be refactored.
- **Fridge-list invariant.** Every commit that touches an upstream file
  MUST also update [`docs/fork-touchpoints.md`](docs/fork-touchpoints.md)
  in the same commit. The PR template enforces this.
- **Audit script.** Run `scripts/audit-fork-touchpoints.sh` before
  committing upstream edits.

## Branch conventions (personal fork)

- `main` — NVIDIA upstream mirror. Never commit port work here.
- `modern-games-sdk-api` — your fork's tracking copy of canonical's
  default branch. Fast-forward this to a feature-branch tip when the
  branch is ready to ship; then open a PR to canonical.
- Feature branches — name them however you like on your fork. The
  maintainer's internal convention is `unity-workstream/<NN>-<short-name>`
  (e.g. `unity-workstream/05-hillaire-atmosphere`) — ad-hoc numbering,
  not monotonic. Contributors are not required to follow this scheme;
  see [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) for guidance.

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
- Don't create feature branches on the canonical repo. They belong
  on contributor forks only.

## Gmod reference repo

Read-only source for porting features from the upstream gmod-rtx
community fork. Branches of note: `origin/unity` (the port's baseline —
named after the gmod branch, carries the Remix API / capture /
hw-skinning / atmosphere work that matters for modern-game integrations),
`origin/gmod-ex` (Garry's Mod game-specific — usually out-of-scope).
Never commit or push to this repo.
