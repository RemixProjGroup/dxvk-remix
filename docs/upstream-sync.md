# Upstream review log

Remix Plus never rebases onto `upstream` (NVIDIAGameWorks/dxvk-remix `main`). Once a month, review the upstream commits since the last-reviewed commit and record the review here. Policy: `AGENTS.md`, Upstream Policy.

How to review:
1. `git fetch upstream`, then `git log <last-reviewed>..upstream/main`.
2. Give every commit one row with a verdict and a one-line reason: **take** (port it), **skip**, or **maybe** (decide later, say what would decide it).
3. Port each take on its own branch, one feature at a time, after the license check. Fill in the port commit when it lands.
4. Add a section per review: the date, the commit range, and the table. End the file with the next last-reviewed commit.

Pass 1 used **idea** for what is now **maybe**, and its takes were cherry-picked with `-x`.

## Pass 1: 2026-10-03, `b474e56b0..324aba8f8` (15 commits)

| Upstream | Date | Subject | Decision | Notes |
| --- | --- | --- | --- | --- |
| `ec8132d67` | 09-21 | Windows on Arm build instructions | skip | ARM |
| `94ec487e2` | 09-22 | RTXIO arm64 binaries | skip | ARM |
| `273376713` | 09-22 | REMIX-5768 camera turns when swapping Remix menus | take → `78a832ca1` | Bridge DirectInput hook; helps D3D9 bridge games, not API hosts. AGENTS.md conflict: kept both. |
| `84acb5227` | 09-22 | REMIX-5178 eyes in virtual instances | take → `697b56c25` | Clean. New debug view 52 (Eye Regions), no clash with fork indices. |
| `2ccdd8212` | 09-22 | Medium test image export fix | skip | Test harness |
| `6da218ee8` | 09-23 | REMIX-5137 fallback light volumetric radiance scale | take → `8526d7618` | Clean, new option with GUI. |
| `c7d3aae50` | 09-29 | Image test metrics as post-warmup averages | skip | Test harness |
| `923c61528` | 09-29 | REMIX-4964 reduced VRAM (RTXDI, FG, PSR) | take → `0b2886492` + `ccace60d0` | Include conflict in `rtx_resources.cpp`. `m_rtxdiReservoirBuffer` is now an `AliasedBuffer`: light BVH binding adapted (`ccace60d0`). With gradients off (RR + no ReSTIR GI) RTXDI uses 2 reservoir pages; light BVH select reads and writes its own pixel in page 0, which is safe. |
| `5229c042a` | 09-29 | DLSS-NR on Windows-on-Arm | skip | ARM |
| `271925bee` | 09-30 | REMIX-4964 dead RR code, GBuffer/decal refactor, NEE cleanup | idea | 391-line `geometry_resolver.slangh` rewrite collides with fork GBuffer work (split stages, decals, PSR). Revisit when the renderer split touches the resolver. |
| `9d98e3283` | 09-30 | arm64ec API shim on WoA | skip | ARM |
| `d65fdf029` | 10-01 | ARM64 / ARM64EC GitHub artifacts | skip | CI |
| `11abe8361` | 10-01 | Output installation in GitHub workflow | skip | CI |
| `e048263db` | 10-02 | Sparse rendering: obsolete options, defaults, composite/RTXDI toggle fixes | idea | Fixes are wanted but it removes options and touches RTXDI temporal/spatial reuse next to the light BVH. Port after an in-game check that sparse rendering is used at all. |
| `324aba8f8` | 10-02 | RTXIO bump for arm64 | skip | ARM |

## Between passes: SHaRC only

The SHaRC work compared upstream's `303c81336` (SHaRC 1.8.3 integration), `90d74fbe1` and `8ce1b446f` with ours and ported them by hand (`bd59552f1` .. `90da7c67f`). The rest of the range was not reviewed.

## Between passes: DLSS Frame Generation preset, 2026-10-26

Ported by hand: the DLFG Lite half of `09808cc70` (its DLSS-NR half was ported earlier), the frame generation labels of `97355c1a1` (the DLSS-NR renames were not taken; the menu keeps its own names), and `c1a891bf8` (pipeline-handle race and command-list recycle order; the duplicate acquire wait was already fixed by `616df3dc2`). The rest of the range was not reviewed.

## Between passes: bridge correctness, 2026-10-29

Ported by hand: `3f75d648b` (bridge query completion, shared heap upload snapshots and allocator, present teardown). It corrects `8bbf141e6`, upstream's copy of our bridge transport work, which we already had in our own form. Not taken: the module bridge read position publish, the `consumeResponse` split and the bridge app tests (see the changelog entry). The rest of the range was not reviewed.

## Starting point, 2026-10-15

- `upstream/main` is at `029602f83` (2026-10-15).
- Last reviewed: `324aba8f8` (2026-10-02). `324aba8f8..029602f83` is 51 commits, not yet reviewed, apart from the three SHaRC commits above.

## Pass 2: 2026-10-29, `324aba8f8..0ba9973fd` (51 commits)

Ports land on four branches: fix/20261029-upstream-small-fixes, fix/20261029-upstream-rtxdi-best-light, fix/20261029-upstream-sparse-fixes and fix/20261029-upstream-crash-fixes. feat/20261029-upstream-sparse-autorate changes the sparse rendering rates and waits for a decision.

| Upstream | Date | Subject | Decision | Notes |
| --- | --- | --- | --- | --- |
| `e048263db` | 10-02 | Sparse rendering: obsolete options, defaults, composite/RTXDI toggles | taken earlier | Ported with the composite work (`8818ea230`). |
| `c4165bc3b` | 10-05 | Several improvements, optimizations and fixes | partly taken | The alpha blend 1/rate brightness fix is on fix/20261029-upstream-sparse-fixes. Maybe: the single-launch PSR resolve and the split Integrate NEE variants, which collide with our G-buffer split. |
| `ca4d79cac` | 10-06 | Sparse rendering: compact G-buffer | skip | We compact with our own active pixel list. |
| `d34850313` | 10-06 | Restructure instance identification | skip | The D3D9 v2 frontend has its own draw identity. |
| `303c81336` | 10-07 | SHaRC 1.8.3 integration | taken earlier | Hand port `bd59552f1` .. `90da7c67f`. |
| `4b31eac40` | 10-07 | Credits: Pal Mezei | take | fix/20261029-upstream-small-fixes. |
| `6fe1548b3` | 10-09 | Runtime RtxOption namespaces | maybe | 2k lines in the option system. Decide together with `5ff24bc34`. |
| `5ff24bc34` | 10-09 | Per-GPU graphics preset overrides | maybe | 3k lines. Useful for our presets, but needs `6fe1548b3`. |
| `a45396e36` | 10-09 | RTX Spark Auto defaults | skip | ARM64 hardware. |
| `90d74fbe1` | 10-11 | Cache sampling roughness behind transparent surfaces | taken earlier | `bd59552f1`. |
| `e675d2ef5` | 10-12 | Drag speed after Alt+Tab | take | fix/20261029-upstream-small-fixes. |
| `8ce1b446f` | 10-12 | SHaRC update records by preset | taken earlier | `bd59552f1`. |
| `0618cab63` | 10-12 | RTXDI spatial reuse per-pixel offsets | take | fix/20261029-upstream-rtxdi-best-light. |
| `51fef8423` | 10-12 | HDRemix publishing | skip | Packaging. |
| `e14940d58` | 10-13 | Sparse rendering auto parametrization | take, split | Fixes on fix/20261029-upstream-sparse-fixes; the rate scaling and the sparse-aware alpha search on feat/20261029-upstream-sparse-autorate (user decision). Its edit of RtxdiApplicationBridge.slangh (NVIDIA notice) is redone as a wrapper in rtxdi_application_bridge_ext.slangh. The sky probe option is not taken. |
| `ebf5cb8a8` | 10-13 | Release publishing CI | skip | CI. |
| `9919be380` | 10-13 | Shader optimization pass | partly taken | The alpha blend part with the composite work (`8818ea230`). Maybe: the rest (NRC resolve, integrate indirect, skinning). |
| `8459d80f6` | 10-13 | Comment rules in the style guide | skip | Our AGENTS.md has its own. |
| `a04738df3` | 10-13 | Bridge: x64 server fallback | skip | Only matters for ARM64 installs. |
| `57bba448d` | 10-13 | Bridge: server's Remix API init status | maybe | Helps API hosts through the bridge; check with the bridge rework. |
| `92dd62632` | 10-13 | Move publishing doc | skip | Docs. |
| `a9747db5a` | 10-13 | Nsight capture on arm64ec | skip | ARM. |
| `ab9575cb2` | 10-13 | CreateMesh refactor | skip | Refactor only. |
| `ec3146421` | 10-14 | Minimum driver 616.92 | skip | Would refuse to start on older drivers for no feature we use. |
| `196e02a88` | 10-14 | AGENTS.md refresh | skip | Docs. |
| `b205e8799` | 10-15 | Particle spawn normal normalization | take | fix/20261029-upstream-small-fixes. |
| `415d612ae` | 10-15 | Sky cube helpers moved | skip | Refactor only. |
| `3af4a39b7` | 10-16 | Sparse volumetrics on primary hits off by default | take, decision | Default flip on feat/20261029-upstream-sparse-autorate. |
| `92f79132c` | 10-16 | View distance mode for noise-free primary blend | maybe | A new view distance mode; port when a game asks for it. |
| `09808cc70` | 10-16 | DLSS NR update and DLFG Lite | taken earlier | 10-26. |
| `8d418d455` | 10-16 | arm64 package readme | skip | ARM. |
| `273a023f5` | 10-16 | Per-game SHaRC voxel size | skip | Our SHaRC options differ (`rtx.sharc.*`); set it in a game's config if wanted. |
| `9344b88d0` | 10-21 | Surface material index refactor | skip | Refactor only. |
| `97355c1a1` | 10-21 | DLSS NR / FG preset labels | taken earlier | Frame generation labels only (10-26). |
| `c8a654180` | 10-21 | Secret replacements after merges | take | fix/20261029-upstream-small-fixes. |
| `c5f0f8e7d` | 10-21 | Texture transform attributes in the material API | maybe | Remix API feature; port with an API request. |
| `ff38fdb93` | 10-22 | SHaRC firefly clamp 100 | skip | Our SHaRC clamps with `rtx.sharc.maxDepositRatio` (20); try 100 in a game config before changing the default. |
| `8bbf141e6` | 10-19 | Bridge per-frame IPC cost | have equivalent | Our own bridge rework (A3a sent-state cache, A4 non-blocking queries, the transport changes) covers it; the options and `d3d9_sent_state.h` are already in the trunk. |
| `da9aeaa35` | 10-20 | SetConfigVariable option mutex | take | fix/20261029-upstream-small-fixes, with its credits line. |
| `b6fc4386f` | 10-23 | Packman bootstrap 8.7.0 | skip | Packaging; take when a package needs it. |
| `c1a891bf8` | 10-26 | Pipeline-handle race, command-list recycle | taken earlier | 10-26. |
| `3f75d648b` | 10-23 | Bridge: D3D9 synchronization and bounded upload memory | maybe, high | Correctness fixes for `8bbf141e6` (query completion, upload ownership, present teardown). 2.4k lines over our bridge transport changes; needs a bridge build and 32-bit game tests. Next bridge task. |
| `eada2cb93` | 10-26 | Eye material on view models | maybe | Medium port (instance manager, eye shader); next pass. |
| `c57f4644c` | 10-27 | RTXDI best light fixes, RTXDI pin 639c99f | take | fix/20261029-upstream-rtxdi-best-light. The SDK stays unmodified; the jitter option is not added. |
| `54b144a0b` | 10-12 | HL2RTX startup responsiveness | maybe | Async NGX init and deferred workers; touches startup paths we changed. |
| `da72af6b3` | 10-27 | GPU trace skill | skip | Agent tooling. |
| `8f8ac5771` | 10-27 | HdRemix AOV outputs | skip | HdRemix. |
| `8e2dd3931` | 10-27 | Register count reductions | taken earlier | Bit-identical parts on 10-29; the rest changes images. |
| `ca69e0453` | 10-28 | Eyes under the free camera | take | fix/20261029-upstream-small-fixes. |
| `0384d427d` | 10-28 | Painkiller crash fixes | take | fix/20261029-upstream-crash-fixes, without the bone staging change, which our D3D9 path already covers. |
| `6d1b3eab3` | 10-28 | First-hit decals before cached inline PSR | skip | Builds on upstream's first-hit decal resolve before inline PSR (`geometryResolverResolveDecalBeforePSR`), which our G-buffer does not have. |

Last reviewed: `0ba9973fd`.
