# Upstream DXVK D3D9 triage

This is the review of upstream DXVK's D3D9 commits since this tree forked, with a verdict for each: port, skip, later, or already present. The first batch of ports is listed in the 2026-11-03 entry of [CHANGELOG.md](CHANGELOG.md).

## Fork base

This tree forked from upstream DXVK at **6a3de28f** "[dxvk] Repurpose updateBuffer" (2022-02-12), which is `v1.9.4-38`, 76 commits before v1.10.

Evidence:
- The repository carries DXVK's history up to the fork. Matching author date and subject against upstream, every upstream commit up to and including 6a3de28f is in our history, and the 50 commits before it are all present without gaps. After it, only 13 upstream D3D9 commits appear, and they are scattered cherry-picks (two are marked `[CHERRY-PICK]` / `[CHERRY-PICKED]` in their subjects).
- A `git merge-base` with upstream returns a 2017 commit because NVIDIA's import rewrote the history after 2017-11-16, so the match is by date and subject, not by hash.
- File comparison agrees: counting changed lines in files NVIDIA rarely touched (`src/dxso`, `src/spirv`, `d3d9_format.cpp`, `d3d9_caps.cpp`, `d3d9_util.cpp`, `d3d9_fixed_function.cpp`, `config.cpp` and a few more), the distance to upstream tags falls from 3265 lines at v1.7.3 to a minimum of 2185 at v1.9.4 and rises again to 2221 at v1.10 and 2297 at v1.10.1.

Upstream reference: v3.1.1 (cf0a0d90). Upstream is zlib-licensed. Ported code keeps the existing license headers, and Robin Kertels, whose code most ports carry, is in `LICENSE`, `CREDITS.md` and `README.md` as in upstream's `LICENSE`.

## Scope and method

The list is every non-merge upstream commit in `6a3de28f..cf0a0d90` that touches `src/d3d9`, `src/dxso` or `src/util/config`: 1414 commits, of which 347 change only `config.cpp`. Commits that touch only `src/dxvk` are not listed. Backend commits that also touch D3D9 files are.

Remix still uses DXVK's D3D9 state tracking, DXSO shader translation (vertex shaders for capture, raster fallback, UI), fixed-function emulation and the swap chain, so fixes there apply. Commits that depend on DXVK 2.x foundations this tree lacks are skipped: the Vulkan 1.3 requirement, graphics pipeline libraries, the new binding and descriptor model, the 2.5 memory allocator and resource model, the 2.x presenter, and the 2026 dxbc-spirv shader compiler and constant buffer rework. The D3D8 frontend is not in this tree.

Verdicts:
- **port**: ported on this branch. The reason column names our commit.
- **present**: already in this tree, cherry-picked by NVIDIA or fixed equivalently.
- **skip**: not applicable, already handled differently, refactor only, or tied to a foundation this tree lacks.
- **later**: worth a look in a later batch.

The commits ported, marked present, and the ones with a specific reason were reviewed against this tree. The rest were classified from their subject and files (refactor, perf, fix and so on), and their reasons say so in general terms ("Fix; needs a check against this tree", "Not reviewed in detail").

## Summary

1414 commits: 99 ported, 14 already present, 651 later, 650 skipped.

By class: 341 correctness, 239 compatibility (mostly game profiles), 27 perf, 430 refactor, 215 irrelevant here (other APIs, D3D8, build), 162 other.

## Ported in the first batch

| Ours | Subject | Upstream |
|---|---|---|
| cae72a3c3 | D3D9: UpdateTexture with different mip counts and automatic mips | c12cd195, b2f22d57, bfd47ec8 |
| 0d2e061a1 | D3D9: fix fallthroughs in swap chain format selection | b2d96ee0 |
| 10b1708b1 | D3D9: don't expose the D32 depth format | 1f88ee59 |
| cef72ab90 | D3D9: disable culling for an invalid cull mode | 4e34c29c |
| 964e8490e | D3D9: fix a crash in StretchRect with null render targets | 5b7406fe |
| 0e9912ca6 | D3D9: resolve stencil with sample zero | e406484b |
| cf0b4bcc9 | D3D9: accept an unknown windowed back buffer format | 95995041, 64d22606 |
| aeba45040 | D3D9: allow scratch surfaces in GetFrontBufferData | ce48b57f |
| 918a5dd4f | D3D9: clamp multisample counts to what the GPU supports | 9e110cd3, 5ae5476d |
| 5bf92c4c6 | D3D9: capture all vertex shader constants in vertex state blocks | 0462454d |
| 5703b8f77 | D3D9: fix a query reset counter underflow | ecc23023 |
| 97e5c2b6b | D3D9: fix an off-by-one in shader-defined constant copies | 9cde0b57 |
| e959090c0 | D3D9: return an empty buffer slice for out-of-bounds offsets | ea314980 |
| 82ac718cb | D3D9: fix the default light | 9e422a2b |
| 55044facd | D3D9: no fixed-function specular on back-facing surfaces | da814668 |
| 226f7ea84 | D3D9: state block capture keeps the recorded stream offset | dc3dd0f8 |
| a4da6ef0e | DXSO: zero-wins lrp with strict float emulation | 630fee59, 18a801de |
| 510ab2173 | DXSO: pixel shader inputs load all components | c585ea25 |
| 6edefc47c | DXSO: fix the render target mask for SM1 pixel shaders | 4b108460 |
| 7bc2159a8 | DXSO: fix invalid SPIR-V for crs with a scalar result | 82ebc29e |
| 80caccb68 | DXSO: centroid color inputs for SM3 pixel shaders | 5962be44 |
| 61cee23d6 | DXSO: don't assume 32 temporary registers | 037669f7 |
| bde1d61bd | DXSO: initialize the highest defined constant | fed51e6c |
| acf5be13a | DXSO: implement the bem instruction | 404c984f |
| 72330241d | D3D9: skip 0x0 clears | 860237e7 |
| d546dde72 | D3D9: fail GetRenderTargetData on mismatched sizes | ab359318 |
| fde76cc9a | D3D9: disable fetch4 when binding an incompatible texture | 9e1ecec7 |
| 7d083806b | D3D9: don't advertise multisampled INTZ | 0fc5c84e |
| 3c6603a74 | D3D9: fix integer divisions in YUV conversion | 9c5551cb |
| 75f3972ea | D3D9: ignore SetLOD on non-managed textures | 098e41a3 |
| fcd767764 | D3D9: rename d3d9.apitraceMode to d3d9.cachedDynamicBuffers | 5fd025c5, d2759c20 |
| ee691f451 | Config: complete The Sims 2 profile | fe9c875f, fb8f79e4 |
| de51fee1d | D3D9: add d3d9.allowDirectBufferMapping | 6d71eea5, cf4c5c34 |
| 06b062a0f | Config: strict float emulation profiles | 4f56e72d, 43b79bcb, a78c1bd6, 968f0cdb, 661f8b5b, a44dfabe, f254afb4, 3d401690, b52156dd, 99b367cd, 0a699fdd |
| 1a7087321 | Config: memory tracking profiles | fe9a24d1, 2b16134b, e05e063d, b707ca13, a3ba8fb4, 61b91ea4, 448fd8fa, b3da8104 |
| e86b7fd8d | Config: GPU identity profiles | cc9266ed, d8eb4d0d, f27e3c9f, c27ef1c4 |
| 9404c869c | Config: cached dynamic buffer profiles | 1a2e724c, a63a5753, f0ff0007, 34d8e65f, b8ce4148, dacb8b43, 4b4d323e, fe0e02de, d31b7997, 11ec6035, 98fa338b, 2c0a94a0, 6bdca15e, 3b90f5a7, bc08cac2 |
| d9a4ea386 | Config: surface, buffer and frame pacing profiles | 9d6804e4, 5c56fa0d, 8ba5256d, bb6d1b3b, ce87bec4, 5621bd01, b06140af, 83dc4678, e6873031, 6e43bf6d, 6be1f6d7 |
| 1ccee086b | D3D9: include the eighth texture stage in the fixed-function sampler mask | 395ac388 |
| 94b9d5cdf | DXSO: clamp exp and expp results with fast float emulation | e9a0fec5 |
| 5a051304c | D3D9: clamp the stage and type in texture stage state calls | 1c6fc7b5 |
| 9c47c8d76 | D3D9: reject shader constant ranges that overflow | 92523fc0 |
| 6a40169da | D3D9: don't record MultiplyTransform in state blocks | 4f0c2176, 9c012c8e |
| c6a1b9eb2 | D3D9: record and capture lights in state blocks | 1a3b576d, 4796eb0b, 0d28be4a |
| 01097195d | D3D9: initialize buffer map flags | a7977232 |

## Adapted rather than copied

- 5fd025c5 + d2759c20: `d3d9.apitraceMode` is renamed to `d3d9.cachedDynamicBuffers`. Upstream's current name, `d3d9.cachedWriteOnlyBuffers` (ae26b84d), belongs to its buffer placement rework (c1e9cb43), which is not ported, so profiles that use it are ported with `d3d9.cachedDynamicBuffers`.
- da814668: upstream declares the N.L comparison with a float result type, which is invalid SPIR-V. It uses `bool_t` here.
- dc3dd0f8: this tree has no `D3D9DeviceState` type, so the capture path names `D3D9CapturableState`.
- 098e41a3: there is no `D3D9CommonTexture::GetPool`, so the pool comes from the description.
- 6d71eea5 + cf4c5c34: the map mode is stored at creation, as upstream does, because it depends on the new option.
- 92523fc0: the same overflow check is added to `GetShaderConstants`.
- 1a3b576d: upstream's `bitvector::set` writes every bit into dword 0, which is undefined for indices of 32 and above. It uses `idx / 32` and `idx % 32` here.
- e9a0fec5: hand-merged, with the `[[fallthrough]]` from 5d4c8a6b.
- Profiles: options this tree lacks are left out (Alien Rage's `dxgi.hideNvidiaGpu`), negative `d3d9.maxFrameRate` caps are left out (this tree's limiter treats negative values as off), and the Tomb Raider profile gets upstream's `d3d9.longMad` for Legend because upstream's later global default (1e5b78e8) is not ported.

## Candidates for the next batch

- 9a8009cd and d7636d09: negative `d3d9.maxFrameRate` (limit only when the game runs faster than n). 24 upstream D3D9 profiles and 13 D3D8 ones use it (Dead Space 1 and 2, Sonic Adventure 2, Tomb Raider, Bionic Commando, Beyond Good and Evil, F.E.A.R. and others). It needs a presenter-side limiter mode.
- 1e5b78e8, then a71603a2: `longMad` on by default, then lrp as mul+add. This changes vertex shader math for every game, so it needs an A/B on Remix capture (z-fighting between passes, hash stability).
- c1e9cb43 and the follow-ups (e553bcce, 39479c0f, 57ec2b2d, 3972f883): the Windows-like buffer strategy. Remix reads mapped vertex data on the CPU, so the write-combined placement needs a perf check first.
- Profiles that need options this tree lacks: `d3d9.deviceLossOnFocusLoss` (10 profiles), `d3d9.countLosableResources` (9), `d3d9.hideNvidiaGpu` / `d3d9.hideAmdGpu` (10), `d3d9.textureMemory` (5), `d3d9.modeCountCompatibility` (2), `d3d9.ignoreDefaultBufferLockRange` (2), `d3d9.forceDrawTimeBufferUpload` (2), `d3d9.supportCubeDepthFormats` (1), `d3d9.extraFrontbuffer` (1).
- The D3D8 section of upstream's profiles: those games reach this runtime through a D3D8-to-D3D9 wrapper, so their `d3d9.*` options would apply.
- The upstream vendor-id and `floatEmulation = Strict` profile for one D3D9 game: it changes that game's renderer path, so check it against existing Remix setups for the game before porting.
- 8b6cbda6: reports `MaxVertexBlendMatrixIndex = 0` in adapter caps, which can steer games to CPU skinning and affects Remix's skinning capture.
- 6e164fc8 (ignore lights with invalid types), 11db6d69 (ProcessVertices reference leak), 49854dbf (saturate viewport depth), 1bc6f966 (clamp cube address mode), e30b7835 and b30a4f0c (depth bias representation; Remix has its own decal depth bias), and the fixed-function fixes from 2024 to 2026 (52f04ca3, 9e26964a, 82bf8b1e, ef81c30f, 8d252af1, 3a4c6fa3, e8c80a8d).

## Findings outside the ports

- `D3D9CommonBuffer::CreateBuffer` asks for `HOST_VISIBLE | HOST_CACHED | DEVICE_LOCAL` for directly mapped buffers. Most discrete GPUs have no memory type with both cached and device-local, and the allocator then drops both optional flags at once, so these buffers can end up in uncached memory even though Remix reads them on the CPU. Upstream noted the same allocator behavior in d2759c20. This is not changed here: the fix belongs with a measured buffer placement change.
- The Dragon Nest profile spelled its key `"d3d9.memoryTrackTest "` with a trailing space, so it never applied. It is fixed in the memory tracking profile commit.

## All commits

Oldest first. Dates are author dates.

| Upstream | Date | Subject | Class | Verdict | Reason |
|---|---|---|---|---|---|
| 4d9b464f | 2022-02-14 | [d3d9] Use new waitForResource method | refactor | skip | Refactor or cleanup |
| 4e464327 | 2022-02-14 | [util] Bump maxImplicitDiscardSize for Quantum Break | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e70f9f92 | 2022-02-14 | [util] Enable apitrace mode for Nier Replicant | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 752f98cf | 2022-02-17 | [d3d9] Use initImage to clear uninitialized image resources | refactor | skip | Refactor or cleanup |
| 6ae54881 | 2022-02-17 | [d3d9] Use initBuffer method | refactor | skip | Refactor or cleanup |
| c12cd195 | 2022-02-02 | [d3d9] Handle different mip chain lengths in UpdateTexture | correctness | port | Ported in cae72a3c3 |
| 917a8d00 | 2022-02-12 | [d3d9] Track last staging resource usage with a sequence number | correctness | present | cherry-picked by NVIDIA |
| 97ae14b6 | 2022-02-12 | [d3d9] Synchronize only to given sequence number in WaitForResources | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 6d71eea5 | 2022-02-12 | [d3d9] Store buffer map mode in D3D9CommonBuffer | correctness | port | Ported in de51fee1d |
| cf4c5c34 | 2022-01-26 | [d3d9] Add option to disable direct buffer mapping | correctness | port | Ported in de51fee1d |
| fd23dcef | 2022-01-28 | [d3d9] Disable direct buffer mapping for RE games | compatibility | later | D3D9 profile; not in the first batch |
| 40444c1f | 2021-07-13 | [d3d9] Unify texture uploads | correctness | present | cherry-picked by NVIDIA |
| dde83717 | 2022-02-13 | [d3d9] Rename WrittenByGPU to NeedsReadback | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| b163a99b | 2022-02-14 | [d3d9] Clean up texture locking | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 29d88127 | 2022-02-21 | [d3d9] Don't set NeedsReadback for POOL_SYSMEM textures | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| a37cd464 | 2022-02-22 | [util] Set maxDynamicImageBufferSize for Total War: Warhammer III | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a81c653b | 2022-02-21 | [d3d9] Fix synchronization after readback | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| b42c0725 | 2022-02-24 | [d3d9] Fix sysmem readback | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 2bceeff5 | 2022-02-28 | [d3d11] Replace apitrace mode option with something more granular | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 5822a8de | 2022-02-28 | [util] Enable cached vertex and index buffers for FFXIV | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| f14bccc8 | 2022-03-03 | [util] Use CPU-cached constant buffers for Anno 1800 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| bb178baa | 2022-03-03 | [util] Enable cached vertex and index buffers for The Evil Within Large performance win. | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d4d4f0ca | 2022-03-09 | [d3d9] Fix default initialization of some state values | refactor | skip | Refactor or cleanup |
| 53b9a723 | 2022-03-09 | [d3d9] Set initial dirty state flags | refactor | skip | Refactor or cleanup |
| b2d96ee0 | 2022-03-09 | [d3d9] Fix fallthroughs in PickFormats | correctness | port | Ported in 0d2e061a1 |
| a5b0f289 | 2022-03-09 | [dxso] Fix ExpP fallthrough | refactor | skip | No-op here: our ExpP already breaks for SM1 and falls through for SM2+ |
| f4ab14ca | 2022-03-09 | [dxso] Fix potential fallthrough in RasterizerOut | correctness | later | Fix; needs a check against this tree |
| dc6c350c | 2022-03-09 | [d3d9] Add fallthrough comment to SetRenderState | refactor | skip | Refactor or cleanup |
| 5d4c8a6b | 2022-03-11 | [dxso] Fix ExpP instruction on Shader Model 2+ | correctness | later | Fix; needs a check against this tree |
| 02d3e0d3 | 2022-02-23 | [dxvk] Add shared handle access to DxvkImage memory. | correctness | present | cherry-picked by NVIDIA |
| 6640cc35 | 2022-02-23 | [d3d9] Add support for shared IDirect3DTexture9 resources. | correctness | present | cherry-picked by NVIDIA |
| 1d1d8ade | 2022-03-12 | [d3d9] Fix shared handle check for exporting images | correctness | present | cherry-picked by NVIDIA |
| 59e3c969 | 2022-03-12 | [d3d9] Fix texture formats that can be exported | correctness | present | cherry-picked by NVIDIA |
| caa83247 | 2022-03-13 | [d3d9] Mark backend image as shared for shared resources | correctness | present | cherry-picked by NVIDIA |
| 4c429f04 | 2022-03-13 | [util] Add another weeb game to the list of workarounds | compatibility | later | D3D9 profile; not in the first batch |
| a6357a25 | 2022-03-11 | [d3d9] Fix CS thread synchronization for directly mapped buffers | correctness | later | Fix; needs a check against this tree |
| 5f4fc562 | 2022-03-12 | [d3d9] Update texture sequence number AFTER using it | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 115385d1 | 2022-03-15 | [d3d9] Update buffer seq number in FlushBuffer | other | later | Not reviewed in detail |
| 1f88ee59 | 2022-03-16 | [d3d9] Don't expose D32 format | correctness | port | Ported in 10b1708b1 |
| 630fee59 | 2022-03-16 | [dxso] Implement zerowins for Lerp. | correctness | port | Ported in a4da6ef0e |
| f1efc9dc | 2022-03-16 | [dxso] Emit spirv OpCross if we can. | other | later | Not reviewed in detail |
| 2550cff1 | 2022-02-22 | [build] Cleanup build system. | irrelevant | skip | Build or platform change |
| a010397f | 2021-04-30 | [util] Move DecodeD3DColor to util | refactor | skip | DXVK backend change that follows the 2.x backend |
| 47b1ab52 | 2021-04-30 | [d3d9] Implement D3D9GlobalAnnotationList | other | later | Not reviewed in detail |
| 787a9795 | 2021-04-30 | [d3d9] Implement D3D9UserDefinedAnnotation | other | later | Not reviewed in detail |
| 937a60c8 | 2021-04-30 | [d3d9] Add hidden exports for registering annotations | other | later | Not reviewed in detail |
| cf1cee04 | 2021-04-30 | [d3d11] Register annotation interfaces with D3D9 | compatibility | later | Interop / shared resources; low priority for Remix |
| b36ca2c7 | 2022-02-01 | [d3d9] Fix Visual Studio build to resolve 'operator !=' is ambiguous error for RECT | irrelevant | skip | Build or platform change |
| 0d54f716 | 2022-03-19 | [util] Enable cached dynamic resources for AC3 and AC4 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 119c1ece | 2022-03-24 | [util] Set frame latency to 1 for God of War | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| b2f22d57 | 2022-03-21 | [d3d9] UpdateTexture: Handle automatic mip gen properly | correctness | port | Ported in cae72a3c3 |
| 991a1161 | 2022-03-25 | [util] Enable d3d9.deferSurfaceCreation for Stranger of Paradise FFO | compatibility | later | D3D9 profile; not in the first batch |
| 4e34c29c | 2022-03-25 | [d3d9] Disable culling when the app passes an invalid value | correctness | port | Ported in cef72ab90 |
| b4efaa4e | 2022-03-26 | [util] Enable cached constant buffers for Frostpunk | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| ebdaf90f | 2022-03-28 | [util] Enable d3d9.deferSurfaceCreation for Atelier Sophie 2 | irrelevant | skip | Profile comment or layout change |
| f0ccd8fe | 2022-03-28 | [util] Limit Limbo to 60 fps | compatibility | later | D3D9 profile; not in the first batch |
| e7e7fa23 | 2022-03-30 | [dxvk] GPU query reset path | refactor | skip | DXVK backend change that follows the 2.x backend |
| 95a34139 | 2022-03-31 | [util] Fix typo in app profiles | irrelevant | skip | Profile comment or layout change |
| ce87bec4 | 2022-04-06 | [util] Force sampler type spec const for Star Wars TFU2 | compatibility | port | Ported in d9a4ea386 |
| b0ed9e30 | 2022-04-06 | [d3d9] Filter window messages when processing WM_ACTIVATEAPP. | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 957a305c | 2022-04-06 | [d3d9] Ignore multiple app activation window messages. | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 42c66c41 | 2022-04-06 | [d3d9] Calculate slice alignment when uploading straight from the mapping buffer | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 3672375b | 2022-04-08 | [util] Use cached constant buffers for Armored Warfare | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 10bab0c1 | 2022-04-09 | [dxso] Use new DxvkShader constructor | refactor | skip | DXVK 2.x binding / shader interface |
| ab95b61e | 2022-04-09 | [d3d9] Use new DxvkShader constructor for fixed-function shaders | refactor | skip | DXVK 2.x binding / shader interface |
| 98ec79f6 | 2022-04-09 | [d3d9] Use new DxvkShader constructor for SWVP emulation | refactor | skip | DXVK 2.x binding / shader interface |
| 427f51ed | 2022-04-09 | [d3d9] Use new DxvkShader constructor for format conversion | refactor | skip | DXVK 2.x binding / shader interface |
| 9302d33a | 2022-04-09 | [d3d9+util] Enable invariant position by default | other | later | Not reviewed in detail |
| 147f3738 | 2022-04-18 | [d3d9] Use strlcpy helper | other | later | Not reviewed in detail |
| bdafa16e | 2022-03-29 | [d3d9] Only bind depth buffer if the depth or stencil test is enabled | other | later | Not reviewed in detail |
| 3004026d | 2022-03-29 | [d3d9] Only bind RT if we actually write to it | other | later | Not reviewed in detail |
| 5b7406fe | 2022-05-10 | [d3d9] Fix crash when using StretchRect with NULL rts | correctness | port | Ported in 964e8490e |
| cde0fbe7 | 2022-05-18 | [util] Enable d3d9.deferSurfaceCreation for Small Radios Big Televisions | compatibility | later | D3D9 profile; not in the first batch |
| cc11bb82 | 2022-05-18 | [d3d9] Don't adjust window position and size in windowed mode. | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 51e56c04 | 2022-04-18 | [util] Enforce a maxAvailableMemory limit for Majesty 2 | compatibility | later | D3D9 profile; not in the first batch |
| 8c968307 | 2022-05-31 | Add d3d9.deferSurfaceCreation workaround for Scrapland (Remastered) (#2574) | compatibility | later | D3D9 profile; not in the first batch |
| 7bcf3e10 | 2022-05-31 | [util] Force SM1 for the Warhammer ROR Launcher (#2579) | compatibility | later | D3D9 profile; not in the first batch |
| 4ff7687d | 2022-05-31 | [util] Force SM1 for the Halo CE SPV3 launcher | compatibility | later | D3D9 profile; not in the first batch |
| 1862e4dc | 2022-05-31 | [util] Report Nvidia VendorId for Myst V | compatibility | later | D3D9 profile; not in the first batch |
| 279b4b7e | 2022-06-02 | [d3d9] Defer surface creation if no HWND is given to device | other | later | Not reviewed in detail |
| 4f56e72d | 2022-06-11 | [util] Enable strict float emulation for Sonic Adventure 2 | compatibility | port | Ported in 06b062a0f |
| 968f0cdb | 2022-06-12 | [util] Strict float emulation for Supreme Commander | compatibility | port | Ported in 06b062a0f |
| bd29fbd9 | 2022-06-13 | [util] Enable sampler type spec constants for SWTOR | compatibility | later | D3D9 profile; not in the first batch |
| 661f8b5b | 2022-06-14 | [util] Add Forged Alliance Forever to Supreme Commander | compatibility | port | Ported in 06b062a0f |
| 27163a6a | 2022-06-21 | [util] Fix built-in config options loging | irrelevant | skip | Profile comment or layout change |
| dee36be2 | 2022-06-25 | Added config for A Way Out (#2694) | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 0fb12277 | 2022-06-02 | [d3d9] Pass shader stage when binding format conversion resources | other | later | Not reviewed in detail |
| 4cc559d6 | 2022-06-02 | [d3d9] Pass shader stage when binding shader resources | other | later | Not reviewed in detail |
| 5edd8e92 | 2022-06-21 | [d3d9] Pass new binding info struct to various helper shaders | refactor | skip | DXVK 2.x binding / shader interface |
| ecbada30 | 2022-06-21 | [d3d9] Pass new binding info struct to fixed-function shaders | refactor | skip | DXVK 2.x binding / shader interface |
| e795f3f3 | 2022-06-21 | [dxso] Pass new binding info struct to shaders | refactor | skip | DXVK 2.x binding / shader interface |
| ab0c15ea | 2022-06-21 | [dxvk] Introduce DxvkContextType | refactor | skip | DXVK backend change that follows the 2.x backend |
| acf70501 | 2022-06-22 | [d3d9] Call endFrame at the end of each frame | other | later | Not reviewed in detail |
| db078615 | 2022-06-23 | [dxvk] Remove old pipeline layout implementation | refactor | skip | DXVK backend change that follows the 2.x backend |
| 76ba0339 | 2022-07-01 | [d3d9,dxso] Fix push constant validation errors | refactor | skip | DXVK 2.x binding / shader interface |
| e406484b | 2022-07-02 | [d3d9] Don't use VK_RESOLVE_MODE_AVERAGE_BIT_KHR for stencil resolves | correctness | port | Ported in 0e9912ca6 |
| 98dcd722 | 2022-07-02 | [util] Add workaround for Garden Warfare 2 (#2700) | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| f95f5418 | 2022-07-02 | [util] Limit Bionic Commando to 60fps (#2685) | compatibility | later | D3D9 profile; not in the first batch |
| 2e7e9eac | 2022-06-30 | [dxvk] Require EXT_robustness2 and the null descriptor feature | refactor | skip | DXVK 2.x binding / shader interface |
| 186dc293 | 2022-06-30 | [d3d9] Remove bound spec constants from fixed-function shaders | refactor | skip | DXVK 2.x binding / shader interface |
| 735e7416 | 2022-06-30 | [d3d9] Add spec constant for null samplers | refactor | skip | DXVK 2.x binding / shader interface |
| c7afe0dd | 2022-06-30 | [dxso] Use new bit mask spec constant to determine whether textures are bound | refactor | skip | DXVK 2.x binding / shader interface |
| 7c4d6028 | 2022-06-30 | [dxvk] Change spec constant IDs | refactor | skip | DXVK 2.x binding / shader interface |
| 03ac5775 | 2022-07-02 | [util] Add workaround to fix missing sun & light shafts in Beyond Good And Evil | compatibility | later | D3D9 profile; not in the first batch |
| f2b1805d | 2022-06-09 | [dxvk] Allow non seamless samplers. | refactor | skip | DXVK backend change that follows the 2.x backend |
| cd8e2360 | 2022-06-09 | [d3d9] Use non seamless samplers if supported. | other | later | Not reviewed in detail |
| 913129d3 | 2022-07-05 | [d3d9] Add an config option to disable non seamless cube maps. | correctness | later | Fix; needs a check against this tree |
| 4a0a9d62 | 2022-07-02 | [util] Limit Dead Space to 60fps and fix vsync | compatibility | later | D3D9 profile; not in the first batch |
| 37335907 | 2022-07-09 | [util] disable allowDoNotWait for Port Royale 3 (#2668) | compatibility | later | D3D9 profile; not in the first batch |
| 1b89394a | 2022-07-09 | [util] Enable cached dynamic resources for DayZ (#2709) | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e8f48c71 | 2022-07-12 | [dxvk] Always enable robustBufferAccess2 feature | refactor | skip | DXVK backend change that follows the 2.x backend |
| f99a833f | 2022-07-12 | [d3d11] Remove d3d11.constantBufferRangeCheck option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| bd68f05c | 2022-07-12 | [dxvk] Make emitRenderTargetReadbackBarrier more generic | refactor | skip | DXVK backend change that follows the 2.x backend |
| 10b174b5 | 2022-07-12 | [d3d9] Fix barriers with staging buffers | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 95995041 | 2022-07-13 | [d3d9] Explicitly check for Unknown in CheckDeviceFormatConversion | correctness | port | Ported in cf0b4bcc9 |
| 64d22606 | 2022-07-08 | [d3d9] add D3DFMT_UNKNOWN to windowed BackBufferFormat | correctness | port | Ported in cf0b4bcc9 |
| ce48b57f | 2022-07-10 | [d3d9] Allow POOL_SCRATCH targets in GetFrontBufferData | correctness | port | Ported in aeba45040 |
| e8d5ce94 | 2022-07-14 | [d3d9] Introduce D3D9ConstantBuffer | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 08c3c458 | 2022-07-14 | [d3d9] Use generic constant buffer implementation | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 4635c72e | 2022-07-14 | [d3d9] Use generic constant buffers for SWVP | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 74a8bfb7 | 2022-07-14 | [d3d9] Remove CreateConstantBuffer method | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 9e110cd3 | 2022-07-13 | [d3d9] Fix up unsupported sample counts | correctness | port | Ported in 918a5dd4f |
| 5ae5476d | 2022-07-14 | [d3d9] Make reported sample counts consistent with DecodeMultiSampleType behavior. | correctness | port | Ported in 918a5dd4f |
| e884413c | 2022-07-06 | [dxvk] Don't synchronize device if going for DLL shutdown | refactor | skip | DXVK backend change that follows the 2.x backend |
| f07a6e16 | 2022-07-14 | [d3d9] Rework UP buffer allocation | other | later | Not reviewed in detail |
| 6ac5ca3b | 2022-07-14 | [d3d9] Use existing staging buffer implementation for managed uploads | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 2774a041 | 2022-07-14 | [dxvk] Use VkPhysicalDeviceVulkan11{Features,Properties} where appropriate | refactor | skip | DXVK backend change that follows the 2.x backend |
| eee4ac1e | 2022-07-14 | [dxvk] Replace VK_KHR_sampler_mirror_clamp_to_edge with core features | refactor | skip | DXVK backend change that follows the 2.x backend |
| 0b47297b | 2022-07-14 | [dxvk] Replace VK_EXT_host_query_reset with core feature | refactor | skip | DXVK backend change that follows the 2.x backend |
| 956f293a | 2022-07-14 | [dxvk] Replace VK_KHR_depth_stencil_resolve with core feature | refactor | skip | DXVK backend change that follows the 2.x backend |
| 131af0d6 | 2022-07-14 | [dxvk] Replace VK_KHR_driver_properties with core feature | refactor | skip | DXVK backend change that follows the 2.x backend |
| 223a4fd1 | 2022-07-14 | [d3d9] Always assume RADV has fast mulz support | perf | later | Perf; measure on Remix first |
| f60bdcbc | 2022-07-14 | [dxvk] Replace VK_EXT_shader_demote_to_helper_invocation with core feature | refactor | skip | DXVK backend change that follows the 2.x backend |
| d6d7d513 | 2022-07-14 | [dxvk] Drop VK_EXT_4444_formats | refactor | skip | DXVK backend change that follows the 2.x backend |
| 320534cb | 2022-07-15 | [spirv] Automatically track interface variables | refactor | skip | DXVK backend change that follows the 2.x backend |
| cdf22a40 | 2022-07-15 | [dxvk] Rename imageFormatInfo -> lookupFormatInfo | refactor | skip | DXVK backend change that follows the 2.x backend |
| 48ac9b27 | 2022-07-17 | eveonline-dx12_workaround | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| f15466a2 | 2022-07-17 | [dxvk] Remove remaining built-in specialization constants | refactor | skip | DXVK backend change that follows the 2.x backend |
| e28b2683 | 2022-07-18 | [util] Enable disableMsaa option for Mary Skelter 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 75d0b1af | 2022-07-20 | [util] Remove Resident Evil 6 workaround | irrelevant | skip | Profile comment or layout change |
| f2d4455c | 2022-07-22 | [util] enable dialog mode for NFS 3 modern patch | compatibility | later | D3D9 profile; not in the first batch |
| cb291f29 | 2022-07-23 | [util] enable alphaTestWiggleRoom for Ninja Blade | compatibility | later | D3D9 profile; not in the first batch |
| 9cd04735 | 2022-07-23 | [util] Set ignoreGraphicsBarriers option for Stray | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| abf5ed15 | 2022-07-22 | [dxvk] Add Direct3D9ForceHybridEnumeration empty export function. | refactor | skip | DXVK backend change that follows the 2.x backend |
| b06140af | 2022-07-25 | [util] set maxFrameLatency to 1 for YS Origin | compatibility | port | Ported in d9a4ea386 |
| 6c5f73ac | 2022-07-26 | [dxvk] Reintroduce VkPhysicalDeviceIDProperties | refactor | skip | DXVK backend change that follows the 2.x backend |
| 116feca6 | 2022-07-10 | [d3d9] Handle unbound textures in fixed function shaders | correctness | later | Fix; needs a check against this tree |
| 45c1d791 | 2022-03-15 | [d3d9] Remove evictManagedOnUnlock | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| ba4d95c5 | 2022-06-11 | [d3d9] Remove direct upload path | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 08ad6583 | 2022-07-28 | [d3d9] Only set upload bit for managed textures | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| c3dbb642 | 2022-06-02 | [d3d9] Implement memory allocator for memory mapped files | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| d598fd31 | 2022-03-11 | [d3d9] Add HUD item for unmappable memory | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 6ca65544 | 2022-03-16 | [d3d9] Use memory mapped files for textures | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| ea76bfd0 | 2022-02-14 | [d3d9] Unmap unused resources | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 1d1f0c7e | 2022-07-30 | [d3d9] Document D3D9SpecConstantId | refactor | skip | Refactor or cleanup |
| 753aede1 | 2022-07-30 | [d3d9] Correct comment about ProjectionType | refactor | skip | Refactor or cleanup |
| bad7d469 | 2022-07-30 | [d3d9] Handle very large Up draws | correctness | later | Fix; needs a check against this tree |
| 10e6d0ef | 2022-07-30 | [dxvk] Don't redundantly reset spec constant values | refactor | skip | DXVK 2.x binding / shader interface |
| 4d8b75c8 | 2022-07-30 | [d3d9] Fix spec constant derp | refactor | skip | DXVK 2.x binding / shader interface |
| 0eaad2eb | 2022-07-30 | [dxso] Don't create shader object for redundant PS permutations | perf | later | Perf; measure on Remix first |
| db786cda | 2022-07-30 | [dxvk] Remove old code to process specialization constants | refactor | skip | DXVK backend change that follows the 2.x backend |
| a2ef99b9 | 2022-07-31 | [d3d9] Generalize D3D9ConstantBuffer constructor | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 54f989b2 | 2022-07-31 | [dxso] Introduce getSpecConstantBufferSlot | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 8db2eb51 | 2022-07-31 | [d3d9] fix ordinal values in the DEF file | correctness | later | Fix; needs a check against this tree |
| 96710555 | 2022-08-01 | [util] Support parsing floating point arguments | refactor | skip | DXVK backend change that follows the 2.x backend |
| 5256d5e2 | 2022-08-01 | [dxvk] Fix minor edge cases when parsing floats in config file | irrelevant | skip | Profile comment or layout change |
| 08424ccb | 2022-08-02 | [d3d9] Add missing vector header | refactor | skip | Refactor or cleanup |
| 0315997f | 2022-08-03 | [d3d9] Use mutable lambdas to avoid some redundant ref counting | perf | later | Perf; measure on Remix first |
| 0f949711 | 2022-08-05 | [d3d11,d3d9] Lock context/device in EndFrame | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 438ae5cd | 2022-08-05 | [dxso] Fix getting sampler type of vertex textures | correctness | skip | Not applicable: fixes the null-sampler spec constant, which this tree does not have |
| b2cbf198 | 2022-07-30 | [d3d9, dxso] Refactor spec constants to use a bitfield layout | refactor | skip | DXVK 2.x binding / shader interface |
| 96193777 | 2022-07-31 | [dxso] Convert opIEqual 1 -> opINotEqual 0 | other | later | Not reviewed in detail |
| d498551a | 2022-08-01 | [dxso] Add bit selectors to D3D9ShaderSpecConstantManager | other | later | Not reviewed in detail |
| 9e1ecec7 | 2022-08-03 | [d3d9] Disable fetch4 when binding an incompatible texture | correctness | port | Ported in fde76cc9a |
| 31d17efb | 2022-08-05 | [dxvk] Add feedback loop aspect flags to bindRenderTargets | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 8d070e54 | 2022-08-05 | [d3d9] Enable extAttachmentFeedbackLoopLayout feature if available | other | later | Not reviewed in detail |
| 90abd993 | 2022-08-05 | [d3d9] Pass feedback loop aspect mask to backend | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 63d582a6 | 2022-08-05 | [d3d9] Use VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_EXT for hazards if available | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 8c9601b4 | 2022-08-05 | [d3d9] Remove generalHazards option | compatibility | later | New option |
| 9108f8c7 | 2022-08-05 | [d3d9] Clean up fetch4 handling | refactor | skip | Refactor or cleanup |
| 65da0da9 | 2022-08-06 | [d3d9] Support for graphics pipeline libraries | refactor | skip | Needs graphics pipeline library (DXVK 2.x) |
| 6589966f | 2022-08-06 | [d3d9] Fix FF VS fog with GPL | refactor | skip | Needs graphics pipeline library (DXVK 2.x) |
| 135b8a0e | 2022-08-06 | [dxso] Setup spec ubo for pixel shaders | other | later | Not reviewed in detail |
| 7b28bbae | 2022-08-06 | [util] Disable unmapping for Saints Row 2 | compatibility | later | D3D9 profile; not in the first batch |
| ddb528cc | 2022-08-07 | [d3d9] Change texture before updating fetch4 | correctness | later | Fix; needs a check against this tree |
| 64169316 | 2022-08-07 | [d3d9] Compute vertex extent in BindInputLayout | other | later | Not reviewed in detail |
| 1c1958be | 2022-08-07 | [d3d9] Use new resource view binding methods | refactor | skip | Refactor or cleanup |
| 29a2cb9a | 2022-08-07 | [d3d9,d3d11] Make GetShaderStage functions constexpr | compatibility | later | Interop / shared resources; low priority for Remix |
| 2e6a2f1b | 2022-08-07 | [dxvk] Make shader stage parameter in bindShader a template parameter | refactor | skip | DXVK backend change that follows the 2.x backend |
| 48319096 | 2022-08-07 | [d3d9] Enable apitrace mode for The Witcher | compatibility | later | D3D9 profile; not in the first batch |
| 4f0c2176 | 2022-08-08 | [d3d9] Fix recording MultiplyTransform | correctness | port | Ported in 6a40169da |
| 0adf64f0 | 2022-08-08 | [dxvk] Add flat shading parameter to rasterizer state | refactor | skip | DXVK backend change that follows the 2.x backend |
| 8f1024c0 | 2022-08-08 | [d3d9] Use new flat shading state for DXSO shaders | refactor | skip | Refactor or cleanup |
| 7789fd53 | 2022-08-08 | [d3d9] Use new flat shading state for fixed-function pipelines | refactor | skip | Refactor or cleanup |
| ac1e44f1 | 2022-08-08 | [d3d9] Fix typo | refactor | skip | Refactor or cleanup |
| 36d8bb77 | 2022-08-08 | [d3d9] Fix synchronization in UpdateTextureFromBuffer | correctness | later | Fix; needs a check against this tree |
| 01fb4042 | 2022-08-08 | [d3d9] Remove some dead debug code | refactor | skip | Refactor or cleanup |
| 1628b9e6 | 2022-08-08 | [d3d9] Add 64bit implementation for D3D9MemoryAllocator | other | later | Not reviewed in detail |
| 49e9ba2c | 2022-08-08 | [d3d9] Move d3d9 bytecode into D3D9Shader | refactor | skip | Refactor or cleanup |
| 1fcd5dc0 | 2022-08-08 | [d3d9] Unmap stored shader bytecode | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 50018de4 | 2022-08-09 | [util] Use topath helper for fstreams | irrelevant | skip | Profile comment or layout change |
| 67f93711 | 2022-08-10 | [dxvk] Remove shrinkNvidiaHVVHeap workaround | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| f031ce44 | 2022-07-29 | [util] Set lower memory limits for Guitar Hero WT | compatibility | later | D3D9 profile; not in the first batch |
| 8feabc65 | 2022-08-08 | [d3d9] Do implicit flush after uploading textures or buffers | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 02f653fd | 2022-08-09 | [d3d9] Limit amount of staging memory in flight | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 5cdee453 | 2022-08-11 | [util] Disable direct buffer mapping for GHWT | compatibility | later | D3D9 profile; not in the first batch |
| b5b74116 | 2022-08-11 | [d3d9] Fix rare hang when waiting for staging buffer markers | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 9bd0040a | 2022-08-11 | [d3d9] Request high-priority shader compiles as necessary | other | later | Not reviewed in detail |
| 0d33d063 | 2022-08-11 | [dxvk] Remove device LUID workaround | refactor | skip | DXVK backend change that follows the 2.x backend |
| 46cb05ce | 2022-08-11 | [d3d9+util] Always ignore D3DLOCK_DONOTWAIT | correctness | later | Fix; needs a check against this tree |
| de8d2b37 | 2022-08-13 | [d3d9] FF: Don't change flatShadingMask for outputs | correctness | later | Fix; needs a check against this tree |
| 7506f658 | 2022-08-14 | [d3d9] Move window proc handling code to own file | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 6baaa3a5 | 2022-08-14 | [d3d9] New window proc code on multiple platforms | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 870dd18f | 2022-08-16 | [d3d9] Set VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT for render targets | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| a78aab14 | 2022-08-16 | [d3d9] Add feedback loop usage for DS | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 08496913 | 2022-08-16 | [d3d9] Only set feedback loop usage on textures, not plain surfaces | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 04bc1bac | 2022-08-16 | [util] Limit Beyond Good and Evil FPS to 60 | compatibility | later | D3D9 profile; not in the first batch |
| 61025c00 | 2022-08-17 | [dxvk] Don't disable frame rate limiter if vsync is enabled | refactor | skip | DXVK backend change that follows the 2.x backend |
| 80fc1d8b | 2022-08-17 | [d3d9] Use new format support queries | refactor | skip | Refactor or cleanup |
| 3c2fc41e | 2022-08-17 | [d3d9] Re-bind framebuffer if RT hazards change | correctness | later | Fix; needs a check against this tree |
| 3806bd44 | 2022-08-17 | [dxvk] Change descriptor info to take only one shader stage | refactor | skip | DXVK 2.x binding / shader interface |
| 2c713a34 | 2022-08-12 | [d3d9,dxso] Factor out common alpha test code | other | later | Not reviewed in detail |
| 47fa3824 | 2022-08-12 | [d3d9] Re-implement alpha test to support configurable accuracy | compatibility | later | New option |
| 5a3cb5ad | 2022-08-17 | [d3d9] Adjust alpha test precision based on render target format | other | later | Not reviewed in detail |
| a695644f | 2022-08-12 | [d3d9] Remove alphaTestWiggleRoom option | compatibility | later | New option |
| 85cc87e4 | 2022-08-18 | [d3d9] Fix invalid shader in fixed-function alpha test | correctness | later | Fix; needs a check against this tree |
| 155944c2 | 2022-08-19 | [dxso] normalize 0 to 0 with strict float emulation | other | later | Not reviewed in detail |
| ea4e0bc4 | 2022-08-17 | [d3d9,d3d11] Require shaderDemoteToHelperInvocation feature | refactor | skip | DXVK backend change that follows the 2.x backend |
| 2d92679c | 2022-08-17 | [dxso] Remove fallback path for discards | refactor | skip | Removal tied to upstream refactors |
| 76ee76d6 | 2022-08-19 | [d3d9,dxso] Use DemoteToHelperInvocation for alpha test | other | later | Not reviewed in detail |
| 654b5170 | 2022-08-17 | [d3d9] Add helpers for new WSI interface | irrelevant | skip | Build or platform change |
| dac7e38f | 2022-08-14 | [d3d9] Make swapchain use new wsi abstraction | irrelevant | skip | Build or platform change |
| 9690b2a9 | 2022-08-14 | [d3d9] Make adapter code use new wsi abstraction | irrelevant | skip | Build or platform change |
| 69eba937 | 2022-08-14 | [d3d9] Make window proc code use new wsi interface | irrelevant | skip | Build or platform change |
| f3992658 | 2022-08-14 | [d3d9] Remove unused util_monitor include | refactor | skip | Refactor or cleanup |
| f80347d9 | 2022-08-20 | [util] Remove RE:REV2 and RE5 workarounds | irrelevant | skip | Profile comment or layout change |
| 1c679edb | 2022-08-21 | [d3d9] Use topath helpers in shader code | other | later | Not reviewed in detail |
| 5f9df1f6 | 2022-08-21 | [d3d9] Remove leftover GetWindowRect | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| c6c8acb0 | 2022-08-21 | [d3d9] Use wsi's isWindow in LeaveFullscreenMode | irrelevant | skip | Build or platform change |
| bf99127e | 2022-08-21 | [d3d9] Stub out cursor code on non-Windows platforms | irrelevant | skip | Build or platform change |
| 0e4e9355 | 2022-08-21 | [d3d9] Ifdef out code for enumerating adapters by attached displays | other | later | Not reviewed in detail |
| ad386305 | 2022-08-21 | [d3d9] Correct DLLEXPORT for native builds | irrelevant | skip | Build or platform change |
| 968bdccb | 2022-08-21 | [d3d9] Include win32 compat headers where applicable | irrelevant | skip | Build or platform change |
| 23c3960f | 2022-08-22 | [dxvk] Store WSI semaphore pair directly with the command list | irrelevant | skip | Build or platform change |
| a8b578b2 | 2022-08-19 | [d3d9] Fix crash when auto generating mip maps for unmappable textures | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 87b1f9fa | 2022-08-22 | [dxso] Fix write mask for nrm | correctness | skip | Not needed: our nrm multiplies the masked load (opVectorTimesScalar) |
| 8f240938 | 2022-08-25 | [util] cap MGS V Ground Zeroes vram at 4095 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 3ee808af | 2022-08-25 | [dxvk] Make memory object of an image publicly accessible | refactor | skip | DXVK backend change that follows the 2.x backend |
| e923cc5b | 2022-08-19 | [dxvk] Change emitGraphicsBarrier to specify a dependency | refactor | skip | DXVK backend change that follows the 2.x backend |
| 2329c71b | 2022-08-25 | [dxvk] Implement sampler reduction mode | refactor | skip | DXVK backend change that follows the 2.x backend |
| bfd47ec8 | 2022-08-26 | [d3d9] Try to match either top or bottom mips in UpdateTexture | correctness | port | Ported in cae72a3c3 |
| c49b1ee3 | 2022-08-29 | [d3d9] Use SetStateTexture in Reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 3e0031ce | 2022-08-29 | [util] Add a maxAvailableMemory limit for Heroes of Annihilated Empires | compatibility | later | D3D9 profile; not in the first batch |
| 2b16134b | 2022-09-07 | [util] Enable d3d9.memoryTrackTest for The Ship | compatibility | port | Ported in 1a7087321 |
| 4b8387c9 | 2022-09-08 | [d3d9] Set host stage and access bits for image staging buffers | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 1bc6f966 | 2022-09-01 | [d3d9] Force D3DTADDRESS_CLAMP for cubes. | correctness | later | Cube address mode clamp; conflicts with sampler key changes |
| bd5d9d90 | 2022-09-06 | [d3d9] Use xor to swap bit in depth mask. | other | later | Not reviewed in detail |
| 2ec25f58 | 2022-09-10 | Cap Warhammer Online to 100 FPS | compatibility | later | D3D9 profile; not in the first batch |
| ccfb986e | 2022-09-09 | [dxbc] Rework TGSM workaround | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| b0ac267f | 2022-09-10 | [d3d9] Explicitly cast to D3DRENDERSTATETYPE in ColorWriteIndex | other | later | Not reviewed in detail |
| 99e01d64 | 2022-09-10 | [dxso] Make fallthrough for extra params explicit | other | later | Not reviewed in detail |
| 10bb285f | 2022-09-10 | [d3d9] Remove unused dstExtent + friends in GetRenderTargetData | refactor | skip | Refactor or cleanup |
| 3a636f60 | 2022-09-11 | [dxvk] Introduce DxvkFormatQuery | refactor | skip | DXVK backend change that follows the 2.x backend |
| e05e063d | 2022-09-12 | [util] Enable memoryTrackTest for SiN Episodes Emergence | compatibility | port | Ported in 1a7087321 |
| 26c8f46b | 2022-08-18 | [util] Limit King Of Fighters XIII to 60 FPS | compatibility | later | D3D9 profile; not in the first batch |
| 18a801de | 2022-09-13 | [dxso] Use a * (y - x) + x for lerp. | correctness | port | Ported in a4da6ef0e |
| 19b76825 | 2022-08-24 | [d3d9] Fix for missing restriction check in UpdateSurface. | correctness | later | Fix; needs a check against this tree |
| 10d6e156 | 2022-09-14 | [d3d9] Do not set window size and position when restoring from fullscreen state | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| b56ec10d | 2022-09-16 | [d3d9] Add helpers for precise matrix-vector products | other | later | Not reviewed in detail |
| 4fb6c200 | 2022-09-16 | [d3d9] Use precise matrix-vector operations to compute vertex position | other | later | Not reviewed in detail |
| 07094ac4 | 2022-09-16 | [util] Remove fps cap from Warhammer Online | compatibility | later | D3D9 profile; not in the first batch |
| 0bb0c1e6 | 2022-09-19 | [util] Enable ignoreGraphicsBarriers for FFXV | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 5962be44 | 2022-09-18 | [dxso] Define color inputs as centroids on SM3 too | correctness | port | Ported in 80caccb68 |
| c759fb45 | 2022-09-17 | [util] Disable DF support for pretty mirrors in GTA IV | compatibility | later | D3D9 profile; not in the first batch |
| 5c22e2fb | 2022-09-19 | [util] Enable GTA IV config for Episodes from Liberty City | irrelevant | skip | Profile comment or layout change |
| 82ebc29e | 2022-09-21 | [dxso] Fix for illegal OpCompositeConstruct while translating Crs opcode | correctness | port | Ported in 7bc2159a8 |
| 1e1ef8f1 | 2022-09-22 | [d3d9] Fix for missing mapping of VertexElements declarations to FVF bits | correctness | later | Fix; needs a check against this tree |
| 1451032b | 2022-09-22 | [d3d9] Minor cleanups in vertex declaration code (#2950) | refactor | skip | Refactor or cleanup |
| ae246507 | 2022-09-21 | [d3d9] Add ID3D9VkInteropInterface | compatibility | later | Interop / shared resources; low priority for Remix |
| ac131126 | 2022-09-21 | [d3d9] Add ID3D9VkInteropTexture | compatibility | later | Interop / shared resources; low priority for Remix |
| e976218e | 2022-09-21 | [d3d9] Make GetDXVKDevice return a const Rc reference | other | later | Not reviewed in detail |
| d221bb7a | 2022-09-21 | [d3d9] Add ID3D9VkInteropDevice | compatibility | later | Interop / shared resources; low priority for Remix |
| f2950953 | 2021-08-11 | [d3d9] Avoid depth degenerate viewports | correctness | present | cherry-picked by NVIDIA |
| 7552cfe6 | 2022-09-26 | [dxso] Always lower projection | other | later | Not reviewed in detail |
| 6188ffa2 | 2022-09-26 | [d3d9] Add a config option for D24 -> D32 | compatibility | later | New option |
| 5684e297 | 2022-09-26 | [d3d9] Track if a texture is upgraded to D32f | other | later | Not reviewed in detail |
| 6fe96d7d | 2022-09-26 | [dxso] Support clamping Dref. | correctness | later | Fix; needs a check against this tree |
| a9bdea72 | 2022-09-26 | [d3d9] Clamp Dref to [0.0, 1.0] if the texture is emulated UNORM | correctness | later | Fix; needs a check against this tree |
| 50857537 | 2022-09-26 | [d3d9] Use D32 not D32_S8 when upgrading formats without stencil aspect. | other | later | Not reviewed in detail |
| 49854dbf | 2022-09-27 | [d3d9] Saturate viewport depth range | correctness | later | Viewport depth clamp; conflicts with Remix viewport changes |
| b2ad2575 | 2022-09-28 | [d3d9] Fix for missing restriction check in ProcessVertices | correctness | later | Fix; needs a check against this tree |
| f8bd19f2 | 2022-09-30 | [util] Cap Sonic Adventure 2 to 60 fps | compatibility | later | D3D9 profile; not in the first batch |
| 038ed23a | 2022-10-03 | [util] Force SM1 for the Escape from Tarkov launcher | compatibility | later | D3D9 profile; not in the first batch |
| 2b964f0c | 2022-10-07 | [d3d9] Make MapD3DDeclToFvf more readable | refactor | skip | Refactor or cleanup |
| 70901055 | 2022-10-08 | [d3d9] Remove declaration fvf mapping log spam | refactor | skip | Removal tied to upstream refactors |
| 4bcabe8d | 2022-10-07 | [d3d9] Return 0 if any vertex decl element can't be represented as fvf | correctness | later | API conformance (validation or return codes) |
| 3b90f5a7 | 2022-10-15 | [util] Enable apitrace mode for Hammer World Editor | compatibility | port | Ported in 9404c869c |
| 25798f6f | 2022-10-05 | [build] Set name_prefix to `libdxvk_` for native builds | irrelevant | skip | Build or platform change |
| 1d3decf1 | 2022-10-06 | [build] Add version scripts for native builds | irrelevant | skip | Build or platform change |
| 859de7e8 | 2022-10-18 | [dxvk] Always enable depth clip feature if supported | refactor | skip | DXVK backend change that follows the 2.x backend |
| 494e01b3 | 2022-10-24 | [d3d9] Avoid querying environment variables on shader creation | perf | later | Perf; measure on Remix first |
| d771f7cf | 2022-10-24 | [d3d9] Remove dead fields | refactor | skip | Refactor or cleanup |
| 9c22a585 | 2022-10-24 | [d3d9] Disable locking non-dynamic default textures | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| f84f992d | 2022-10-23 | [util] Also enable workaround for the "mod load exe" of ToEE | irrelevant | skip | Profile comment or layout change |
| bc31ebe1 | 2022-10-29 | [d3d9] Fix OptimizeLayout with feedback loop usage. | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 0fc5c84e | 2022-10-30 | [d3d9] Don't advertise support for MS INTZ format | correctness | port | Ported in 7d083806b |
| 0462454d | 2022-10-31 | Fix for saving vertex shader constants with a StateBlock | correctness | port | Ported in 5bf92c4c6 |
| 892f6766 | 2022-11-02 | [util] Disable direct buffer mapping for Dragon Age Origins | compatibility | later | D3D9 profile; not in the first batch |
| e1eec9b3 | 2022-11-05 | [util] Set Fallout 3 to Nvidia vendor id | compatibility | later | D3D9 profile; not in the first batch |
| 40a4908a | 2022-11-04 | Support for MW:R H1-Mod | irrelevant | skip | Profile comment or layout change |
| 4fb7acc6 | 2022-11-05 | [d3d9] Implement converter for W11V11U10 | correctness | present | cherry-picked by NVIDIA |
| 145c1ce1 | 2022-11-06 | [util] Limit fps to 60 for some WayForward games | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d8933ca1 | 2022-08-18 | [d3d9] Only do one allocation for all texture subresources | other | later | Not reviewed in detail |
| 03dca539 | 2022-10-28 | [vulkan,d3d9,d3d11] Move surface creation to swap chain implementation | compatibility | later | Interop / shared resources; low priority for Remix |
| ecc23023 | 2022-11-07 | [d3d9] Fix query reset counter underflow | correctness | port | Ported in 5703b8f77 |
| e30b7835 | 2022-11-08 | d3d9: Use a different rvalue for depth bias on NV | correctness | later | Depth bias representation; Remix has its own depth bias handling for decals, needs a check |
| bc08cac2 | 2022-11-09 | [util] Enable apitraceMode for Sonic & All-Stars Racing Transformed | compatibility | port | Ported in 9404c869c |
| 8a2e4ef4 | 2022-11-09 | [d3d9] Allow locking DEFAULT pool based on texture type | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 785bc973 | 2022-11-15 | [util] Disable DC Single Use Mode for Cardfight!! Vanguard Dear Days | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| ad3c316d | 2022-11-16 | [util] Set Black Mesa customVendorId to Nvidia | compatibility | later | D3D9 profile; not in the first batch |
| 3393c5f4 | 2022-11-13 | [d3d9] Only upload mip 0 of managed automipgen textures | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 2c53459f | 2022-11-18 | [util] disable floatControls for Bad Company 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d0a10cc9 | 2022-11-21 | [d3d9] Handle DS texture uploads | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 4796eb0b | 2022-11-21 | [d3d9] Implement capturing lights in state blocks | correctness | port | Ported in c6a1b9eb2 |
| 3b9235fe | 2022-11-21 | [util] Set max frame latency to 1 for Sonic Frontiers | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 522909b1 | 2022-12-03 | [d3d9] Don't set NeedsReadback when dirtying mip maps | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 4a4d8801 | 2022-12-04 | [util] Set TRAHA Global to Intel vendor id | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| b838b655 | 2022-12-06 | [d3d9] Correctly mask alpha reference push constant | refactor | skip | DXVK 2.x binding / shader interface |
| 7b202488 | 2022-11-28 | [util] disable unmapping for Final Fantasy XIV d3d9 | compatibility | later | D3D9 profile; not in the first batch |
| f94b42f2 | 2022-12-30 | [d3d9] Dont use GenerateDrawInfo in DrawPrimitiveUp | other | later | Not reviewed in detail |
| 3ed0a4fd | 2023-01-07 | [util] Set Secret World Legends launcher to SM 2 | compatibility | later | D3D9 profile; not in the first batch |
| 0342a25e | 2023-01-08 | [d3d9,dxso] Add d3d9.forceSampleRateShading option | compatibility | later | New option |
| cc9266ed | 2023-01-11 | [util] Add config for Alien Rage | compatibility | port | Ported in e86b7fd8d |
| 97a91c81 | 2022-12-31 | [d3d9] Disable instancing for non-indexed draws | correctness | later | Fix; needs a check against this tree |
| 4a309333 | 2023-01-12 | [dxvk] Add UBO set property to descriptor info | refactor | skip | DXVK 2.x binding / shader interface |
| d35bf455 | 2023-01-13 | [dxvk] Rename bindResourceBuffer to bindUniformBuffer | refactor | skip | DXVK backend change that follows the 2.x backend |
| 459758c6 | 2023-01-13 | [d3d9] Don't set storage buffer usage unless necessary | correctness | later | Fix; needs a check against this tree |
| 906b931e | 2023-01-17 | [util] Disable unmapping for BlazBlue Centralfiction | compatibility | later | D3D9 profile; not in the first batch |
| 61d72eeb | 2023-01-23 | [dxgi,d3d9] Use global singleton for DXVK instances | other | later | Not reviewed in detail |
| caf31033 | 2023-02-07 | [util] Far Cry 2: Set VendorId to Nvidia, enable apitraceMode (#3241) | compatibility | later | D3D9 profile; not in the first batch |
| 08118131 | 2023-02-18 | [util] Limit Battle Fantasia Revisited Edition to 60fps (#3256) | compatibility | later | D3D9 profile; not in the first batch |
| 0d28be4a | 2023-02-18 | [d3d9] Fix capturing lights in state block | correctness | port | Ported in c6a1b9eb2 |
| 81440340 | 2023-03-01 | [d3d9,dxvk,util] Actually use dxvk::mutex | other | later | Not reviewed in detail |
| 4c789646 | 2023-03-01 | [d3d9] Only log QueryInterface errors once | other | later | Not reviewed in detail |
| 5609c5e0 | 2023-03-01 | [util] Reduce maximum chunk size for WILD HEARTS. | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1c6fc7b5 | 2023-03-02 | [d3d9] Clamp stage and type in [G,S]etTextureStageState | correctness | port | Ported in 5a051304c |
| 8ecd1b3b | 2023-01-24 | [util] enableDialogMode for Codename Panzers Phase One/Two | compatibility | later | D3D9 profile; not in the first batch |
| 996acbe3 | 2023-02-11 | [d3d9] Refactor state blocks to allocate dynamically. | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| d14dcf5d | 2023-03-10 | [d3d9] Fix redundant copy of SWVP VS consts | perf | later | Perf; measure on Remix first |
| 3a123222 | 2023-03-11 | [util] Enable cached vertex buffers for SpellForce 3 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 243c2f3c | 2023-03-14 | [d3d9] remove support for the A1/X1R5G5B5 formats | refactor | skip | Removal tied to upstream refactors |
| da32453b | 2023-03-16 | [dxvk] Add submission feedback to command submissions | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 9bdad71d | 2023-03-16 | [d3d11] Implement CreateWrappedResource for D3D12 textures | compatibility | later | Interop / shared resources; low priority for Remix |
| b0ed97e0 | 2023-03-22 | [util] Disable nvapiHack for Diablo 4 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d11878e7 | 2023-03-23 | [d3d9] Only apply viewport zBias if minZ is below 0.5 | other | later | Not reviewed in detail |
| 1a5afc77 | 2023-03-23 | [util] Include cstdint to fix compilation with GCC 13 | irrelevant | skip | Build or platform change |
| 631171cf | 2023-03-24 | [util] Disable nvapiHack for Tom Clancy's Ghost Recon Breakpoint. | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 977669c6 | 2023-03-25 | [util] Enable cached dynamic vertex/index buffers in GTA V | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 3ce3209e | 2023-04-05 | [util] Set syncInterval to 1 for Crash Bandicoot N sane trilogy | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| af0009c5 | 2023-04-08 | [d3d9] Use DxvkFormatInfo for video formats | other | later | Not reviewed in detail |
| 8508633b | 2023-04-06 | [util] Disable allowDirectBufferMapping for Dark Romance: Vampire in Love | compatibility | later | D3D9 profile; not in the first batch |
| 6b779206 | 2021-08-26 | remove unneeded definitions | refactor | skip | Refactor or cleanup |
| 01dc9ddc | 2023-03-27 | [util] Add new "Zusi 3" exe filename | irrelevant | skip | Profile comment or layout change |
| 990a7205 | 2023-04-16 | [d3d9] Handle a bunch of texture converter edge cases | correctness | later | Fix; needs a check against this tree |
| 1971a5d1 | 2023-04-19 | [util] Also add d3d9.deferSurfaceCreation for Atelier Ryza 3 | irrelevant | skip | Profile comment or layout change |
| 16d2a604 | 2023-04-19 | [util] Switch Nioh deferSurfaceCreation from dxgi to d3d9 | compatibility | later | D3D9 profile; not in the first batch |
| a42643b2 | 2023-04-13 | [d3d9] Fall back to GDI blit for partial presents | other | later | Not reviewed in detail |
| 860237e7 | 2023-05-02 | [d3d9] Skip 0x0 clears | correctness | port | Ported in 72330241d |
| f140d2de | 2023-05-03 | [d3d9] Handle swapchain OOM and other errors more gracefully | correctness | later | Fix; needs a check against this tree |
| b5f43063 | 2023-05-01 | [util] Spoof Nvidia for Modern Warfare 2 Campaign Remastered | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| b4366db3 | 2023-03-27 | [d3d9] Implement rudimentary device loss | other | later | Not reviewed in detail |
| b357d169 | 2023-03-27 | [util] Enable deviceLost for DC Universe Online and GTA IV | compatibility | later | D3D9 profile; not in the first batch |
| 242ac207 | 2023-05-04 | [d3d9] Release DC in fallback present path | other | later | Not reviewed in detail |
| 5443a2f9 | 2023-05-03 | [d3d9] Don't swap buffers for SWAPEFFECT_COPY & DISCARD with 1 backbuffer | correctness | later | Fix; needs a check against this tree |
| 96e22e7c | 2023-05-06 | [d3d9+util] Remove a bunch of redundant config options | perf | later | Perf; measure on Remix first |
| b08665c8 | 2023-05-03 | [d3d9] Fix compiler warning in Clear size check | irrelevant | skip | Build or platform change |
| bef2ef69 | 2023-05-07 | [d3d9] Fix stupid variable name | correctness | later | Fix; needs a check against this tree |
| 2b09932d | 2023-05-07 | [d3d9] Fix crash if device is freed with bound textures | correctness | later | Member order change; no texture destructor here calls into members declared after m_state, low value |
| 80f7d2ab | 2023-05-03 | [d3d9] fix opCompositeExtract out of bound | correctness | present | NVIDIA has an equivalent texcoord count clamp (NV-DXVK block in the FF VS) |
| 65520fa1 | 2023-05-16 | [util] Fix BlazBlue Centralfiction config | compatibility | later | D3D9 profile; not in the first batch |
| 99b367cd | 2023-05-18 | [util] Set floatEmulation to Strict for Halo Online | compatibility | port | Ported in 06b062a0f |
| f2bb1d4b | 2023-05-15 | [d3d9] Add extended swapchain interfaces | other | later | Not reviewed in detail |
| 6449f583 | 2023-05-21 | [util] Set dcSingleUseMode to false for SnowRunner | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1a2e724c | 2023-05-22 | [util] Enable apitrace mode for Tomb Raider Anniversary | compatibility | port | Ported in 9404c869c |
| d2759c20 | 2023-05-22 | [d3d9+d3d11] Remove DEVICE_LOCAL flag when forcing cached memory | correctness | port | Ported in fcd767764 |
| b9b2db51 | 2023-05-23 | [util] Limit fps for some Tomb Raider games | compatibility | later | D3D9 profile; not in the first batch |
| 8560efa3 | 2023-05-24 | [d3d9] Unbind RTs which are unwritten by a PS | other | later | Not reviewed in detail |
| 075c0bf2 | 2023-05-24 | [d3d9] Use m_boundRTs for COLORWRITEENABLE checks | other | later | Not reviewed in detail |
| 269bab2c | 2023-05-24 | [d3d9] Track if any color writes are enabled for an RT | other | later | Not reviewed in detail |
| cafd1047 | 2023-05-24 | [d3d9] Pack RT bitmasks tightly | other | later | Not reviewed in detail |
| b5c18a02 | 2023-05-24 | [d3d9] Optimize UpdateAnyColorWrites for 0th case | perf | later | Perf; measure on Remix first |
| e0654977 | 2023-05-24 | [d3d9] Stub out D3D9On12 interfaces | compatibility | later | Interop / shared resources; low priority for Remix |
| 4b108460 | 2023-05-26 | [dxso] Fix RT mask for SM1 | correctness | port | Ported in 6edefc47c |
| 8b6cbda6 | 2023-05-30 | [d3d9] Properly expose the MaxVertexBlendMatrixIndex capability | compatibility | later | Adapter caps report 0 blend matrix indices; can steer games to CPU skinning, which affects Remix skinning capture |
| b1b0abdb | 2023-05-29 | [dxvk] Move presenter implementation to DXVK module | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 1728d9e8 | 2023-05-29 | [dxvk] Rework presenter creation to take a DxvkDevice | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| e6be0cf9 | 2023-05-29 | [dxvk] Rework present mode selection for swap chains | refactor | skip | DXVK backend change that follows the 2.x backend |
| ab005912 | 2023-05-31 | [dxvk] Introduce dxvk.tearFree option | refactor | skip | DXVK backend change that follows the 2.x backend |
| c585ea25 | 2023-06-02 | [dxso] Respect all PS input elements for registers | correctness | port | Ported in 510ab2173 |
| 52f04ca3 | 2023-06-04 | [d3d9] FF: Fix using wrong texCoord mask | correctness | later | FF texcoord mask; conflicts with the NV-DXVK texcoord clamp |
| 2e70a2b0 | 2023-06-01 | [util] Set maxChunkSize to 1 for the Rockstar launcher and social club | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d241daa0 | 2023-06-04 | [util] Add app profile for Fallout 76 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 4a55047d | 2023-05-26 | [d3d9] Forbid disabling SWVP on a pure SWVP device | other | later | Not reviewed in detail |
| bbaf01d9 | 2023-05-26 | [d3d9] Force staging buffer mapping for pure SWVP devices | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 2efd3f36 | 2023-05-22 | [util] Re-enable direct buffer mapping for RE games | compatibility | later | D3D9 profile; not in the first batch |
| 1db2e3a6 | 2023-05-22 | [util] Re-enable direct buffer mapping for Dark Romance | compatibility | later | D3D9 profile; not in the first batch |
| 18508194 | 2023-05-25 | [d3d9] Only use direct buffer mapping for DYNAMIC buffers | other | later | Not reviewed in detail |
| 2c3f2b9a | 2023-05-26 | [d3d9] Ignore DISCARD after device loss | correctness | later | Fix; needs a check against this tree |
| 9b877cf6 | 2023-05-26 | [util] Remove D&D - The Temple of Evil workaround | compatibility | later | D3D9 profile; not in the first batch |
| d9d63166 | 2023-05-26 | [d3d9] Remove allowDiscard hack | refactor | skip | Removal tied to upstream refactors |
| 5fd025c5 | 2023-05-26 | [d3d9] Rename apitraceMode | correctness | port | Ported in fcd767764 |
| 9d6804e4 | 2023-05-26 | [util] Disable direct buffer mapping for Injustice | compatibility | port | Ported in d9a4ea386 |
| 22f6246f | 2023-06-06 | [util] Fix app profile grouping | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a1a91dd7 | 2023-06-11 | [d3d9] Fix potential race when discarding systemmem textures | correctness | later | Fix; needs a check against this tree |
| 52ac271a | 2023-06-10 | [d3d9] Reject Reset if there's any remaining DEFAULT resources | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 8f740c53 | 2023-06-13 | [d3d9] Remove IsLosable | refactor | skip | Removal tied to upstream refactors |
| b8d36eea | 2023-06-13 | [d3d9] Fix losing auto depth stencil surface | correctness | later | Fix; needs a check against this tree |
| d1707026 | 2023-06-13 | [d3d9] Remove uninitialized device present params | refactor | skip | Refactor or cleanup |
| c75ed869 | 2023-06-13 | [d3d9] Reset vertex & index buffer in Reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 5d134b87 | 2023-05-20 | [d3d9] implement 'samplerLodBias' as a conf option | compatibility | later | New option |
| 3fce9886 | 2023-05-20 | [d3d9] implement 'clampNegativeLodBias' as a conf option | correctness | later | Fix; needs a check against this tree |
| 6478c10a | 2023-06-15 | [util] Set maxChunkSize to 1 for Battle.net | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 2c014fdb | 2023-06-14 | [util] Match on the GOG version of KoF XIII as well | irrelevant | skip | Profile comment or layout change |
| c5ab5be4 | 2023-06-15 | [util] Set maxChunkSize to 1 for EA App | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 36e6a7c2 | 2023-06-15 | [util] Set maxChunkSize to 1 for GOG and fix EA App exe name | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a7977232 | 2023-04-21 | [d3d9] Initialize m_mapFlags to 0 | correctness | port | Ported in 01097195d |
| 404c984f | 2023-04-27 | [dxso] Implement bem instruction | correctness | port | Ported in acf5be13a |
| 24dbcf8f | 2023-06-19 | [dxso] Shared code for texbem and bem | other | later | Not reviewed in detail |
| 0236e780 | 2022-11-17 | [d3d9] Fix std::hex in D3D9CommonTexture | correctness | later | Fix; needs a check against this tree |
| 77e7e8bf | 2023-06-18 | [util] Report NVIDIA for Star Wars Battlefront as it errors out on AMD | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 211d095e | 2023-06-18 | [util] Limit STEINS;GATE ELITE to 60 fps | compatibility | later | D3D9 profile; not in the first batch |
| b30a4f0c | 2023-06-15 | [d3d9] Use FLOAT or FORCE_UNORM depth bias representation | correctness | later | Depth bias representation (VK_EXT_depth_bias_control); see e30b7835 |
| 5a1ebfa4 | 2023-06-20 | Revert "[d3d9] Only use direct buffer mapping for DYNAMIC buffers" | refactor | skip | Revert of 18508194 |
| ca349257 | 2023-06-09 | [dxvk] Add functionality to wait for a given present operation | refactor | skip | DXVK backend change that follows the 2.x backend |
| 5d119673 | 2023-06-15 | [dxvk] Implement waiting for specific present requests | refactor | skip | DXVK backend change that follows the 2.x backend |
| 7dbe4abb | 2023-06-15 | [d3d9] Use new presenter signal mechanism for frame pacing | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| a20869fb | 2023-06-21 | [d3d9] Track textures in m_activeHazardsRT instead of RT | other | later | Not reviewed in detail |
| 60b6e985 | 2023-06-21 | [d3d9] Transition DS too if there's a feedback loop | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 3625c5d4 | 2023-06-22 | [d3d9] Optimize DS active hazard check | perf | later | Perf; measure on Remix first |
| c7681962 | 2023-06-22 | [d3d9] Compress hazard state going into CS | other | later | Not reviewed in detail |
| a791493d | 2023-06-22 | [d3d9] Don't rebind AlphaTest when changing RT if not necessary | correctness | later | Fix; needs a check against this tree |
| 80b27f95 | 2023-06-22 | [d3d9] Fix active hazards RT only being 4 bits | correctness | later | Fix; needs a check against this tree |
| 7f302fc3 | 2023-06-24 | [d3d9] Don't mark DS hazards if not used by shader | correctness | later | Fix; needs a check against this tree |
| 362743c1 | 2023-06-24 | [d3d9] Update DS hazards when PS shader masks change | correctness | later | Fix; needs a check against this tree |
| 6b60de2d | 2023-06-24 | [d3d9] Fix unbinding RTs | correctness | later | Fix; needs a check against this tree |
| ff65599d | 2023-06-24 | [d3d9] Use m_activeRTs in BindFramebuffer | other | later | Not reviewed in detail |
| 0e36a07a | 2023-06-24 | [d3d9] Don't mark DS as hazardous if depth write is disabled | correctness | later | Fix; needs a check against this tree |
| 55be12da | 2023-06-24 | Revert "[d3d9] Use m_activeRTs in BindFramebuffer" | refactor | skip | Revert |
| 00ae1186 | 2023-06-24 | [d3d9] Don't use m_activeRTs in SetPixelShader | correctness | later | Fix; needs a check against this tree |
| 3b3ebc93 | 2023-06-24 | [d3d9] Rename some members to be clearer | refactor | skip | Refactor or cleanup |
| b77928b6 | 2023-06-24 | [util] Fix Modern Warfare 2 Campaign Remastered config | irrelevant | skip | Profile comment or layout change |
| d6e7e3e7 | 2023-06-21 | [d3d9] Add DxvkD3D8Bridge for d3d8 interop | compatibility | later | Interop / shared resources; low priority for Remix |
| 022bf1d1 | 2023-06-19 | [d3d9] Allow changing API name for d3d8 | correctness | later | Fix; needs a check against this tree |
| ccb87d5e | 2023-06-22 | [d3d9] Port flush heuristic from D3D11 | correctness | present | cherry-picked by NVIDIA |
| 3a368f47 | 2023-06-25 | Bladestorm Nightmare - Game speed increases when above 60 FPS outside of missions | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 2f72115f | 2023-06-24 | [d3d9] Keep 1 presenter per swapchain window | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 6be1f6d7 | 2023-07-05 | [util] Limit fps in The Incredibles | compatibility | port | Ported in d9a4ea386 |
| 886268fc | 2023-07-15 | [util] Remove Secret World Legends config | compatibility | later | D3D9 profile; not in the first batch |
| 878da498 | 2023-07-17 | [util] Disable single-use command lists for Ghost Recon Wildlands | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1daae750 | 2023-07-23 | [util] Set Cached Dynamic Resources for d3d11 Vindictus | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e598dcd7 | 2023-08-01 | [util] Add DXVK_CONFIG to define additional options | irrelevant | skip | Profile comment or layout change |
| a62117cd | 2023-08-01 | build: Disable stdcall alias-ing and use kill-at | irrelevant | skip | Build or platform change |
| b0b46fd0 | 2023-07-16 | [d3d9] Don't show/hide a software cursor | correctness | later | Cursor handling; Remix overlays its own UI |
| 549bd86f | 2023-08-05 | [d3d9] use strict float emulation for nvk | other | later | Not reviewed in detail |
| dfcd7aed | 2023-08-09 | [util] Limit Conflict Vietnam to 60fps (#3606) | compatibility | later | D3D9 profile; not in the first batch |
| 0746a3b9 | 2023-08-09 | [d3d9] Don't resolve an image with 1 sample | correctness | later | Fix; needs a check against this tree |
| 295a58af | 2023-07-16 | [d3d9] Check depth bounds test when deciding to bind DSV | other | later | Not reviewed in detail |
| 037669f7 | 2023-08-12 | [dxso] Don't assume 32 registers. | correctness | port | Ported in 61cee23d6 |
| b6a7714e | 2023-08-14 | [dxgi,util] Rename dxgi.nvapiHack option to dxgi.hideNvidiaGpu | compatibility | later | D3D9 profile; not in the first batch |
| 4ae542e8 | 2023-08-14 | [util] Do not hide Nvidia GPUs from Ratchet & Clank | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| bcaaac4a | 2023-08-17 | [d3d9] Handle sampling from DS_READONLY properly | correctness | later | Fix; needs a check against this tree |
| 740ebec7 | 2023-08-20 | [d3d9+dxso] Consider DMAP sampler in bit masks | perf | later | Perf; measure on Remix first |
| 6fce0949 | 2023-08-20 | [util] Limit fps in Project: Snowblind | compatibility | later | D3D9 profile; not in the first batch |
| 5c56fa0d | 2023-08-29 | [util] Enable deferSurfaceCreation for Drakensang | compatibility | port | Ported in d9a4ea386 |
| ce2f9f35 | 2023-08-31 | [util] Hide AMD in Riders Republic | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 9e26964a | 2023-08-23 | [d3d9] Divide projected textures by w if ProjectedCount is 0 | correctness | later | Projected textures with count 0; FF code differs |
| 4705de57 | 2023-09-03 | [util] Escape . | irrelevant | skip | Profile comment or layout change |
| a53f0e81 | 2023-09-03 | [util] Remove whitespace | irrelevant | skip | Profile comment or layout change |
| c113b791 | 2023-09-08 | [util] Enable 60 FPS lock for Aviary Attorney | compatibility | later | D3D9 profile; not in the first batch |
| 83dc4678 | 2023-09-14 | [util] Set maximum frame latency to 1 for Age of Empires 2 (2013) | compatibility | port | Ported in d9a4ea386 |
| 0632da19 | 2023-09-19 | [d3d9] Add a device compatibility mode for d3d8 | other | later | Not reviewed in detail |
| 494f7fd3 | 2023-09-18 | [d3d9] Only set initial NeedsUpload for D3DPOOL_MANAGED textures | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| f0ff0007 | 2023-10-02 | [util] Enable cachedDynamicBuffers for Battlestations Midway | compatibility | port | Ported in 9404c869c |
| 4d974685 | 2023-09-17 | [d3d9] Mark presenter for recreation on device reset with deferSurfaceCreation | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 552d2f0a | 2023-10-09 | [dxgi] Add useMonitorFallback option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e00db245 | 2023-10-10 | [util] Enable useMonitorFallback for Holocure | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| f45911a2 | 2023-10-11 | [util] Disable allowDirectBufferMapping for SkyDrift | compatibility | later | D3D9 profile; not in the first batch |
| a427d22c | 2023-10-31 | [dxvk] Add Vulkan instance flag for D3D9 apps | refactor | skip | DXVK backend change that follows the 2.x backend |
| 1cb58b07 | 2023-11-09 | [Util] two more executables to workaround Warhammer Online | irrelevant | skip | Profile comment or layout change |
| ea314980 | 2023-11-14 | [d3d9] Return empty buffer slice for out-of-bounds offsets | correctness | port | Ported in e959090c0 |
| 91f7f43c | 2023-11-17 | [util] Don't spoof Nvidia on AMD GPUs in Hitman 3 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1b31aa5d | 2023-11-27 | [util] Enable hideIntegratedGraphics for Metro Exodus EE | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| adb33d3a | 2023-12-22 | [util] Hide Intel in Far Cry 3, 4 and Primal | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 9cde0b57 | 2023-12-25 | [d3d9] Fix off-by-one when copying shader defined constants | correctness | port | Ported in 97e5c2b6b |
| a7a63b37 | 2023-12-27 | [util] Fix incorrect config option name | compatibility | later | D3D9 profile; not in the first batch |
| 2cf590f6 | 2023-12-29 | [util] Enable cached vertex buffers for Kenshi | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 5e06cf95 | 2023-12-27 | [util] Limit Sonic CD to 60 fps | compatibility | later | D3D9 profile; not in the first batch |
| a44dfabe | 2024-01-03 | [util] Set float emulation to Strict for UK Truck Simulator 1 | compatibility | port | Ported in 06b062a0f |
| eb806952 | 2024-01-09 | [util] Set deviceLossOnFocusLoss for Assassin's Creed 2 (#3763) | compatibility | later | D3D9 profile; not in the first batch |
| 854e06d3 | 2024-01-14 | [util] Enable deviceLossOnFocusLoss for BF2 and BF2142 | compatibility | later | D3D9 profile; not in the first batch |
| 4b8e8bed | 2024-01-19 | [d3d9] Move operators out of dxvk namespace | refactor | skip | Refactor or cleanup |
| 0cd41656 | 2024-01-19 | [d3d9] Add D3DDISPLAYMODEEX operator | correctness | present | cherry-picked by NVIDIA |
| 1568c263 | 2024-01-19 | [d3d9] Only add unique modes to mode list | correctness | present | cherry-picked by NVIDIA |
| 34d8e65f | 2024-01-23 | [util] cachedDynamicBuffers for Codename Panzers Phase One/Two | compatibility | port | Ported in 9404c869c |
| 89267b62 | 2024-01-19 | [build] Remove declspec UUID annotations | irrelevant | skip | Build or platform change |
| 7d9864c0 | 2024-01-25 | [d3d9] Only enable ATOC when rendering to MS RT | other | later | Not reviewed in detail |
| 0841f5fa | 2024-01-25 | [d3d9] Implement config option to disable rejecting reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 2ca8fdf8 | 2024-01-25 | [util] Disable counting losable resources for Supreme Ruler Ultimate | compatibility | later | D3D9 profile; not in the first batch |
| e2a46a34 | 2024-01-27 | [meta] Declare bool conversion operators as explicit | refactor | skip | DXVK backend change that follows the 2.x backend |
| 05cb963e | 2024-02-02 | [util] Set sync interval override for P3R | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 62d64bd6 | 2023-12-19 | [d3d9] Don't upload buffers before Up draws | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 5312ef1c | 2023-12-19 | [d3d9] Upload DYNAMIC+SYSMEM vertex and index data for each draw | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| fae78407 | 2024-02-06 | d3d9: Remove an unused variable | refactor | skip | Refactor or cleanup |
| e9a0fec5 | 2024-02-05 | [dxso] Clamp Exp when fast float emulation is enabled | correctness | port | Ported in 94b9d5cdf |
| ab359318 | 2024-02-05 | [d3d9] Fail GetRTData if src and dst have mismatching sizes | correctness | port | Ported in d546dde72 |
| 9491b56b | 2024-02-15 | util: Enable d3d11.ignoreGraphicsBarriers for Granblue Relink | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 738fd4f8 | 2024-02-15 | [d3d9] Don't actually unbind buffers | correctness | later | Fix; needs a check against this tree |
| 39c19e92 | 2024-02-15 | [d3d9] End scene on reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| f254afb4 | 2024-02-20 | [util] Enable strict float emulation for Red River | compatibility | port | Ported in 06b062a0f |
| c5a37d44 | 2024-02-20 | [d3d9] Handle null IBO when uploading data for draw | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 24d4c9c9 | 2024-02-22 | [util] Disable command lists for Granblue Fantasy Relink | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 49b18f03 | 2024-02-20 | [d3d9] Unbind buffers in EndScene & Reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| eaa732d0 | 2024-02-07 | [d3d9] Place GetFrontBufferData screenshot at window position | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| a0e39e94 | 2024-02-07 | [d3d9] Use most recently used swapchain for GetFrontBufferData | other | later | Not reviewed in detail |
| 428c98bc | 2024-01-27 | [util] Disable countLosableResources for all d3d9 Supreme Ruler games | irrelevant | skip | Profile comment or layout change |
| 20490b67 | 2024-03-06 | [d3d9] Fix missing else brackets in ResetSwapchain | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 0414bbe2 | 2024-03-06 | [dxgi] Add separate option to override vendor IDs for NVK | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| afec5cce | 2024-02-24 | [util] Remove some unneeded built in configs | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 0a699fdd | 2023-09-10 | Update config.cpp | compatibility | port | Ported in 06b062a0f |
| 85215b10 | 2024-03-06 | [d3d9] Respect vertex buffer offset when dynamically uploading geometry | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 72c86b82 | 2024-03-06 | [d3d9] Only unbind in EndScene if the game cleared the binding | other | later | Not reviewed in detail |
| 15ddadc4 | 2024-03-07 | [d3d9] Fix number of streams | correctness | skip | Not applicable: this tree does not loop over MaxNumVertexBindings there |
| 0776d764 | 2024-03-09 | build: Support building for ARM64EC | irrelevant | skip | Build or platform change |
| 8b8be7c2 | 2024-03-10 | [d3d9] Fix stream count in Reset | correctness | skip | Not applicable: see 15ddadc4 |
| e857b094 | 2024-03-15 | [dxgi] Change default behaviour of hideNvkGpu option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| ef4428ab | 2024-03-16 | [d3d9] Improve ReturnUnderlyingResource stub for 9on12 | compatibility | later | Interop / shared resources; low priority for Remix |
| 0beb18ef | 2024-03-16 | [d3d9] Wait for submission when calling ReturnUnderlyingResource | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 2e1a19c7 | 2024-03-17 | [util] Cap Dark Void to 60fps | compatibility | later | D3D9 profile; not in the first batch |
| 28c7c09b | 2024-03-20 | [dxgi] Remove useMonitorFallback option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 855b2746 | 2024-03-26 | [util] Remove TRAHA Global config | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 133f0794 | 2024-04-06 | [util] Remove framerate limiter for Nier Replicant Without mods, Nier Replicant runs faster when going above 60 FPS. The game had an official patch that implemented a framerate limiter to prevent this. This limiter is terrible, because it's not a stable 60 FPS, but a weird 57-58 FPS. There's no way to disable this in-game, it has to be done by editing a config file or through a mod. | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 571948cf | 2024-04-13 | [d3d9] Remove support for VERTEXSTATS queries | refactor | skip | Removal tied to upstream refactors |
| 8ba5256d | 2024-04-21 | [util] Set deferSurfaceCreation for 9th Dawn II | compatibility | port | Ported in d9a4ea386 |
| 3f27a0ee | 2024-04-26 | [util] Add a way to define app profiles exclusive to Steam Deck | irrelevant | skip | Profile comment or layout change |
| 462165da | 2024-04-26 | [util] Add Deck profile for Fallout 4 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 2970645f | 2024-04-26 | [dxvk] Fix push constant compatibility for pipeline libraries | refactor | skip | Needs graphics pipeline library (DXVK 2.x) |
| 79398b46 | 2024-04-27 | [util] Enable longMad for Red Faction Guerrila Remastered | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 29253da3 | 2024-04-29 | feat: add H2M-Mod to config | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| dacb8b43 | 2024-04-30 | [util] Add configs for Delta Force Xtreme 1 & 2 | compatibility | port | Ported in 9404c869c |
| b2789ab8 | 2024-05-02 | [d3d9] Validate DS format support during CheckDepthStencilMatch | correctness | later | API conformance (validation or return codes) |
| 611dc600 | 2024-05-08 | [d3d9] Do not support cube textures with depth formats | other | later | Not reviewed in detail |
| 6b76d70d | 2024-05-09 | [util] Enable d3d11.longMad for Guild Wars 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 7bad17c1 | 2024-05-10 | [util] Set deviceLossOnFocusLoss for The Sims 3 | compatibility | later | D3D9 profile; not in the first batch |
| 20185a53 | 2024-05-18 | [d3d9] Do not enable support for DF formats on Nvidia | other | later | Not reviewed in detail |
| c1f665f9 | 2024-05-18 | [util] Disable supportDFFormats for Prototype | compatibility | later | D3D9 profile; not in the first batch |
| 3420cd78 | 2024-05-17 | [dxvk] Use new Version helper to deal with driver version numbers | refactor | skip | DXVK backend change that follows the 2.x backend |
| dfc3776b | 2024-05-20 | [d3d9] FF: Fix a bunch of wine tests with FF texture coordinates | correctness | later | API conformance (validation or return codes) |
| 65dd3c7d | 2024-05-20 | [d3d9] Always enable STORAGE_BUFFER usage | other | later | Not reviewed in detail |
| f33453af | 2023-12-12 | [build] Add soversion to dxvk-native binaries | irrelevant | skip | Build or platform change |
| 83436a97 | 2023-03-31 | [meson] Set the stem of library names instead of the name_prefix | irrelevant | skip | Build or platform change |
| 2ff2c826 | 2023-03-31 | [build] Generate pkg-config metadata to link to DXVK libraries | irrelevant | skip | Build or platform change |
| 58d8ea2d | 2024-05-16 | [d3d11,d3d9,util] Add a config option for reproducible VK output | compatibility | later | Interop / shared resources; low priority for Remix |
| 07d007c6 | 2024-05-22 | [d3d9] Use customVendorId to determine the options vendorId | compatibility | later | New option |
| a1ce690c | 2024-05-22 | [d3d9] Determine DF format support in the options parser | compatibility | later | New option |
| 88980288 | 2024-03-21 | [d3d9] Rework uploading dynamic sysmem buffers at draw time | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 60cfafe0 | 2024-05-25 | [d3d9] Fix strange type in dynamic vertex upload | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| c9815268 | 2024-05-22 | [d3d9] Tweak VCache query results | other | later | Not reviewed in detail |
| 7df8017e | 2024-05-23 | [d3d9] FF: Apply transform flags count to generated texture coords | other | later | Not reviewed in detail |
| 79eea564 | 2024-05-28 | Add dxvk.maxChunkSize 1 to Origin | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| c2fd91f8 | 2024-06-02 | [util] Set longMad for Ghostbusters Remastered | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| ee18aecb | 2024-06-02 | [util] Add dxvk.maxChunkSize 1 for Origin Web Helper Service and Ubisoft Connect (UPlay) (#4047) | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 8d965359 | 2024-06-06 | [util] Set longMad for Watch_Dogs | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 5674abe4 | 2024-06-06 | [d3d9] Limit frame rate to display refresh as necessary | other | later | Not reviewed in detail |
| 1811f4b9 | 2024-06-06 | [util] Consolidate maxFrameRate options | refactor | skip | Reverted upstream (86f04a2d) |
| 018db923 | 2024-06-10 | [util] Disable Crysis 1 refresh rate fps limiter | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a8710f70 | 2024-06-12 | [util] Dead Space 2 - Lock to 60 FPS and enable vsync | compatibility | later | D3D9 profile; not in the first batch |
| 3e5eb166 | 2024-06-14 | [d3d8] Implement Dref scaling and fixed-function depth textures (#3565) | irrelevant | skip | D3D8 frontend, not in this tree |
| 22b13a94 | 2024-06-15 | [util] Enable deviceLossOnFocusLoss for The Force Unleased | compatibility | later | D3D9 profile; not in the first batch |
| d9994665 | 2024-06-17 | [util] Set strict floatEmulation for New Vegas | compatibility | later | D3D9 profile; not in the first batch |
| 4b4d323e | 2024-06-17 | [util] Set cachedDynamicBuffers for Dragonshard | compatibility | port | Ported in 9404c869c |
| a276e138 | 2024-06-19 | [util] Set deviceLossOnFocusLoss for Guild Wars | compatibility | later | D3D9 profile; not in the first batch |
| e9fc071d | 2024-06-20 | [d3d9] Validate D3DCREATE_PUREDEVICE usage | correctness | later | API conformance (validation or return codes) |
| 3145020a | 2024-06-21 | [d3d9] Only change vertex offset draw parameters when necessary | perf | later | Perf; measure on Remix first |
| fe0e02de | 2024-06-22 | [util] Set cachedDynamicBuffers for Battle for Middle-Earth 2 | compatibility | port | Ported in 9404c869c |
| 86f04a2d | 2024-06-26 | Revert "[util] Consolidate maxFrameRate options" | refactor | skip | Revert of 1811f4b9 |
| afbcd945 | 2024-06-26 | [util] Fix up recently added app profiles | compatibility | later | D3D9 profile; uses a negative maxFrameRate (needs 9a8009cd) |
| 1783b959 | 2024-07-04 | [util] Enable 60 FPS lock for WRC4 | compatibility | later | D3D9 profile; not in the first batch |
| 18035820 | 2024-06-23 | [d3d9] Remove vestigial D32 support | refactor | skip | Removal tied to upstream refactors |
| b03de97f | 2024-06-23 | [d3d9] Expose support for D16_LOCKABLE only on AMD | other | later | Not reviewed in detail |
| 5ae3cfe4 | 2024-07-04 | [d3d9] Fix DF Formats selection logic | correctness | later | Fix; needs a check against this tree |
| 17577294 | 2024-03-06 | [d3d9] Fix Wine test failures in StretchRect | correctness | later | API conformance (validation or return codes) |
| 9c898bd2 | 2024-03-07 | [d3d9] Skip presenting if D3D9Swapchain has no associated window | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 11db6d69 | 2024-03-07 | [d3d9] Fix reference leak in ProcessVertices | correctness | later | ProcessVertices reference leak; SWVP emulator interface differs here |
| 9e422a2b | 2024-03-11 | [d3d9] Fix default light | correctness | port | Ported in 82ac718cb |
| da814668 | 2024-03-12 | [d3d9] FF: Prevent specular highlights on the back of geometry | correctness | port | Ported in 55044facd |
| dc3dd0f8 | 2024-03-13 | [d3d9] Ignore the vertex stream offset in StateBlock::Capture | correctness | port | Ported in 226f7ea84 |
| ab12ffa0 | 2024-03-13 | [d3d9] Use max point size of Vulkan device | other | later | Not reviewed in detail |
| bbe851f6 | 2024-03-13 | [d3d9] Reduce data copied for SWVP vertex decls | other | later | Not reviewed in detail |
| fa5ce496 | 2024-07-04 | [util] Enable longMad for Watch_Dogs 2 and Legion | irrelevant | skip | Profile comment or layout change |
| b2d89db8 | 2024-06-03 | [util] Disable countLosableResources for Myst V | compatibility | later | D3D9 profile; not in the first batch |
| 9b504b50 | 2024-05-28 | [d3d9] Rework FF texcoord processing to be more accurate | other | later | Not reviewed in detail |
| ef0c6b6f | 2024-07-06 | [d3d9] Hardware cursor fixes | correctness | later | Cursor handling; Remix overlays its own UI |
| 60e523b4 | 2024-07-07 | [d3d8] Implement Direct3D 8 Frontend | compatibility | later | D3D9 profile; not in the first batch |
| 5a0f05ae | 2024-07-07 | [util] Disable counting losables for Battlefield 2 & 2142 | compatibility | later | D3D9 profile; not in the first batch |
| a63a5753 | 2024-07-07 | [util] Enable longMad for Tomb Raider Legend | compatibility | port | Ported in 9404c869c |
| fe9c875f | 2024-06-28 | [util] Set cachedDynamicBuffers for The Sims 2 and friends | compatibility | port | Ported in ee691f451 |
| 5f9ca080 | 2024-07-07 | [util] Enable deviceLossOnFocussLoss for Rise of Nations | compatibility | later | D3D9 profile; not in the first batch |
| a7ae5999 | 2024-07-08 | [d3d8] Slightly clean up CopyRects | irrelevant | skip | D3D8 frontend, not in this tree |
| cc87870b | 2024-07-08 | [d3d9] Relax stretch rect check in D3D8 mode | other | later | Not reviewed in detail |
| d456d0b4 | 2024-07-09 | [d3d9] StretchRect: Allow using an offscreen surface as dst when stretching | other | later | Not reviewed in detail |
| a8dbbcfa | 2024-07-11 | [util] Reorder config options by API | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| af1ba1b2 | 2024-07-15 | [d3d9] FF: Fix return type for lighting op | correctness | later | Fix; needs a check against this tree |
| b74725b2 | 2024-07-15 | Add Batman Arkham Knight Custom Vendor ID to config to fix the game for Intel GPUs | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 2a2d51e0 | 2024-07-16 | changed dxgi.customVendorId to dxgi.hideIntelGpu for Batman Arkham Knight | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| bac7ae29 | 2024-07-15 | [util] Disable countLosableResources for Inquisitor | compatibility | later | D3D9 profile; not in the first batch |
| b7d61b70 | 2024-07-19 | [d3d9] Use converted format for multiplane formats in CheckFormat | other | later | Not reviewed in detail |
| 031a98c2 | 2024-07-19 | [d3d9] Allow fourcc formats as the source in CheckFormatConversion | other | later | Not reviewed in detail |
| 10ab85c3 | 2024-07-13 | [d3d8] Remove references to d3d8.useShadowBuffers for now | irrelevant | skip | Profile comment or layout change |
| 33bf0912 | 2024-07-17 | [util] Add configs for Splinter Cell Conviction | compatibility | later | D3D9 profile; not in the first batch |
| 1077d6a6 | 2024-07-29 | [util] Set textureMemory to 0 for Operation Racoon City | compatibility | later | D3D9 profile; not in the first batch |
| e2b823b7 | 2024-07-31 | [util] Enable cachedDynamicBuffers for Art of Murder | compatibility | later | D3D9 profile; not in the first batch |
| 7c9e1ed7 | 2024-08-03 | [util] Set maxChunkSize to 1 for Epic Games Launcher | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e38693cd | 2024-08-07 | [d3d9] Allow querying ID3D9VkInteropTexture from surfaces | compatibility | later | Interop / shared resources; low priority for Remix |
| 5c987ea3 | 2024-07-31 | [d3d9] Handle map failure in texture initializer | refactor | skip | Refactor or cleanup |
| 0d0b9eaa | 2024-08-08 | [util] Enable d3d11.longDot for Trails through Daybreak | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 6308266a | 2024-08-13 | [d3d9] Default to Strict floatEmulation for amdvlk 2024.Q3.1 | other | later | Not reviewed in detail |
| 60e04503 | 2024-08-12 | [d3d11] Enable longMad/longDot behaviour by default | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1e5b78e8 | 2024-08-12 | [d3d9] Enable longMad behaviour by default | correctness | later | longMad on by default changes VS math for every game; needs an A/B on Remix capture |
| 71e44b38 | 2024-08-14 | [util] Enable context lock for EDF6 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d89e324b | 2024-08-19 | [util] Remove obsolete Metro Exodus EE config | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 9a280b06 | 2024-08-24 | [util] Set maxDeviceMemory for GTA IV | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a5fc08d1 | 2024-07-10 | [util] Disable direct buffer mapping for Max Payne 1 | compatibility | later | D3D9 profile; not in the first batch |
| 8c58bef9 | 2024-08-15 | [util] Hide Intel for Kena: Bridge of Spirits to skip faulty water rendering path. | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d31b7997 | 2024-08-30 | [util] Enable cachedDynamicBuffers for APB:Reloaded | compatibility | port | Ported in 9404c869c |
| 38308d44 | 2024-08-30 | [build] added <algorithm> header for failed subcommand fix - `remove_if` | irrelevant | skip | Build or platform change |
| 04d558cf | 2024-08-29 | [util] Make frame rate limiter enablement heuristic more robust | refactor | skip | DXVK backend change that follows the 2.x backend |
| 11efd509 | 2024-07-30 | [d3d9] Handle remaining edge cases of Discard & Lockable | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 1b172344 | 2024-08-08 | [d3d9] Remove redundant scene check in StretchRect | perf | later | Perf; measure on Remix first |
| 7f8cfec4 | 2024-08-08 | [d3d9] Always set IsAttachmentOnly for CreateOffscreenPlainSurfaceEx | other | later | Not reviewed in detail |
| daccde76 | 2024-08-08 | [d3d9] Don't set IMAGE_USAGE_SAMPLED_BIT for non-msaa depth stencil surfaces | correctness | later | Fix; needs a check against this tree |
| 63506ee1 | 2024-09-07 | [util] Enable cachedDynamicBuffers for Steel Soldiers and FIFA 2003 | compatibility | later | D3D9 profile; not in the first batch |
| 565ec7e0 | 2024-09-06 | [dxvk] Remove dxvk.maxChunkSize option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1a1c3a42 | 2024-09-11 | [d3d9,d3d11] recreate swapchain on VK_SUBOPTIMAL_KHR | correctness | present | cherry-picked by NVIDIA |
| 3e269589 | 2024-09-11 | [util] Add a framecap for Pandora Tomorrow | compatibility | later | D3D9 profile; not in the first batch |
| 903f1af1 | 2024-09-12 | [d3d9] Fix PresentGDI | correctness | later | Fix; needs a check against this tree |
| 0a2f2275 | 2024-09-14 | [d3d8] Add a framecap for Chrome: Gold Edition | compatibility | later | D3D9 profile; not in the first batch |
| 010738c1 | 2024-09-17 | [d3d9] Add HUD item for FF shaders | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 11ec6035 | 2024-09-17 | [util] Enable cachedDynamicBuffers for Battle Mages | compatibility | port | Ported in 9404c869c |
| ddb59ae3 | 2024-09-13 | [dxvk] Add usage property to buffer views | refactor | skip | DXVK backend change that follows the 2.x backend |
| 828aaa5c | 2024-09-20 | [d3d9] Add missing TRANSFER_SRC usage for swapchain helper images | other | later | Not reviewed in detail |
| 46e81616 | 2024-09-20 | [d3d9] Enable shaderInt8 for ProcessVertices | other | later | Not reviewed in detail |
| 900edf55 | 2024-09-20 | [d3d9] Only create sampling image views if the image supports sampling | other | later | Not reviewed in detail |
| c8791a6b | 2024-09-20 | [d3d9] Don't emit 1-sized vector type in SWVP Emu | correctness | later | Fix; needs a check against this tree |
| c7cf0a73 | 2024-09-17 | [d3d8] Respect D3DCREATE_MULTITHREADED and make d3d8 thread safe | irrelevant | skip | D3D8 frontend, not in this tree |
| 07e7781c | 2024-09-22 | [d3d9] Only add meta image usage after determining layout | other | later | Not reviewed in detail |
| e6873031 | 2024-09-22 | [util] Cap Prince of Persia (2008) max fps | compatibility | port | Ported in d9a4ea386 |
| 7985ac9c | 2024-08-30 | [d3d9] Add R16 and AL16 as known unsupported formats | other | later | Not reviewed in detail |
| 97fb6e4f | 2024-08-30 | [d3d9] Clean up supported formats from GetUnsupportedFormatInfo | refactor | skip | Refactor or cleanup |
| 5bb8d09a | 2024-09-18 | [d3d9] Always use per-draw buffer uploads on pure SWVP devices | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 033104f3 | 2024-09-19 | [d3d9] Cleanup buffer memory flag selection | refactor | skip | Refactor or cleanup |
| 04ad9869 | 2024-09-19 | [d3d9] Add SWVP HUD item | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 5a08b3c4 | 2024-09-22 | [util] Cap fps for F.E.A.R 1 & expansions | compatibility | later | D3D9 profile; not in the first batch |
| ef8bad33 | 2024-09-19 | [d3d9] Optimize late buffer uploads | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 1d49f247 | 2024-09-19 | [d3d9] Remove incorrect early-exit | correctness | later | Fix; needs a check against this tree |
| ca3aa201 | 2024-09-22 | [d3d9] Improve comment about synchronization with ProcessVertices | refactor | skip | Refactor or cleanup |
| d7c2e3ac | 2024-09-24 | [d3d9] Use correct aspect mask when blitting depth stencil textures | correctness | later | Fix; needs a check against this tree |
| f5ca3cf5 | 2024-09-25 | [d3d9] ResolveZ: Only copy aspects that both images support | other | later | Not reviewed in detail |
| bb6d1b3b | 2024-08-18 | [util]Add support for Time Leap Paradise | compatibility | port | Ported in d9a4ea386 |
| 97091aad | 2024-07-14 | [util] Enable legacy DISCARD for Rayman 3 | compatibility | later | D3D9 profile; not in the first batch |
| 6f6e75b4 | 2024-09-22 | [d3d9] Use DxvkBufferAllocation where appropriate | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 1fd3c804 | 2024-09-23 | [dxvk] Remove DxvkBufferAllocation | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 088ba404 | 2024-09-23 | [dxvk] Rework buffer view creation | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 713b76be | 2024-09-26 | [dxvk] Rework image creation | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 25076d92 | 2024-09-26 | [dxvk] Rework image view creation | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 4ed50ec6 | 2024-09-27 | [d3d9] Fix UP buffer allocation | correctness | later | Fix; needs a check against this tree |
| 01a7457a | 2024-09-28 | [dxvk] Remove createImageView function | refactor | skip | DXVK backend change that follows the 2.x backend |
| 8195bea6 | 2024-09-28 | [dxvk] Remove DxvkBufferViewCreateInfo | refactor | skip | DXVK backend change that follows the 2.x backend |
| b4e69dce | 2024-09-28 | [dxvk] Remove DxvkImageViewCreateInfo | refactor | skip | DXVK backend change that follows the 2.x backend |
| 4635397b | 2024-09-26 | [dxvk] Implement sampler pool | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| c7dab6a4 | 2024-09-27 | [d3d9] Remove internal sampler pool | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 543b5c7a | 2024-09-27 | [d3d9] Optimize sampler state decoding | perf | later | Perf; measure on Remix first |
| 07dfeeb3 | 2024-09-27 | [d3d9] Move building sampler key to CS thread | irrelevant | skip | Build or platform change |
| e6f89062 | 2024-09-27 | [d3d9] Ensure that we stay below the maximum sampler count | other | later | Not reviewed in detail |
| 67d1285b | 2024-09-28 | [dxvk] Refactor meta blits | refactor | skip | DXVK backend change that follows the 2.x backend |
| b164d6e2 | 2024-09-28 | [d3d9] Do not proactively enable meta copy usage flags | other | later | Not reviewed in detail |
| 4807af01 | 2024-10-02 | [d3d9] Resolve whole image in StretchRect | other | later | Not reviewed in detail |
| 7d05a996 | 2024-10-02 | [d3d9] Fix StretchRect fast-path resolve | perf | later | Perf; measure on Remix first |
| 207e15eb | 2024-09-30 | [dxvk] Refactor swap chain blitter to use plain Vulkan | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 63b200f0 | 2024-09-30 | [dxvk] Reimplement HUD rendering to use Vulkan directly | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 2af3fde5 | 2024-10-02 | [d3d9] Remove additional swap chain context | refactor | skip | Removal tied to upstream refactors |
| 329d9a0b | 2024-10-04 | [dxvk] Add support for drawing a software cursor | refactor | skip | DXVK backend change that follows the 2.x backend |
| 9d19fa18 | 2024-10-03 | [d3d9] Set D3DRS_POINTSIZE_MIN to 0.0f in D3D8 compatibility mode | other | later | Not reviewed in detail |
| 7ff53219 | 2024-09-28 | [d3d9] Implement a software cursor | correctness | later | Cursor handling; Remix overlays its own UI |
| bbe82aa5 | 2024-10-05 | [d3d9] Fix (most) cursor-related Wine tests | correctness | later | API conformance (validation or return codes) |
| 980a8d18 | 2024-09-27 | [d3d9] Use D24X8 for D3DFMT_UNKNOWN depth stencils | other | later | Not reviewed in detail |
| 912a530d | 2024-10-04 | [d3d9] Enforce various CheckDeviceMultiSampleType validations | correctness | later | API conformance (validation or return codes) |
| 6e0c048b | 2024-10-04 | [d3d9] Return D3DERR_DEVICELOST on reset failure due to losable resources | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 088cf454 | 2024-10-04 | [d3d9] Validate 0 valued back buffer dimensions for fullscreen mode | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 559398f6 | 2024-10-06 | [d3d9] Check block alignment on texture creation with DXT formats | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| c614e537 | 2024-10-05 | [dxvk] Remove alignment parameter from staging buffers | refactor | skip | DXVK backend change that follows the 2.x backend |
| f67c8dd1 | 2024-10-05 | [dxvk] Support format conversion in copyBufferToImage | refactor | skip | DXVK backend change that follows the 2.x backend |
| ad1f70be | 2024-10-06 | [dxvk] Support format conversion in copyImageToBuffer | refactor | skip | DXVK backend change that follows the 2.x backend |
| a1a3800b | 2024-10-08 | [d3d9] Fix software cursor reset and transitions | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 395ac388 | 2024-10-09 | [d3d9] Fix fixed function texture mask | correctness | port | Ported in 1ccee086b |
| d39d8798 | 2024-10-09 | [d3d9] Add a bunch of missing likely/unlikely to PrepareDraw | other | later | Not reviewed in detail |
| c8759418 | 2024-10-09 | [d3d9] Fix device reset handling when no cursor is set | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 48c57c11 | 2024-10-11 | [d3d9] Make proper use of X/YHotSpot for software cursors | correctness | later | Cursor handling; Remix overlays its own UI |
| 5ff365b9 | 2024-10-08 | [d3d9] Remove initializer context | refactor | skip | Refactor or cleanup |
| 5890eae3 | 2024-10-11 | [d3d9] Don't clear mipGenBit if texture is bound as attachment | correctness | later | Fix; needs a check against this tree |
| d00669cb | 2024-10-12 | [dxvk] Rename DxvkGpu{Event,Query} to Dxvk{Event,Query} | refactor | skip | DXVK backend change that follows the 2.x backend |
| 05d089be | 2024-10-06 | [d3d9] Silence unhandled render state D3DRS_ADAPTIVETESS_Y | correctness | later | Fix; needs a check against this tree |
| 421ead5b | 2024-10-11 | [d3d9] Only dirty frame buffer on render state changes if render area is impacted | correctness | later | Fix; needs a check against this tree |
| f65897be | 2024-10-13 | [d3d9] Remove a few unnecessary bitmasks | refactor | skip | Removal tied to upstream refactors |
| b707ca13 | 2024-10-14 | [util] Enable memoryTrackTest for Secret World Legends | compatibility | port | Ported in 1a7087321 |
| 98fa338b | 2024-10-14 | [util] Move Far Cry 2 config to d3d9 section | compatibility | port | Ported in 9404c869c |
| a044c639 | 2024-10-11 | [d3d9] Add ID3D9VkInteropDevice::CreateImage to create custom texture/surfaces | compatibility | later | Interop / shared resources; low priority for Remix |
| 25069dbd | 2024-10-14 | [d3d9] Add alternate constructor for D3D9Surface | other | later | Not reviewed in detail |
| eafdf0bf | 2024-10-14 | [d3d9] Add alternate constructor for D3D9Texture2D | other | later | Not reviewed in detail |
| 01a1c835 | 2024-10-14 | [d3d9] Fix spelling nit | refactor | skip | Refactor or cleanup |
| e4630685 | 2024-10-14 | [d3d9] Fix mismatched case in MultiSample and MultisampleQuality | correctness | later | Fix; needs a check against this tree |
| 763b82b1 | 2024-10-15 | [d3d9] Fix render target null pointer deref | correctness | skip | Not applicable: our Clear iterates m_boundRTs |
| f76fb759 | 2024-10-12 | [d3d9] Prefer GENERAL layout for read-only textures | other | later | Not reviewed in detail |
| 500c86c0 | 2024-10-16 | [d3d9] Replace staging buffer marker with monotonic fence | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| fb6e0ad6 | 2024-10-16 | [dxvk] Remove useless DxvkMarker class | refactor | skip | DXVK backend change that follows the 2.x backend |
| d72346f2 | 2024-10-16 | [dxvk] Rename various storage-related functions | refactor | skip | DXVK backend change that follows the 2.x backend |
| 6540ab4f | 2024-10-17 | [dxvk] Use DxvkPagedResource for CPU synchronization | refactor | skip | DXVK backend change that follows the 2.x backend |
| f813549c | 2024-10-20 | [util] Set forceSamplerTypeSpecConstants for Alpha Protocol | compatibility | later | D3D9 profile; not in the first batch |
| 24e04400 | 2024-10-23 | [util] Remove The Old Republic config | compatibility | later | D3D9 profile; not in the first batch |
| b4ed1081 | 2024-10-22 | [d3d9] Remove format conversion context | refactor | skip | Removal tied to upstream refactors |
| 4af31a9d | 2024-10-22 | [dxvk] Remove context type concept | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 640758af | 2024-10-27 | [util] Enable mapped memory clearing for Vindictus | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 678ccc72 | 2024-10-11 | [d3d9] Validate pRect dimensions with LockRect | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| a323abe0 | 2024-10-11 | [d3d9] Validate alignment for block aligned formats | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 63d9affd | 2024-10-11 | [d3d9] Add volume texture creation and locking validations | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| a0d48cd2 | 2024-10-11 | [d3d9] Adjust pLockedRect/pLockedBox clearing and return behavior | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 7ffe77f7 | 2024-10-13 | [d3d9] Validate pBox dimensions with LockBox | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 5ad84563 | 2024-10-18 | [d3d9] Improve the naming of GetFormatBlockSize helper | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 0259f552 | 2024-10-26 | wsi: Pass window state to setWindowMode | irrelevant | skip | Build or platform change |
| 201e1628 | 2024-10-29 | [d3d9] Add ID3D9VkInteropTexture to volumes | compatibility | later | Interop / shared resources; low priority for Remix |
| 84ce80f9 | 2024-10-30 | [d3d9] Use new fence wait function in WaitStagingBuffer | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| d4f25c81 | 2024-10-30 | [d3d11] Remove d3d11.maxImplicitDiscardSize option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 6e43bf6d | 2024-10-31 | [util] Include Prince of Persia main executable | compatibility | port | Ported in d9a4ea386 |
| f74b2cd6 | 2024-10-30 | [d3d9] Order modes descending by refresh rate | other | later | Not reviewed in detail |
| c606ab03 | 2024-11-02 | [util] Re-enable direct buffer mapping for Rayman 3 | compatibility | later | D3D9 profile; not in the first batch |
| 8d10bfec | 2024-11-02 | [d3d9] Remove unused readonly bitfield | refactor | skip | Refactor or cleanup |
| 19fea8c4 | 2024-11-02 | [d3d9] Move remaining texture bitmask updates to function | refactor | skip | Refactor or cleanup |
| 48d8e7c4 | 2024-11-02 | [d3d9] Improve code readability with comments | refactor | skip | Refactor or cleanup |
| 0e6be8a4 | 2024-10-13 | [d3d8/9] Add several Set/GetPrivateData validations | correctness | later | API conformance (validation or return codes) |
| 35352b9c | 2024-10-28 | [d3d8] Set D3D8 compatibility at D3D9 interface level | irrelevant | skip | D3D8 frontend, not in this tree |
| a7c2eb14 | 2024-10-14 | [d3d9] Add present params validations on device creation and reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| a760f4a8 | 2024-10-14 | [d3d9] Fixed return code on CheckAdapterFormat with D3DFMT_UNKNOWN | correctness | later | API conformance (validation or return codes) |
| 931796d1 | 2024-10-14 | [d3d9] Fix CreateQuery error return code | correctness | later | API conformance (validation or return codes) |
| 88be8256 | 2024-10-14 | [d3d9] Add additional ValidateBufferProperties validations | correctness | later | API conformance (validation or return codes) |
| 860d038d | 2024-10-14 | [d3d8] Don't record calls to MultiplyTransform | irrelevant | skip | D3D8 frontend, not in this tree |
| 27ceecfa | 2024-10-16 | [d3d9] Fix cursor X/YHotSpot coordinate validation | correctness | later | API conformance (validation or return codes) |
| 778b04f3 | 2024-11-03 | [d3d9] Properly destroy format conversion set and pipeline layouts | other | later | Not reviewed in detail |
| c4421348 | 2024-11-03 | [d3d9] Fix image layout for transfer-only images | correctness | later | Fix; needs a check against this tree |
| ccd19ce9 | 2024-11-03 | [d3d9] Properly handle block-compressed formats in ColorFill | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 3c885913 | 2024-11-03 | [d3d9] Fix various other ColorFill edge cases | correctness | later | Fix; needs a check against this tree |
| 64b35c5c | 2024-11-02 | [d3d9] Early return D3D_OK on present with a NULL m_window | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| eb980474 | 2024-11-06 | [d3d9] Skip some validations when hDeviceWindow is NULL | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 62970d24 | 2024-10-22 | [d3d9] Enable strict float emulation by default on new Nvidia drivers | other | later | Not reviewed in detail |
| bdf4cb76 | 2024-10-31 | [d3d11] Remove d3d11.maxDynamicImageBufferSize option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 125f0ace | 2024-11-10 | [util] Hide integrated graphics for Diablo 4 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a71603a2 | 2024-11-10 | [dxso] Fix lrp instruction | correctness | later | lrp as mul+add depends on longMad default (1e5b78e8) |
| bca3e8db | 2024-11-10 | [dxso] Explicitly emit long dot products | other | later | Not reviewed in detail |
| 6610f423 | 2024-11-10 | [dxso] Remove OpCross path for cross products | refactor | skip | Removal tied to upstream refactors |
| 3756e83a | 2024-11-10 | [dxso] Don't emit FOrdNotEqual | correctness | later | Fix; needs a check against this tree |
| 2dadad57 | 2024-11-12 | [util] Hide amd for GTA The Definitive Edition trilogy | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 4c09b006 | 2024-11-13 | [d3d9] Actually use correct sampler state for anisotropy | correctness | later | Anisotropy sampler state; sampler key code differs |
| 482e758a | 2024-11-16 | [dxgi] Introduce dxgi.enableUe4Workarounds option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| fefa4ee1 | 2024-11-17 | [util] Hide iGPU for WoW and WC3 Reforged | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 987c132d | 2024-11-17 | [util] Don't include original WC3 in Reforged config | irrelevant | skip | Profile comment or layout change |
| 7f1e5270 | 2024-11-16 | [d3d9] Change correctness factor to 0.5f | correctness | later | Fix; needs a check against this tree |
| 43b79bcb | 2024-11-23 | [util] Set Borderlands 2 floatEmulation to Strict | compatibility | port | Ported in 06b062a0f |
| 4f98844f | 2024-11-19 | [d3d9] Skip some validations for D3DDEVTYPE_NULLREF devices | correctness | later | API conformance (validation or return codes) |
| 77020760 | 2024-11-19 | [d3d9] Fix behavior on multiple image unlocks | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 97ad37e4 | 2024-11-20 | [d3d9] Validate RT parent device during SetRenderTarget | correctness | later | API conformance (validation or return codes) |
| 9e1bc1db | 2024-11-20 | [d3d9] Expand NormalizeTextureProperties validations | correctness | later | API conformance (validation or return codes) |
| 15db475e | 2024-11-23 | [dxvk] Always disable exclusive fullscreen | refactor | skip | DXVK backend change that follows the 2.x backend |
| d7bd3cd5 | 2024-11-23 | [d3d9] Remove d3d9.enableDialogMode option | compatibility | later | New option |
| b8ce4148 | 2024-11-27 | [util] Enable deferSurfaceCreation for the Codename Panzer series | compatibility | port | Ported in 9404c869c |
| 4e07302c | 2024-11-27 | [d3d9] Fix race condition in PIX events | correctness | later | Fix; needs a check against this tree |
| 218b67dd | 2024-11-28 | [d3d9] Use header back buffer limits during validation | refactor | skip | Refactor or cleanup |
| 640a2661 | 2024-11-25 | [d3d9] Only advertise SRCCOLOR2 & INVSRCCOLOR2 with 9Ex | other | later | Not reviewed in detail |
| 9c012c8e | 2024-11-26 | [d3d9] Skip recording MultiplyTransform calls in state blocks | correctness | port | Ported in 6a40169da |
| fb56668d | 2024-11-26 | [d3d9] Adjust out of bounds clip plane index handling | other | later | Not reviewed in detail |
| f23c7494 | 2024-11-27 | [d3d9] Validate supported formats in D3D9Surface::GetDC | correctness | later | API conformance (validation or return codes) |
| 17b85acc | 2024-11-27 | [d3d9] Adjust device reset failure error codes | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 027fe596 | 2024-11-27 | [d3d9] Fixes for state block specific behavior | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 8c4c814f | 2024-12-04 | [d3d9] Spec-constant out writes to clip distances when disabled | correctness | later | Fix; needs a check against this tree |
| d956b188 | 2024-12-04 | [d3d9] Clip plane compaction | other | later | Not reviewed in detail |
| d0940530 | 2024-10-24 | [d3d9] Implement several IDirect3DShaderValidator9 validations | correctness | later | API conformance (validation or return codes) |
| a78c1bd6 | 2024-11-25 | [util] Separate Borderlands 2 fixes from Borderlands: The Pre-Sequel | compatibility | port | Ported in 06b062a0f |
| d1abce3b | 2024-12-03 | [d3d9] Add GetInstanceExtensions to interop API | compatibility | later | Interop / shared resources; low priority for Remix |
| 4dd0afe1 | 2024-12-09 | [d3d9] Validate depth and stencil clears | correctness | later | API conformance (validation or return codes) |
| 0997719a | 2024-12-12 | [util] Set textureMemory to 0 for Dark Sector | compatibility | later | D3D9 profile; not in the first batch |
| fb8f79e4 | 2024-12-11 | [util] Add Sims 2 Content Manager & HomeCrafter Plus | compatibility | port | Ported in ee691f451 |
| d1789c4d | 2024-12-15 | [dxvk] Always consider env var configuration as active | irrelevant | skip | Profile comment or layout change |
| 67f4ec73 | 2024-12-16 | [util] Adjust "Earth Defense Force 5" position | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 65843aa0 | 2024-12-08 | [d3d9] Don't bind textures if texture type doesnt match | correctness | later | Fix; needs a check against this tree |
| acf93aa7 | 2024-12-12 | [util] Remove Alpha Protocol config | compatibility | later | D3D9 profile; not in the first batch |
| 1eb0c687 | 2024-12-20 | [d3d9] Fix messing up the dirty textures bitmask | correctness | later | Fix; needs a check against this tree |
| 39f4d804 | 2024-12-29 | [util] Limit frame rate to 60 fps for Arcana Heart 3 Love Max!!!!! and Arcana Heart 3 Love Max Six Stars!!!!!! Xtend | compatibility | later | D3D9 profile; not in the first batch |
| 7425a33d | 2025-01-01 | [util] Merge the Arcana Heart 3 entries | compatibility | later | D3D9 profile; not in the first batch |
| 8b9b46df | 2025-01-01 | [util] Cap fps in The hurricane of the Varstray | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 24f98c58 | 2025-01-04 | [d3d9] Skip texture type check when forceSpecConst option is active | compatibility | later | New option |
| 28a08cae | 2024-12-22 | [d3d8/9] Clear pLockedRect/Box contents universally on lock | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 150cb0d4 | 2024-12-21 | [d3d9] Refactor the D3D9ShaderValidator implementation | refactor | skip | Refactor or cleanup |
| 475bf4e9 | 2025-01-07 | [d3d9] Use cdw to iterate over register tokens in D3D9ShaderValidator | correctness | later | API conformance (validation or return codes) |
| 8ead28e4 | 2025-01-09 | [d3d8] Unify source file endline delimiters | irrelevant | skip | D3D8 frontend, not in this tree |
| 4151d35e | 2025-01-09 | [d3d8] Alignment and formatting improvements | irrelevant | skip | D3D8 frontend, not in this tree |
| 5487f8b9 | 2024-11-06 | [dxvk] Add convenience methods to check for debug utils support | refactor | skip | DXVK backend change that follows the 2.x backend |
| 1721be49 | 2025-01-08 | [d3d9] Set debug names for internal buffers | other | later | Not reviewed in detail |
| d9b5f092 | 2025-01-08 | [dxvk] Add debug region stack | refactor | skip | DXVK backend change that follows the 2.x backend |
| 07397305 | 2025-01-11 | [util] Enable zeroInitWorkgroupMemory for Far Cry 5 and New Dawn | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 7f4e2526 | 2025-01-11 | [util] Enable hideIntegratedGraphics for Bright memory | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| af45295a | 2025-01-06 | [d3d9] Log instruction stream in validator | correctness | later | API conformance (validation or return codes) |
| 5f5c9a4c | 2025-01-06 | [d3d9] Improve logging for PS input register indices | other | later | Not reviewed in detail |
| 3d401690 | 2025-01-13 | [util] Enable Strict floatEmulation for Max Payne 3 | compatibility | port | Ported in 06b062a0f |
| 073806df | 2025-01-12 | [dxvk] Move surface creation to the backend | refactor | skip | DXVK backend change that follows the 2.x backend |
| ab964655 | 2025-01-12 | [dxvk] Import swap chain images from the back-end | refactor | skip | DXVK backend change that follows the 2.x backend |
| 63e88deb | 2025-01-12 | [dxvk] Move deferSurfaceCreation handling to backend | refactor | skip | DXVK backend change that follows the 2.x backend |
| 43838d3d | 2025-01-12 | [dxvk] Move Vulkan swapchain management to backend | refactor | skip | DXVK backend change that follows the 2.x backend |
| 1d790970 | 2025-01-13 | [d3d9] Set color space for swap chain image | other | later | Not reviewed in detail |
| d85d07c0 | 2025-01-13 | [dxvk] Infer swap chain color space from image views | refactor | skip | DXVK backend change that follows the 2.x backend |
| 5134a4b3 | 2025-01-13 | [dxvk] Move HUD update and rendering into swapchain blitter | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 0f9245ff | 2025-01-13 | [d3d9,d3d11] Remove numBackBuffers option | compatibility | later | Interop / shared resources; low priority for Remix |
| c43cf689 | 2025-01-15 | [d3d9] Refactor presenter management | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| f9c3dd1f | 2025-01-15 | [d3d9] Remove explicit swapchain synchronization | refactor | skip | Removal tied to upstream refactors |
| 3dbe8ad4 | 2025-01-15 | [d3d9] Rework device reset detection | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 5ed2d990 | 2025-01-16 | [d3d9] Fix software cursor texture format | correctness | later | Cursor handling; Remix overlays its own UI |
| a3ba8fb4 | 2025-01-20 | [util] Add configs for Star Wars Empire at War | compatibility | port | Ported in 1a7087321 |
| b73f9d8e | 2025-01-22 | [util] Extend Battlefield 2/2142 config to Project Reality | irrelevant | skip | Profile comment or layout change |
| 273e4abb | 2025-01-17 | [dxvk] Add latency tracker to queue submissions | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 0e3ed707 | 2025-01-17 | [d3d9] Implement latency tracking | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 4fcbf527 | 2025-01-20 | [d3d9] Always limit to display refresh in low latency mode | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 539da5ab | 2025-01-20 | [dxvk] Refactor CS chunk queues | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 4184452f | 2025-01-10 | [d3d8] Validate srcRect/destPoint dimensions in UpdateTextureFromBuffer | irrelevant | skip | D3D8 frontend, not in this tree |
| d846e89a | 2025-01-11 | [d3d8] Improve handling of FullScreen_PresentationInterval | irrelevant | skip | D3D8 frontend, not in this tree |
| c6dc7e09 | 2025-01-21 | [d3d8/9] Proper (and age accurate) handling of d3d9.shaderModel = 0 | other | later | Not reviewed in detail |
| ad0920fe | 2025-01-26 | [d3d9] Relax SM2 minor version checks on shader creation | other | later | Not reviewed in detail |
| 5b212863 | 2025-01-26 | [d3d9] Adjust SM2 VS/PS caps | other | later | Not reviewed in detail |
| 1d90772e | 2025-01-27 | [d3d9] Report vertex texture filtering support only for SM3 | other | later | Not reviewed in detail |
| 06bdc88e | 2025-01-30 | [d3d8] Restore config for Splinter Cell | compatibility | later | D3D9 profile; not in the first batch |
| 0cad97d5 | 2025-01-30 | [util] Add scaleDref to SplinterCell2 | irrelevant | skip | Profile comment or layout change |
| 8776cf2c | 2025-01-29 | [util] Cleanup some deviceLossOnFocusLoss configs | compatibility | later | D3D9 profile; not in the first batch |
| 8860bde3 | 2025-01-30 | [util] Fix black screen after alt-tab for Trainz | compatibility | later | D3D9 profile; not in the first batch |
| dd5b28e5 | 2025-02-02 | [d3d8] Use D3D8 compatibilty mode to set HUD API level | irrelevant | skip | D3D8 frontend, not in this tree |
| bd69d1ef | 2025-01-28 | [d3d8/9] Use shader version macros where applicable | other | later | Not reviewed in detail |
| 3a34dae7 | 2025-02-02 | [dxvk] Work around locale-dependent regex parsing crash | irrelevant | skip | Profile comment or layout change |
| 54a26dde | 2025-02-07 | [util] Disable dcSingleUseMode for Cardfight!! Vanguard Dear Days 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 13554f18 | 2025-02-04 | [d3d9] Update software cursor position using SetCursorPosition | correctness | later | Cursor handling; Remix overlays its own UI |
| 80fb7e22 | 2025-02-05 | [d3d9,d3d11] Adjust flush heuristic on tiling GPUs | compatibility | later | Interop / shared resources; low priority for Remix |
| 80bfd2ed | 2025-02-12 | [util] Add functionality to support hashed app profiles | irrelevant | skip | Profile comment or layout change |
| c5a3aa73 | 2025-02-12 | [util] Enable dummy composition swap chain for an upcoming game | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 0aebedae | 2025-01-31 | [d3d8] Add shadow perspective divide hack for Splinter Cell | irrelevant | skip | Profile comment or layout change |
| 3269f921 | 2025-02-16 | [d3d9] Fix mismatching texture type mask updates | correctness | later | Fix; needs a check against this tree |
| 274c590a | 2025-02-14 | [d3d9] Set priority only for D3DPOOL_MANAGED/DEFAULT resources | other | later | Not reviewed in detail |
| 92523fc0 | 2025-02-15 | [d3d9] Fix x64 crash on SetVertexShaderConstantF index overflow | correctness | port | Ported in 9c47c8d76 |
| 4282829f | 2025-01-14 | [d3d9] Route operations on unmappable memory through allocator | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| d37a1384 | 2025-02-15 | [dxvk] Rename and repurpose ignoreGraphicsBarriers option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 317607e1 | 2025-02-19 | [d3d8/9] Prevent device child ref underruns on release | correctness | later | Fix; needs a check against this tree |
| 3716d48c | 2025-02-19 | [d3d8/9] Use numeric_limits globally | other | later | Not reviewed in detail |
| 22052106 | 2025-02-19 | [d3d9] Relax logging level on validateGammaRamp | correctness | later | API conformance (validation or return codes) |
| f7d56886 | 2025-02-21 | [d3d9] Fix sampler slot correction not respecting dmap texture | correctness | later | Fix; needs a check against this tree |
| 94b48c16 | 2025-02-21 | [d3d9] Slightly clean up sampler slot handling | refactor | skip | Refactor or cleanup |
| 016f05a7 | 2025-02-21 | [dxvk] Implement draw batching via VK_EXT_multi_draw | refactor | skip | DXVK backend change that follows the 2.x backend |
| e3f047a9 | 2025-02-23 | [d3d11] Remove d3d11.dcSingleUseMode option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a0c8bbaf | 2025-02-24 | [dxvk] Add method to retrieve resource debug names | refactor | skip | DXVK backend change that follows the 2.x backend |
| fcbdff3b | 2025-02-26 | [d3d9] Set correct input topology for SWVP emulation | correctness | later | Fix; needs a check against this tree |
| 11dc0e7c | 2025-02-26 | [util] Enable compute UAV barriers for Watch Dogs 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| fed51e6c | 2025-03-05 | [dxso] Fix uninitialized m_maxDefinedConstant variable | correctness | port | Ported in bde1d61bd |
| d8eb4d0d | 2025-03-06 | [util] Spoof vendor ID for CivCity: Rome | compatibility | port | Ported in e86b7fd8d |
| 542e0d2a | 2025-03-06 | [dxvk,d3d9,d3d11] Decouple ID3DUserDefinedAnnotation from internal markers | compatibility | later | Interop / shared resources; low priority for Remix |
| 91cde7f9 | 2025-03-07 | [d3d9] Remove double presents with SyncInterval > 1 | refactor | skip | Removal tied to upstream refactors |
| bbdab683 | 2025-03-05 | [dxvk/d3d9] Adjust/remove several loggers | refactor | skip | Removal tied to upstream refactors |
| ed071005 | 2025-03-05 | [d3d8/9] Enforce SM1 when in D3D8 compatibility mode | other | later | Not reviewed in detail |
| 066fd1bb | 2025-03-05 | [d3d9] Validate block aligned format mip > 0 dimensions as well | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| e36121bf | 2025-03-05 | [d3d9] CheckDeviceFormat will error out for Vertex/IndexBuffer RTypes | other | later | Not reviewed in detail |
| 3f746093 | 2025-03-05 | [d3d8] Enforce the pCaps->MaxVertexShaderConst limit on VS creation | irrelevant | skip | D3D8 frontend, not in this tree |
| fe10e443 | 2024-04-10 | [d3d9] Add option for extra frame buffer to fix GetFrameBufferData | correctness | later | Fix; needs a check against this tree |
| ce2fc317 | 2025-03-07 | [util] Enable extra frontbuffer for Silent Hill 2 | compatibility | later | D3D9 profile; not in the first batch |
| cd409187 | 2025-03-07 | [d3d9] Remove misleading comment | refactor | skip | Refactor or cleanup |
| 1b529c71 | 2025-03-07 | [d3d9] Remove mentions of SWAPEFFECT_COPY_VSYNC | refactor | skip | Removal tied to upstream refactors |
| ed87cd58 | 2025-03-07 | [d3d9] More or less minor static analysis nits | irrelevant | skip | Build or platform change |
| 4543c76e | 2025-03-08 | [dxso] Fix uninitialized variables and other static analysis hints | irrelevant | skip | Build or platform change |
| 209b07f7 | 2025-03-10 | [dxvk] Extend resolveImage to support depth-stencil resolves | refactor | skip | DXVK backend change that follows the 2.x backend |
| 99093e6c | 2025-03-10 | [dxvk] Remove resolveDepthStencilResolve method | refactor | skip | DXVK backend change that follows the 2.x backend |
| a59dde38 | 2025-03-09 | [d3d9] Consolidate shader const related fields | refactor | skip | Refactor or cleanup |
| 05e26f51 | 2025-03-11 | [dxvk,d3d11,d3d9] Refactor viewport updates | compatibility | later | Interop / shared resources; low priority for Remix |
| 2ad721b4 | 2025-03-14 | [d3d8/9] Validate D3DDEVTYPE on device creation and cap queries | correctness | later | API conformance (validation or return codes) |
| a5370e4f | 2025-03-20 | [util] Make AC Shadows app profile public | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| d3a23141 | 2025-03-23 | [dxvk] Remove subresource parameter from initImage | refactor | skip | DXVK backend change that follows the 2.x backend |
| bdb9e4f8 | 2025-03-23 | [d3d9] Clamp depth bounds to 0..1 range | correctness | later | Depth bounds clamp; conflicts |
| 186866c8 | 2025-03-24 | [d3d9] Do not resolve in StretchRect if src and dst are at the same sample count | other | later | Not reviewed in detail |
| f460af89 | 2025-03-24 | [util] Enable cached constant buffers for Hitman 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 2c0a94a0 | 2025-03-24 | [util] Enable cachedDynamicBuffers for Lego Indiana Jones: The Original Adventures | compatibility | port | Ported in 9404c869c |
| ce6802ad | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor depth-stencil state object | compatibility | later | Interop / shared resources; low priority for Remix |
| 45dbcc9b | 2025-03-22 | [dxvk,d3d9,d3d11] Normalize depth-stencil state in front-end | compatibility | later | Interop / shared resources; low priority for Remix |
| 6e1241a7 | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor multisample state object | compatibility | later | Interop / shared resources; low priority for Remix |
| 57c79943 | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor logic op state object | compatibility | later | Interop / shared resources; low priority for Remix |
| 5a68f480 | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor blend state object | compatibility | later | Interop / shared resources; low priority for Remix |
| 089de0e7 | 2025-03-22 | [dxvk,d3d9,d3d11] Normalize blend state in front-end | compatibility | later | Interop / shared resources; low priority for Remix |
| 9d5cd5d0 | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor rasterizer state object | compatibility | later | Interop / shared resources; low priority for Remix |
| 70afbbce | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor input assembly state object | compatibility | later | Interop / shared resources; low priority for Remix |
| fc2b552d | 2025-03-22 | [dxvk,d3d9,d3d11] Refactor input layout objects | compatibility | later | Interop / shared resources; low priority for Remix |
| e5a94112 | 2025-03-21 | [dxvk] Use clearRenderTarget for discards as well | refactor | skip | DXVK backend change that follows the 2.x backend |
| 6bdca15e | 2025-03-30 | [util] Enable cachedDynamicBuffers for Lego Batman: The Videogame | compatibility | port | Ported in 9404c869c |
| 62b99d7b | 2025-03-31 | [dxvk] Move custom D3DDEVINFO_RESOURCEMANAGER declaration into dxvk namespace | refactor | skip | DXVK backend change that follows the 2.x backend |
| 77c063b2 | 2025-03-23 | [d3d9] Don't create shader module for format helper pipelines | refactor | skip | DXVK 2.x binding / shader interface |
| e5d0d10b | 2025-04-11 | [util] Disable direct image mapping for Rocketbirds 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 714a04cd | 2025-04-15 | [util] Report just below 2GB vram for Prototype | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| fe9a24d1 | 2025-04-15 | [util] Minor config.cpp cleanup | compatibility | port | Ported in 1a7087321 |
| b52156dd | 2025-04-23 | [util] Enable Strict floatEmulation for Thumper | compatibility | port | Ported in 06b062a0f |
| f2a75025 | 2025-04-24 | [dxso] Use new sincos operation | refactor | skip | Refactor or cleanup |
| 3c95ef0c | 2025-04-24 | [d3d9] Add option to enable sincos emulation | compatibility | later | New option |
| d121ec5d | 2025-04-24 | [d3d9] Implement hideVendorGpu logic for d3d9 | other | later | Not reviewed in detail |
| 064aae56 | 2025-04-24 | [util] Adjust configs and use new d3d9 options | compatibility | later | D3D9 profile; not in the first batch |
| 4799558d | 2025-04-30 | [d3d9] Gate the ATOC hacks behind respective vendor IDs | other | later | Not reviewed in detail |
| c5596c5c | 2025-05-03 | [d3d9] Enable ATOC with NONMASKABLE MSAA | other | later | Not reviewed in detail |
| 994551ac | 2025-05-03 | [d3d9] Reject creating textures with unsupported samples | correctness | later | API conformance (validation or return codes) |
| 54d37d96 | 2025-05-07 | [d3d9] Implement Get/SetClipStatus | other | later | Not reviewed in detail |
| f48d3e67 | 2025-05-11 | [d3d9] Properly report supported adapter/backbuffer formats | other | later | Not reviewed in detail |
| 079d3026 | 2025-05-10 | [dxvk] Rework DxvkBindingInfo | refactor | skip | DXVK 2.x binding / shader interface |
| db595aa2 | 2025-05-10 | [d3d9] Use new built-in pipeline creation for format conversion | refactor | skip | Refactor or cleanup |
| b7af5f2e | 2025-05-10 | [dxvk] Use generic push constant layout for linked pipelines | refactor | skip | DXVK 2.x binding / shader interface |
| c9a31991 | 2025-05-10 | [d3d9] Bind output for ProcessVertices as raw buffer view | other | later | Not reviewed in detail |
| 99f86841 | 2025-05-17 | [util] Add configs for alt-tabbing in Red Orchestra: Ostfront | compatibility | later | D3D9 profile; not in the first batch |
| 61b91ea4 | 2025-05-19 | [util] Set memoryTrackTest & maxAvaliableMemory for Pirate Hunter | compatibility | port | Ported in 1a7087321 |
| df992615 | 2025-05-19 | [dxvk] Only allow one image usage bit per view | refactor | skip | DXVK backend change that follows the 2.x backend |
| 0e6a29eb | 2025-05-19 | [d3d9] Set explicit view layout for RTVs and DSV | other | later | Not reviewed in detail |
| e20407d6 | 2025-05-19 | [dxvk] Remove render target layout from attachment info | refactor | skip | DXVK backend change that follows the 2.x backend |
| 7d3fde84 | 2025-05-19 | [dxvk] Use plain buffer view descriptor info for resource updates | refactor | skip | DXVK 2.x binding / shader interface |
| 397ed898 | 2025-05-19 | [d3d9] Use new resource binding method for format conversion | refactor | skip | Refactor or cleanup |
| 3d518058 | 2025-05-20 | [dxvk] Remove DxvkContextObjects | refactor | skip | DXVK backend change that follows the 2.x backend |
| a97039b6 | 2025-05-02 | [d3d9] Properly determine support for vendor hacks | other | later | Not reviewed in detail |
| 56023800 | 2025-05-23 | [dxvk] Support per-stage push data in context | refactor | skip | DXVK 2.x binding / shader interface |
| d5cbe230 | 2025-05-23 | [dxvk] Explicitly pass local and shared push constants to shaders | refactor | skip | DXVK 2.x binding / shader interface |
| 3e94db30 | 2025-05-23 | [d3d9] Use private push data for SWVP shader | refactor | skip | DXVK 2.x binding / shader interface |
| 5b9a2e76 | 2025-05-24 | [dxvk] Support binding sampler heap for built-in pipelines | refactor | skip | DXVK backend change that follows the 2.x backend |
| ba204190 | 2025-05-24 | [d3d9] Set up render state and descriptors for bindless samplers | refactor | skip | DXVK 2.x binding / shader interface |
| 189f248f | 2025-05-24 | [d3d9] Use bindless samplers for fixed-function PS | refactor | skip | DXVK 2.x binding / shader interface |
| 5cfa1258 | 2025-05-24 | [dxso] Use bindless samplers | refactor | skip | DXVK 2.x binding / shader interface |
| a5894c59 | 2025-05-26 | [d3d9] Adjust d3d9 specific depth stencil format support | other | later | Not reviewed in detail |
| 9afa095c | 2025-05-31 | [d3d9] Fix GetAvailableTextureMem (#4959) | correctness | later | Fix; needs a check against this tree |
| f27e3c9f | 2025-05-28 | [util] Add a deviceId spoof for Battle Engine Aquila | compatibility | port | Ported in e86b7fd8d |
| c59b60be | 2025-06-03 | [dxvk] Read adatper info from new device capability class | refactor | skip | DXVK backend change that follows the 2.x backend |
| 3e924608 | 2025-06-03 | [dxvk] Use new capability class for device creation | refactor | skip | DXVK backend change that follows the 2.x backend |
| cb6e82d0 | 2025-06-03 | [d3d9] Remove GetDeviceFeatures method | refactor | skip | Removal tied to upstream refactors |
| ea84682e | 2025-06-03 | [dxvk] Use proper extension list for extension providers | refactor | skip | DXVK backend change that follows the 2.x backend |
| 27423849 | 2025-06-05 | [dxvk] Remove depth bias enable from rasterizer state | refactor | skip | DXVK backend change that follows the 2.x backend |
| cc578fea | 2025-06-05 | [dxvk] Make depth bounds state fully dynamic | refactor | skip | DXVK backend change that follows the 2.x backend |
| 2bb89007 | 2025-06-06 | [d3d9] Improve SetRenderState state tracking | other | later | Not reviewed in detail |
| 06e5081f | 2025-05-31 | [d3d9] Clarify vendor hack support | other | later | Not reviewed in detail |
| 9604037c | 2025-06-07 | [d3d9] Don't end scene in Reset on Ex device | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| f4e67e53 | 2025-06-07 | [d3d9] Don't change offset when skipping binding null VB | correctness | later | Fix; needs a check against this tree |
| 53fbb0e7 | 2025-06-07 | [d3d9] Don't unbind buffers on Ex device | correctness | later | Fix; needs a check against this tree |
| 7570fd1a | 2025-06-07 | [d3d11] Remove d3d11.zeroWorkgroupMemory option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| fe6a45e4 | 2025-06-08 | [d3d9] Only show unsupported vendor hack warnings once | other | later | Not reviewed in detail |
| dedeee19 | 2025-06-09 | [d3d9/dxgi] Add a force refresh rate config option | compatibility | later | New option |
| 23166803 | 2025-06-11 | [util] Disable direct image mapping for LEGO City Undercover | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 6b1d4a38 | 2025-05-23 | [d3d9] Remove stored shader masks | refactor | skip | Removal tied to upstream refactors |
| 4d229e2a | 2025-05-23 | [d3d9] Remove unused IsDMAPSampler | refactor | skip | Refactor or cleanup |
| 9ae3ae4a | 2025-05-24 | [d3d9] Add dref clamp bits for VS textures | correctness | later | Fix; needs a check against this tree |
| 0d33e8d0 | 2025-05-24 | [d3d9] Move texture type bitmask update into UpdateTextureBitmasks | refactor | skip | Refactor or cleanup |
| 42eaad38 | 2025-05-24 | [d3d9] Remove cubemap tracking mask | refactor | skip | Removal tied to upstream refactors |
| cf4ed789 | 2025-05-24 | [d3d9] Remove lastSamplerType | refactor | skip | Removal tied to upstream refactors |
| cc67b2c0 | 2025-06-07 | [d3d9] Move texture tracking masks to struct | refactor | skip | Refactor or cleanup |
| 6d7ef862 | 2025-06-07 | [d3d9] Move render target tracking masks to struct | refactor | skip | Refactor or cleanup |
| 4e7ae144 | 2025-06-07 | [d3d9] Move vertex buffer tracking masks to struct | refactor | skip | Refactor or cleanup |
| d2ee3ffe | 2025-06-10 | [d3d9] Remove support for d3d9ex specific depth formats | refactor | skip | Removal tied to upstream refactors |
| bbf89c39 | 2025-06-13 | [dxvk] Remove total sampler counter | refactor | skip | DXVK backend change that follows the 2.x backend |
| 5afe6881 | 2025-06-13 | [d3d9] Remove sampler count HUD | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 1b2923ce | 2025-06-13 | [dxvk] Remove legacy dxvk.enableStateCache option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| f6d790ea | 2025-06-18 | [util] Fix black screen on return from the A.I.M. options window | compatibility | later | D3D9 profile; not in the first batch |
| e668e335 | 2025-06-19 | [d3d9] Remove the forceSwapchainMSAA config option | compatibility | later | New option |
| 9c5c310a | 2025-06-20 | [util] Don't count losable resources for Astebreed | compatibility | later | D3D9 profile; not in the first batch |
| e77e38ff | 2025-06-13 | [d3d9] Rework RT binding logic | other | later | Not reviewed in detail |
| d92325a0 | 2025-06-20 | [d3d9] Remove redundant GetImageView | perf | later | Perf; measure on Remix first |
| a1a126d0 | 2025-06-17 | [d3d9] Return error when creating lockable MS texture | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 528dbba2 | 2025-06-17 | [d3d9] Remove intermediate resolve in StretchRect | refactor | skip | Removal tied to upstream refactors |
| 6847d003 | 2025-06-15 | [d3d8/9] Clean up CheckDeviceFormat and CreateImageSurface formats | refactor | skip | Refactor or cleanup |
| e4989d11 | 2025-06-22 | [d3d9] Add more known unsupported formats | other | later | Not reviewed in detail |
| 7a322214 | 2025-06-21 | [d3d9] Fix NULL format CheckDeviceMultiSampleType behavior | correctness | later | Fix; needs a check against this tree |
| dd46e48b | 2025-06-26 | [util] disable d3d9.textureMemory for Dungeon Lords | compatibility | later | D3D9 profile; not in the first batch |
| fae0af9c | 2025-06-27 | [util] Disable losable resource tracking for SFC3 | compatibility | later | D3D9 profile; not in the first batch |
| 02c6caab | 2025-06-27 | [d3d9] Forward 9On12 args to device | compatibility | later | Interop / shared resources; low priority for Remix |
| 985fe554 | 2025-06-27 | [d3d9] Display whether device is a 9On12 device | compatibility | later | Interop / shared resources; low priority for Remix |
| c2d59b06 | 2025-06-28 | [util] Hide Nvidia GPU for Wargame: European Escalation | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| b6981142 | 2025-06-29 | [d3d9] Only use readonly depth layout if necessary | perf | later | Perf; measure on Remix first |
| 3de4924c | 2025-07-02 | [d3d9] Shared resource fixes (based on d3d9ex wine tests) (#5065) | compatibility | later | Interop / shared resources; low priority for Remix |
| 24fcf381 | 2025-07-03 | [util] Splinter Cell config option tweaks | compatibility | later | D3D9 profile; not in the first batch |
| 5c405989 | 2025-07-05 | [util] Add configs for GTR - FIA GT Racing Game | compatibility | later | D3D9 profile; not in the first batch |
| 2ac295f7 | 2025-06-27 | [util] Enable deviceLossOnFocusLoss for Test Drive Unlimited 2 | compatibility | later | D3D9 profile; not in the first batch |
| 118094c0 | 2025-07-11 | [d3d8/9] General and format helpers cleanup | refactor | skip | Refactor or cleanup |
| 614d6a7d | 2025-07-14 | [d3d9] Fix a couple of omissions | correctness | later | Fix; needs a check against this tree |
| 35aa75e6 | 2025-07-16 | [d3d9] Only fail on DISCARD\|READONLY locks of D3DPOOL_DEFAULT resources | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 629a5f0d | 2025-07-22 | [util] Enable d3d9.hideNvidiaGpu for Perilous Warp | compatibility | later | D3D9 profile; not in the first batch |
| 3265e4aa | 2025-08-01 | [util] Remove Crysis 3 vendor override | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 070a0a66 | 2025-07-27 | [d3d9] Adjust texture memory reporting | other | later | Not reviewed in detail |
| 9738d6c3 | 2025-07-27 | [util] Atempt to align most config profiles | compatibility | later | D3D9 profile; uses a negative maxFrameRate (needs 9a8009cd) |
| 1b213e8e | 2025-08-02 | [d3d9] Adjust DrawRect/TriPatch returns | correctness | later | API conformance (validation or return codes) |
| 5743ae96 | 2025-08-03 | [d3d9] Silence GetPresentStats warn loggers | other | later | Not reviewed in detail |
| 842b8128 | 2025-08-05 | [util] Add a custom device ID for Comanche 4 | compatibility | later | D3D9 profile; not in the first batch |
| 07f0d8eb | 2025-07-19 | [d3d9] Validate present parameters on calls to ResetEx | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 65f9c3e3 | 2025-07-19 | [d3d9] Validate SetMaximumFrameLatency MaxLatency values | correctness | later | API conformance (validation or return codes) |
| df0f17c4 | 2025-07-19 | [d3d9] Don't allow D3DPOOL_MANAGED use on extended devices | correctness | later | Fix; needs a check against this tree |
| 0286e994 | 2025-07-20 | [d3d9] Validate shader constant register count | correctness | later | Shader constant register count validation; conflicts |
| 61eec753 | 2024-07-09 | [d3d9] Add a modeCountCompatibility config option | compatibility | later | New option |
| 24761356 | 2025-07-05 | [util] Add modeCountCompatibility for the first 2 AquaNox games | compatibility | later | D3D9 profile; not in the first batch |
| 15dd7028 | 2025-07-13 | [d3d9] Add some missing texture validation | correctness | later | API conformance (validation or return codes) |
| e9156681 | 2025-07-13 | [d3d9] Don't add RT usage for every offscreen surface | correctness | later | Fix; needs a check against this tree |
| 835e98cf | 2025-07-13 | [d3d9] Improve naming in StretchRect | refactor | skip | Refactor or cleanup |
| c3125fe4 | 2025-08-08 | [util] Hide Nvidia in Fallout New Vegas | compatibility | later | D3D9 profile; not in the first batch |
| 5ef9412c | 2025-08-09 | [util] Cap Witcher 1 at 300 fps | compatibility | later | D3D9 profile; not in the first batch |
| 9f39fef2 | 2025-08-11 | [util] Cap Guilty Gear at 60 fps | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 3425b1c5 | 2025-08-11 | [util] Add a frame rate cap for Psi-Ops | compatibility | later | D3D9 profile; not in the first batch |
| 3a1c226f | 2025-08-06 | [d3d9] Properly set alpha test dirty flag when changing RT | correctness | later | Fix; needs a check against this tree |
| 9ba6d9fa | 2025-08-06 | [d3d9] Allow using StretchRect for simple copies on 9Ex devices | other | later | Not reviewed in detail |
| 91306f4f | 2025-08-06 | [d3d9] Consolidate ATOC & alpha test state changes | refactor | skip | Refactor or cleanup |
| 3daeb939 | 2025-08-06 | [d3d9] Correct spelling of Reference | refactor | skip | Refactor or cleanup |
| 120fa71f | 2025-08-10 | [d3d9] Address several d3d9Ex specific behaviors | other | later | Not reviewed in detail |
| bb2a46c6 | 2025-08-10 | [d3d9] Restrict ATI2 format use | other | later | Not reviewed in detail |
| 68b28b11 | 2025-07-24 | [d3d9] Don't use RT mask in UpdateActiveHazardsRT | correctness | later | Fix; needs a check against this tree |
| 9466f439 | 2025-07-25 | [d3d9] Rework BindFramebuffer | other | later | Not reviewed in detail |
| 59826f34 | 2025-08-08 | [d3d9] Avoid rebinding frame buffer based on shader change | perf | later | Perf; measure on Remix first |
| 34853122 | 2025-08-18 | [util] Enforce legacy DISCARD behavior for Top Spin | irrelevant | skip | Profile comment or layout change |
| d0c3cdee | 2025-08-16 | [d3d9] Handle setting/getting texture palettes | correctness | later | Fix; needs a check against this tree |
| ba88d0cd | 2025-08-16 | [d3d9] Handle setting/getting NPatchMode | correctness | later | Fix; needs a check against this tree |
| dee66496 | 2025-08-16 | [d3d9] Return 0 on calls to GetGPUThreadPriority | correctness | later | API conformance (validation or return codes) |
| 46488ce2 | 2025-08-17 | [d3d8] Use Set/GetNPatchMode for D3DRS_PATCHSEGMENTS | irrelevant | skip | D3D8 frontend, not in this tree |
| 448fd8fa | 2025-08-20 | [util] Track memory use for Alone in the Dark | correctness | port | Ported in 1a7087321 |
| 402c290e | 2025-08-24 | [d3d9] Update the new texture type before checking diff | other | later | Not reviewed in detail |
| bcb0abde | 2025-08-23 | [util] Set cachedDynamicResources for Everybody's Gone to the Rapture | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 1e1551e5 | 2025-08-22 | [d3d9] Fix crash in GetVulkanImageInfo for SYSMEM or NULL textures | correctness | later | Fix; needs a check against this tree |
| 84c233b6 | 2025-08-23 | [d3d9] Validate the cursor bitmap against adapter mode dimensions | correctness | later | API conformance (validation or return codes) |
| 97b220d9 | 2025-08-23 | [d3d9] Allow the creation of D3DDEVTYPE_REF devices | other | later | Not reviewed in detail |
| 5bf84f39 | 2025-08-20 | [d3d9] Bring back allowDiscard hack | other | later | Not reviewed in detail |
| 5621bd01 | 2025-08-20 | [util] Disable allowDiscard for Scarface | compatibility | port | Ported in d9a4ea386 |
| 1ed752a1 | 2025-08-28 | [util] Fix annoying config warning | irrelevant | skip | Profile comment or layout change |
| e0b23bf2 | 2025-08-27 | [dxvk] Factor out SPIR-V specific shader parts to separate class | refactor | skip | DXVK backend change that follows the 2.x backend |
| 618080e6 | 2025-08-27 | [dxvk] Decouple shader look-up hash from shader key | refactor | skip | DXVK backend change that follows the 2.x backend |
| 21b9e2f3 | 2025-08-29 | [dxvk] Don't explicitly pass topologies to shader creation | refactor | skip | DXVK backend change that follows the 2.x backend |
| f5070a52 | 2025-08-29 | [dxvk] Do not explicitly pass stage and I/O masks to shader creation | refactor | skip | DXVK backend change that follows the 2.x backend |
| 5028a7ea | 2025-08-29 | [dxvk] Move shader create info to SPIR-V shader class | refactor | skip | DXVK backend change that follows the 2.x backend |
| 494461e0 | 2025-08-30 | [dxvk] Clean up shader compile state handling | refactor | skip | DXVK backend change that follows the 2.x backend |
| d83e2213 | 2025-09-01 | [d3d9] Override sampler anisotropy only on multi-mip textures | other | later | Not reviewed in detail |
| 0f73e919 | 2025-09-01 | [d3d9] Only set anisotropy for textures with multiple mips | other | later | Not reviewed in detail |
| 6450041b | 2025-09-02 | [util] Add a frame cap for HoAE | compatibility | later | D3D9 profile; not in the first batch |
| 465b5b7e | 2025-09-02 | [util] Enable dummy composition for Anno 117 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e6d17a15 | 2025-09-01 | [d3d9,d3d11] Remove per-API sincos lowering option | compatibility | later | Interop / shared resources; low priority for Remix |
| 7cb8e5fc | 2025-08-28 | [d3d9] Add feedback loop usage to attachment image views | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 71ed2e6d | 2025-08-31 | [d3d8/9] Adjust invalid viewport handling | other | later | Not reviewed in detail |
| 07084e12 | 2025-09-07 | [d3d9] Only D3D_OK D3DMULTISAMPLE_NONE checks for NULL format | other | later | Not reviewed in detail |
| 94a6e30e | 2025-09-06 | [dxvk] Consolidate shader compile option structs | refactor | skip | DXVK backend change that follows the 2.x backend |
| ec9e21bd | 2025-08-14 | [d3d9] Statically register all linker slots used by FF | other | later | Not reviewed in detail |
| 2b271de8 | 2025-09-08 | [dxso] Emit all outputs that fixed function fragment shaders expect | correctness | later | Fix; needs a check against this tree |
| c9b39796 | 2025-09-09 | [d3d9] Remove unused m_isgn from D3D9FFShader | refactor | skip | Refactor or cleanup |
| c5d77d71 | 2025-08-16 | [d3d9] Reorganize spec constants | refactor | skip | DXVK 2.x binding / shader interface |
| 8e658c2f | 2025-09-09 | [d3d9] Slightly improve spec const naming | refactor | skip | DXVK 2.x binding / shader interface |
| d8eff863 | 2025-09-09 | [d3d9] Turn drefScaling into spec constant | refactor | skip | DXVK 2.x binding / shader interface |
| 17f083b0 | 2025-09-09 | [d3d9] Reduce size of VertexBlendCount field | other | later | Not reviewed in detail |
| eadb1ae8 | 2025-09-09 | [d3d9] Clean up parts of the fixed function generator | refactor | skip | Refactor or cleanup |
| dd427a2d | 2025-09-09 | [d3d9] Use existing spec consts instead of FF key | refactor | skip | DXVK 2.x binding / shader interface |
| 80bf9ad2 | 2025-09-10 | [d3d9] Dirty FF shader when texture slot is change from/to NULL | correctness | later | Fix; needs a check against this tree |
| 87f809d8 | 2025-09-02 | [d3d9] Relax UpdateTexture size validations for single mip textures | correctness | later | API conformance (validation or return codes) |
| 16e70cd7 | 2025-09-12 | [d3d9] Dirty FF shaders when changing texture transform flags | correctness | later | Fix; needs a check against this tree |
| 6782b8cb | 2025-09-10 | [d3d9] Remove leftovers in unused FF VS Key | refactor | skip | Refactor or cleanup |
| bd918976 | 2025-09-12 | [d3d9] Improve naming of VS key field | refactor | skip | Refactor or cleanup |
| 460b27db | 2025-09-13 | [d3d9] Implement shared helper for GLSL ubershaders | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 83b64c95 | 2025-09-13 | [d3d9] Remove unused fields from FF FS shader key | refactor | skip | Refactor or cleanup |
| 9c8f76e1 | 2025-09-10 | [d3d9] Implement fixed function vertex ubershader | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| f5b82033 | 2025-09-10 | [d3d9] Hook up fixed function vertex ubershader | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| dd429f6a | 2025-09-10 | [d3d9] Implement fixed function fragment ubershader | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| a53dda55 | 2025-09-10 | [d3d9] Hook up fixed function fragment ubershader | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 8d58e7c5 | 2025-09-10 | [d3d9] Optimize FF fragment shader using spec constants | refactor | skip | DXVK 2.x binding / shader interface |
| 4b079535 | 2025-09-11 | [d3d9] Enable FF ubershaders by default | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 900cc271 | 2025-09-13 | [d3d9] Show ubershader in FF HUD element | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| d5f8c2f6 | 2025-09-13 | [d3d9] FF Ubershader: Fix reading spec const texture stage args | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 3798b12f | 2025-09-13 | [d3d9] FF Ubershader: Improve readability of stage count spec const | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 66ac77a7 | 2025-09-17 | [d3d9] Clean up cursor logic | refactor | skip | Refactor or cleanup |
| c9feb0d6 | 2025-09-17 | [d3d9] Advertise multisample NULL format checks | other | later | Not reviewed in detail |
| f9634335 | 2025-09-21 | [util] Rename custom device name for Crazy Machines 3 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 7216d72a | 2025-09-21 | [util] Set custom device name for World of Final Fantasy | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 10aacfdf | 2025-09-23 | [d3d9] Don't hold adapter pointer in format table | correctness | later | Fix; needs a check against this tree |
| 39024310 | 2025-09-19 | [dxso] Copy DXBC fourcc implementation | other | later | Not reviewed in detail |
| 3d2ee752 | 2025-09-19 | [d3d9] Factor in global specular state during texture blending | other | later | Not reviewed in detail |
| 0c39a13d | 2025-09-21 | [d3d9] Fix up texture blending specular use for the FF shader compiler | correctness | later | Fix; needs a check against this tree |
| dfa52b6c | 2025-09-18 | [d3d9] Fast skip DrawIndexedPrimitive(UP) calls with 0 NumVertices | perf | later | Perf; measure on Remix first |
| 34a37f3c | 2025-09-26 | [d3d11] Remove legacy config options | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 54746cea | 2025-09-26 | [d3d11] Rename forceVolatileTgsmAccess option | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 87c08e95 | 2025-09-29 | [util] Enable LDS barrier insertion for Far Cry 5 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e37ab162 | 2025-09-30 | [util] Hide nvidia in RaceRoom Racing Experience | compatibility | later | D3D9 profile; not in the first batch |
| 9a80ae60 | 2025-09-29 | [util] Disable direct buffer mapping for Lego Racers 2 | compatibility | later | D3D9 profile; not in the first batch |
| 49ab4b95 | 2025-10-01 | [util] Disable direct buffer mapping for Smash Up Derby | compatibility | later | D3D9 profile; not in the first batch |
| f55ef653 | 2025-10-01 | [util] Extend Far Cry 5 config to New Dawn | irrelevant | skip | Profile comment or layout change |
| 8bd72bb2 | 2025-09-26 | [d3d9] Relax lock dimension checks on SYSTEMMEM surfaces | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 43db11fb | 2025-09-24 | [d3d9] Lock the device when applying or capturing state blocks | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 49d928f5 | 2025-09-25 | [d3d8] Refactor d3d8 state blocks | irrelevant | skip | D3D8 frontend, not in this tree |
| 55ca713b | 2025-09-25 | [d3d8/9] Validate state block types on creation | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 53eee4b7 | 2025-10-14 | [util] Splinter a Steam profile for Pandora Tomorrow | compatibility | later | D3D9 profile; not in the first batch |
| b3da8104 | 2025-10-19 | [util] Enable memoryTrackTest for Blitzkrieg 2 | compatibility | port | Ported in 1a7087321 |
| f33133ac | 2025-10-22 | [util] Disable counting losables for Core Awaken 2.5 (#5281) | compatibility | later | D3D9 profile; not in the first batch |
| 95d591a8 | 2025-10-26 | [d3d9] Pass vertex color1 through in FF VS if specular is disabled | correctness | later | Fix; needs a check against this tree |
| 0bc20a25 | 2025-10-26 | [d3d9] Fix color default in FF & shaders | correctness | later | Fix; needs a check against this tree |
| aa73fdb0 | 2025-10-26 | [d3d9] FF: Fix material source selection fallback | correctness | later | Fix; needs a check against this tree |
| 88fe0c68 | 2025-10-25 | [d3d9] FF Ubershader: Apply SELECTMASK when checking for texture usage | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 675eca87 | 2025-10-25 | [d3d9] Move non-dirty flags out of D3D9DeviceFlag | refactor | skip | Refactor or cleanup |
| 3eaf3abb | 2025-10-25 | [d3d9] Clean up SetRenderTargetInternal | refactor | skip | Refactor or cleanup |
| faae3ed1 | 2025-10-02 | [wsi] Introduce separate methods to save and restore window state | irrelevant | skip | Build or platform change |
| 670875f6 | 2025-10-30 | [d3d9] Fix default color output for color0 | correctness | later | Fix; needs a check against this tree |
| e9ad9056 | 2025-10-30 | [d3d9] FF: Slightly clean up fog calculation | refactor | skip | Refactor or cleanup |
| eb1e3757 | 2025-10-15 | [util] Fix D3DKMT_DESTROYDCFROMMEMORY hDc member name | refactor | skip | DXVK backend change that follows the 2.x backend |
| f5ad1aaf | 2025-10-15 | [util] Move initializers out of D3DKMT_DESTROYDCFROMMEMORY | refactor | skip | DXVK backend change that follows the 2.x backend |
| f4b58194 | 2025-10-31 | [d3d11,util] Add GPU execution cost heuristic for submissions | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 66ad437d | 2025-11-10 | [util] enable zeroMappedMemory for Ridge Racer Unbounded | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| ee76585b | 2025-11-11 | [dxvk] Bump spec constant count to 20 | refactor | skip | DXVK 2.x binding / shader interface |
| 3902edbe | 2025-11-17 | [dxvk] Remove built-in latency tracker | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| b14661b4 | 2025-11-14 | [d3d9] Keep depth write state in mind when trying to keep DS hazards | other | later | Not reviewed in detail |
| 3924944d | 2025-11-15 | [d3d9] Keep COLORWRITEMASK state in mind when trying to keep RT hazards | other | later | Not reviewed in detail |
| 1925f044 | 2025-11-22 | [d3d9] Disable depth bounds test if min > max | correctness | later | Depth bounds min > max; conflicts |
| bb13ca06 | 2025-10-15 | [d3d9] Use D3DKMTEscape to set shared resource metadata | compatibility | later | Interop / shared resources; low priority for Remix |
| d972ca4c | 2025-10-08 | [d3d9] Validate shared handle D3DKMT descriptor on import | refactor | skip | DXVK 2.x binding / shader interface |
| 563d3ec5 | 2025-10-15 | [d3d11] Use D3DKMTEscape to set shared resource metadata | compatibility | later | Interop / shared resources; low priority for Remix |
| 8d58ad2f | 2025-11-26 | [d3d9] Use D_READONLY_S_OPTIMAL layout for DS sampling views | other | later | Not reviewed in detail |
| 43178ea8 | 2025-11-26 | [dxvk] Re-add latency tracker | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 9a8009cd | 2025-11-27 | [d3d9] Allow negative values for maxFrameRate option | compatibility | later | Negative maxFrameRate needs the 2.x presenter limiter; blocks many -60 profiles |
| d7636d09 | 2025-11-27 | [util] Adjust app profiles to use negative frame rate limits | compatibility | later | D3D9 profile; uses a negative maxFrameRate (needs 9a8009cd) |
| a4f0ba1d | 2025-12-04 | [d3d9] Implement D3DRS_MULTISAMPLEANTIALIAS | other | later | Not reviewed in detail |
| 1855e5e5 | 2025-12-02 | [d3d9] Add ID3D9VkExtInterface | other | later | Not reviewed in detail |
| 0118eaff | 2025-12-05 | [d3d9] Deprecate ID3D9VkExtSwapchain::UnlockAdditionalFormats | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| c1e9cb43 | 2025-11-27 | [d3d9] Change buffer strategy to match Windows | perf | later | Buffer placement rework; base of cachedWriteOnlyBuffers; Remix reads mapped buffers on the CPU |
| e553bcce | 2025-11-27 | [d3d9] Ignore DISCARD if READONLY is also set | correctness | later | Fix; needs a check against this tree |
| 39479c0f | 2025-11-27 | [d3d9] Ignore READONLY for non-managed | correctness | later | Fix; needs a check against this tree |
| 57ec2b2d | 2025-11-27 | [d3d9] Port over discard allocation throttling to D3D9 | perf | later | Perf; measure on Remix first |
| 449750f4 | 2025-11-28 | [util] Remove allowDirectBufferMapping configs that were meant to work around OOM | compatibility | later | D3D9 profile; not in the first batch |
| ae26b84d | 2025-11-28 | [d3d9+util] Rename cachedDynamicBuffers to cachedWriteOnlyBuffers | refactor | skip | Option rename tied to c1e9cb43; this tree keeps d3d9.cachedDynamicBuffers |
| a0358f85 | 2025-12-04 | [util] Remove unnecessary configs | compatibility | later | D3D9 profile; not in the first batch |
| 3972f883 | 2025-12-05 | [d3d9] Ignore buffer lock range for static D3DPOOL_DEFAULT buffers | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 6cec53d9 | 2025-12-08 | [util] Remove redundant cachedWriteOnlyBuffers config options | compatibility | later | D3D9 profile; uses a negative maxFrameRate (needs 9a8009cd) |
| 2f7b9a6d | 2025-12-08 | [dxvk] Get rid of explicit code buffers for built-in shaders | refactor | skip | DXVK backend change that follows the 2.x backend |
| f65510db | 2025-12-08 | [d3d9] Initialize config options early | refactor | skip | Refactor or cleanup |
| d772a8e4 | 2025-12-08 | [d3d9] Re-add support for sample rate shading with fixed-function shaders | correctness | later | Fix; needs a check against this tree |
| 4628ac86 | 2025-12-10 | [d3d9] Optimize the remaining FF arguments | perf | later | Perf; measure on Remix first |
| 21eb344f | 2025-12-11 | [d3d9] Activate focus window when creating fullscreen device | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| f618e58b | 2025-12-17 | [d3d9] Pass view format and swizzle to samplers | other | later | Not reviewed in detail |
| b650faef | 2025-12-18 | [d3d9] Bump allowed amount of staging/discard memory in flight | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 584041f1 | 2025-12-30 | [util] Add a frame cap for The Evil Within 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 8c877df0 | 2026-01-08 | [d3d9] Take format into account when checking MSAA compatibility | other | later | Not reviewed in detail |
| bb91ac52 | 2026-01-10 | [d3d9] Overhaul sample count validation when creating textures | correctness | later | API conformance (validation or return codes) |
| c10c75ef | 2026-01-06 | [d3d8] Readjust ZBIAS factor | irrelevant | skip | D3D8 frontend, not in this tree |
| aa7f7792 | 2026-01-19 | Support for MW:R HMW-Mod | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 08925bae | 2026-01-19 | Add comment for AMD AGS crash in HMW-Mod | irrelevant | skip | Profile comment or layout change |
| 77d57f2d | 2026-01-19 | [util] Add infinite warfare / iw7-mod support (#5457) | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| a5d2ded4 | 2026-01-17 | [d3d9] Allow cursor clears on hidden HW cursors | correctness | later | Cursor handling; Remix overlays its own UI |
| 71d09f95 | 2026-01-23 | [d3d9] Only check hardware sample support if we create vulkan image | other | later | Not reviewed in detail |
| 9d5dd98a | 2026-01-23 | [d3d9] Add a tiny amount of padding to texture mapping data | other | later | Not reviewed in detail |
| 252bdd86 | 2026-01-23 | [util] Remove BlazBlue textureMemory workaround | compatibility | later | D3D9 profile; not in the first batch |
| c27ef1c4 | 2026-01-23 | [util] Spoof GPU for Sims 3 | compatibility | port | Ported in e86b7fd8d |
| 2c25e68e | 2026-01-18 | [d3d11,d3d9] Use SHADER_READ_ONLY_OPTIMAL layout for textures again | compatibility | later | Interop / shared resources; low priority for Remix |
| 81157fc8 | 2026-01-18 | [d3d9] Recreate images with feedback loop usage on demand | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 0226c737 | 2026-01-27 | [d3d9] Reset texture projected bitmask | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 4bbe4879 | 2026-01-28 | [util] Fix crash on resolution change in Pirates | compatibility | later | D3D9 profile; not in the first batch |
| 00bec3e3 | 2026-01-30 | [d3d9] Remove unused function from D3D9Device | refactor | skip | Refactor or cleanup |
| e0424ebe | 2026-01-30 | [d3d9] Remove redundant null check in SetRenderTarget | perf | later | Perf; measure on Remix first |
| d222d2c3 | 2026-02-18 | [util] Disable direct image mapping for Total War Pharaoh Dynasties | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 84614029 | 2026-02-19 | [util] Disable implicit resolves for Total War Pharaoh | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 697ee071 | 2026-02-17 | [d3d9] Update framebuffer when clearing render targets | other | later | Not reviewed in detail |
| edd24ed7 | 2026-02-16 | [d3d8] Properly handle W11V11U10 support | irrelevant | skip | D3D8 frontend, not in this tree |
| b02cac89 | 2026-02-19 | [meta] FIFA regex adjustment | irrelevant | skip | Profile comment or layout change |
| 4357d638 | 2026-02-21 | [util] Enable zeroMappedMemory for Dawn of War DE | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| fae3c436 | 2026-02-22 | [meta] Expand the W40K DoW DE regex | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e86d70d3 | 2026-03-01 | [d3d9] Replace DxsoProgramType with new enum | other | later | Not reviewed in detail |
| 0b600963 | 2026-03-03 | [d3d9+dxso] Remove a bunch of shader options that are mostly unused | refactor | skip | Refactor or cleanup |
| d83d5585 | 2026-03-03 | [d3d9] Replace DxvkShaderKey with DxvkShaderHash | other | later | Not reviewed in detail |
| 6f3a758f | 2026-03-07 | [util] Enable zeroMappedMemory for Nioh 2 | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 3730b707 | 2026-03-12 | [d3d9] Update sampler state when binding a different texture | correctness | later | Sampler state update on texture change; sampler code differs |
| faa7d16e | 2026-03-25 | [d3d9,d3d11] Always query properties from device when possible | compatibility | later | Interop / shared resources; low priority for Remix |
| e981df13 | 2026-03-25 | [d3d9] Query adapter capabilities separately | other | later | Not reviewed in detail |
| 0a921071 | 2026-03-25 | [d3d9] Use limited adapter info for 9on12 device selection | compatibility | later | Interop / shared resources; low priority for Remix |
| b4e97574 | 2026-03-25 | [d3d9] Use limited adapter info to get texture memory | other | later | Not reviewed in detail |
| ee2cce21 | 2026-03-04 | [d3d9] Refactor shader creation | refactor | skip | Refactor or cleanup |
| ba45932d | 2026-04-13 | [d3d9] Remove dumping disassembled shader | refactor | skip | Removal tied to upstream refactors |
| 99e35a9e | 2026-02-23 | [d3d9] Create plain 2D views for blits if possible | other | later | Not reviewed in detail |
| b1b1d97b | 2026-04-17 | [d3d9] Fix weird 16-bit swapchain format mapping | correctness | later | Fix; needs a check against this tree |
| e19d221f | 2026-02-19 | [dxvk] Always enable shaderInt16 and shaderInt8 features | refactor | skip | DXVK backend change that follows the 2.x backend |
| dd4b4321 | 2026-04-22 | [dxvk] Work around depth-stencil copies involving weird formats (#5597) | refactor | skip | DXVK backend change that follows the 2.x backend |
| 817081a9 | 2026-04-25 | [d3d9] Add option to force draw time buffer uploads | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 3174b66e | 2026-04-25 | [util] Enable draw time buffer uploads for CSGO and Insurgency | compatibility | later | D3D9 profile; not in the first batch |
| d86ee541 | 2026-04-30 | [d3d9] Refactor calculating of binding slots. | refactor | skip | Refactor or cleanup |
| c2308b6b | 2026-05-04 | [d3d9] Disable sRGB support for D3DFMT_L8 | correctness | later | Fix; needs a check against this tree |
| 6e9b5047 | 2026-05-09 | [d3d9] Fix PSShared BufferType value | correctness | later | Fix; needs a check against this tree |
| 5d7cc493 | 2026-05-12 | [d3d9] Use roundEven in FF ubershader | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 7603f2e1 | 2026-05-17 | [d3d9] Clamp fog factor in FF shader | correctness | later | Fix; needs a check against this tree |
| 2bfdc805 | 2026-05-17 | [d3d9] Remove dead argument from fog calculation in VS FF | refactor | skip | Refactor or cleanup |
| e7ec5be5 | 2026-05-18 | [d3d9] Use roundEven for fixed function array access | correctness | later | Fix; needs a check against this tree |
| f2043678 | 2026-05-17 | [dxso] Add dxbc-spv dependency to fix build build | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 189f7c83 | 2026-05-19 | [d3d9] Write new shader analysis prepass | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| deb452eb | 2026-05-19 | [d3d9] Hook up new shader analysis pass | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 0a0d5b6a | 2026-05-19 | [d3d9] Build flat shading mask in new analysis pass | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 6dc5c62d | 2026-05-19 | [d3d9] Add spec const layout for the new compiler. | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 061202cf | 2026-05-19 | [d3d9] Hook up new compiler | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| cd08fb78 | 2026-05-21 | [d3d9] Add missing saturate to some FF texture stage ops in ubershader | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| bd3e0d65 | 2026-05-22 | [d3d9] Validate SM3 bytecode in shader analysis pass | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 64df9525 | 2026-05-27 | [d3d9] Enable dxbc-spirv by default | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 49d2921c | 2026-05-28 | [d3d9] Remove fixed-function ubershader options | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| cf983c1a | 2026-05-28 | [d3d9] Remove legacy fixed-function shader builder | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 94c313b5 | 2026-05-28 | [d3d9] Do not enable FP16 by default | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 3ff2a72e | 2026-05-29 | [d3d9] Simplify shader creation | refactor | skip | Refactor or cleanup |
| 61878771 | 2026-05-29 | [d3d9] Use dxbc-spirv functions to look up FF I/O locations | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 8560c8c6 | 2026-05-29 | [d3d9] Use dxbc-spirv functions to set up fixed-function flat shading | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| bedf3008 | 2026-05-29 | [d3d9] Use dxbc-spirv parser in shader validation | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 08b47b58 | 2026-05-29 | [d3d9] Add SWVP shader generator based on dxbc-spv IR | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| ba65c364 | 2026-05-29 | [d3d9] Wire up new SWVP shader generator | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| e0e4f02c | 2026-05-29 | [d3d9] Remove legacy SWVP generator | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 9a1b240d | 2026-05-29 | [d3d9] Simplify SWVP shader building | irrelevant | skip | Build or platform change |
| be4ac08f | 2026-05-30 | [d3d9] Adjust fixed function shaders HUD element | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| 63cbcad1 | 2026-06-01 | [d3d9] Simplify fixed-function texture sampling code | refactor | skip | Refactor or cleanup |
| 3c6f5083 | 2026-06-01 | [d3d9] Declare spec constant layout as constant in FF shaders | refactor | skip | DXVK 2.x binding / shader interface |
| 7b2c3c2d | 2026-06-01 | Add configuration for Fallout New Vegac PCR | irrelevant | skip | Profile comment or layout change |
| 02d97846 | 2026-06-02 | Clarify and correct Launcher.exe path in Fallout New Vegas PCR folder | irrelevant | skip | Profile comment or layout change |
| 48d852b0 | 2026-05-15 | [d3d9] Add a config option for cube texture depth format support | compatibility | later | New option |
| d7b1a29d | 2026-06-02 | [util] Extend Fallout NV regex to include the GOG release | compatibility | later | D3D9 profile; not in the first batch |
| c77876d9 | 2026-06-03 | [d3d9] Pass shader analysis to shader converter | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 80aa6963 | 2026-05-31 | [d3d9] Implement lowering for SM3 built-in inputs | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 6eddf20c | 2026-06-02 | [meta] Update dxbc-spirv | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 03e9438d | 2026-05-30 | [d3d9] Unify specular state tracking and use | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| d991995f | 2026-06-03 | [d3d9] Mark auxilary constant buffers as fully bound | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 0d72ff05 | 2026-05-29 | [d3d9] Always use dxbc-spirv for shader compilation | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| aeb0f217 | 2026-05-29 | [d3d9] Remove DXSO module | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 4d71e589 | 2026-05-29 | [d3d9] Remove remaining legacy fixed-function code | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| d5ee01ac | 2026-05-29 | [d3d9] Remove legacy spec constant manager | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| cd87a9e4 | 2026-05-29 | [spirv] Remove legacy SPIR_V module builder | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| bf583bc4 | 2026-06-05 | [d3d9] Fix bad move in shader creation | correctness | later | Fix; needs a check against this tree |
| 2cae043b | 2026-06-08 | [d3d9] Consolidate SWVP HUD | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| ca6bff65 | 2026-06-09 | [d3d9] Disallow view type mismatch for regular texture views | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| b683164c | 2026-06-09 | [d3d9] Remove sampler type mismatch tracking | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 2499af4f | 2026-06-04 | [d3d9] Remove non-robustness2 fallback | refactor | skip | DXVK backend change that follows the 2.x backend |
| 433d26b1 | 2026-06-05 | [d3d9] Remove non-GPL fallback code | refactor | skip | Needs graphics pipeline library (DXVK 2.x) |
| 0eb97074 | 2026-06-05 | [d3d9] Add helper to compute compacted shader constant layouts | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 50f1667c | 2026-06-03 | [d3d9] Set up constant layouts in shader analysis | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 6cf12981 | 2026-06-04 | [d3d9] Massively simplify resource binding assignment | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 6bdd4ad6 | 2026-06-04 | [d3d9] Create new constant buffers for shader constants | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 3e97bdf1 | 2026-06-04 | [d3d9] Always align constant buffer slices to full cache lines | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 4123e3e5 | 2026-06-04 | [d3d9] Add new path for constant buffer copies | refactor | skip | Constant buffer rework (DXVK 2.x) |
| c9edcc78 | 2026-06-05 | [d3d9] Implement shader lowering for new constant buffers | refactor | skip | Constant buffer rework (DXVK 2.x) |
| d9a0b13a | 2026-06-04 | [d3d9] Remove legacy constant buffer copies | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| d614d501 | 2026-06-05 | [d3d9] Simplify shader constant dirty tracking | refactor | skip | Refactor or cleanup |
| adc4cc30 | 2026-06-05 | [d3d9] Only dirty constants when constant values change | correctness | later | Fix; needs a check against this tree |
| 3eb73324 | 2026-06-05 | [d3d9] Also add hashing and equality to constant buffer layout | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 7b1e6615 | 2026-06-05 | [d3d9] Add global constant buffer layout table | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 5a78c391 | 2026-06-05 | [d3d9] Deduplicate constant buffer layouts globally | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 07d63812 | 2026-06-05 | [d3d9] Also store shader-defined constants in constant layout | refactor | skip | Constant buffer rework (DXVK 2.x) |
| bbb411ab | 2026-06-05 | [d3d9] Only dirty shader constants if the layout changes | correctness | later | Fix; needs a check against this tree |
| 6a361b2b | 2026-06-05 | [d3d9] Check correct SWVP state for shader analysis | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| a7faba98 | 2026-06-05 | [d3d9] Remove legacy constant buffers | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 654cf5d3 | 2026-06-05 | [d3d9] Eliminate shader-defined integer constants from layout | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| c7b0e7bc | 2026-06-07 | [d3d9] Add fast path for recursive device lock | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 0839f036 | 2026-06-07 | [d3d9] Explicitly call ExecuteFlush from locked contexts | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 516fe62e | 2026-06-07 | [d3d9] Rework misc. constant buffer slice allocation | refactor | skip | Constant buffer rework (DXVK 2.x) |
| beccc969 | 2026-06-07 | [d3d9] Rework constant buffer implementation | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 70b50682 | 2026-06-08 | [d3d9] Rework constant buffer creation | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 68799dbf | 2026-06-08 | [d3d9] Separate static and dynamic constant uploads | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| f5dd8c0f | 2026-06-08 | [d3d9] Implement DMA copies for constant buffers | refactor | skip | Constant buffer rework (DXVK 2.x) |
| d81bdc5c | 2026-06-08 | [d3d9] Enable constant buffer streaming on discrete GPUs | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 2d251a65 | 2026-06-08 | [d3d9] Repurpose d3d9.deviceLocalConstantBuffers option | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 2a5740b5 | 2026-06-09 | [d3d9] Clean up texture and sampler binding | refactor | skip | Refactor or cleanup |
| 1f12afca | 2026-06-09 | [d3d9] Include constant index in constant debug names | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| dfc46a3f | 2026-06-09 | [d3d9] Avoid redundantly setting input layout for instanced draws | perf | later | Perf; measure on Remix first |
| 3db36db2 | 2026-06-09 | [d3d9,dxgi] Add unified dxvk.maxFrameRate option | compatibility | later | New option |
| 6e164fc8 | 2026-06-09 | [d3d9] Ignore fixed function lights with invalid types | correctness | later | Needs the FF light count moved out of the VS key; check Remix light conversion too |
| 20274877 | 2026-06-09 | [d3d9] Avoid redundant binding setups for input layout updates | perf | later | Perf; measure on Remix first |
| 4cd6c761 | 2026-06-09 | [d3d9] Add more compact representation of input signatures | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 4776f101 | 2026-06-09 | [d3d9] Optimize input layout setup with constant-time attribute look-up | perf | later | Perf; measure on Remix first |
| 916dfb21 | 2026-06-10 | [d3d9] FF: Always use spec constants for texture stage data | refactor | skip | DXVK 2.x binding / shader interface |
| c4b96c80 | 2026-06-10 | [d3d9] Move textureFactor into push constants | refactor | skip | DXVK 2.x binding / shader interface |
| 56b54d8f | 2026-06-10 | [d3d9] Remove FF PS key | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 611e257a | 2026-06-11 | [d3d9] Fix dumb input layout bug with undefined vertex inputs | correctness | later | Fix; needs a check against this tree |
| dbbb1fc1 | 2026-06-11 | [d3d9] Slightly clean up FF VS key | refactor | skip | Refactor or cleanup |
| f9480f8d | 2026-06-11 | [d3d9] Get rid of cursed bitfield for VS key | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| e7387091 | 2026-06-11 | [d3d9] Get rid of last VS key remains | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 0cb22df7 | 2026-06-11 | [d3d9] Remove unused fogColor from FFVS | refactor | skip | Refactor or cleanup |
| 3ada7f1b | 2026-06-11 | [d3d9] Consolidate fog dirty flags | refactor | skip | Refactor or cleanup |
| d9d5344b | 2026-06-11 | [d3d9] Rework push data allocation and uploads | refactor | skip | DXVK 2.x binding / shader interface |
| 78337af1 | 2026-06-11 | [d3d9] Remove old render state structure | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 6370406d | 2026-06-10 | [dxvk] Reduce reserved and shared push data size | refactor | skip | DXVK 2.x binding / shader interface |
| 6520ac1c | 2026-06-11 | [d3d9] Take statically indexed constants into account for dynamic indexing | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| b21eafb4 | 2026-06-12 | [d3d9] Enable PUSH_ADDRESS path for dynamically indexed constants | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 377eaaf5 | 2026-06-12 | [d3d9] Ignore SWVP state in pixel shader analysis | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 25bd76db | 2026-06-12 | [d3d9] Always declare maximum shared push data block size for FF shaders | refactor | skip | DXVK 2.x binding / shader interface |
| c655a161 | 2026-06-12 | Revert "[d3d9] Declare spec constant layout as constant in FF shaders" | refactor | skip | Revert |
| 1a336f4a | 2026-06-13 | [d3d9] Fix incorrect constant count being passed to vertex shader | correctness | later | Fix; needs a check against this tree |
| 0e06635e | 2026-06-14 | [d3d9] Simplify vertex and index buffer binding | refactor | skip | Refactor or cleanup |
| ea987889 | 2026-06-14 | [d3d9] Only update buffer range when re-binding same vertex buffer | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 5ddd2c8c | 2026-06-13 | [d3d9] Use backend-provided spec data buffer for fixed-function shaders | correctness | later | Fix; needs a check against this tree |
| d7baf1b2 | 2026-06-13 | [d3d9] Remove explicit spec constant CBV from IR shaders | refactor | skip | DXVK 2.x binding / shader interface |
| c78e28d4 | 2026-06-13 | [d3d9] Remove spec constant buffer | refactor | skip | DXVK 2.x binding / shader interface |
| 3f50d38f | 2026-06-13 | [d3d9] Move vertex shader push data out of global push data block | refactor | skip | DXVK 2.x binding / shader interface |
| d4b5dff4 | 2026-06-16 | [d3d9] Introduce typed buffered command system like in D3D11 | compatibility | later | Interop / shared resources; low priority for Remix |
| 7fc488f9 | 2026-06-16 | [d3d9] Apply primitive type on app thread | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 152fd422 | 2026-06-16 | [d3d9] Always return instance count of 1 if no streams are indexed | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 4322877d | 2026-06-16 | [d3d9] Batch draw calls with no state changes in between | perf | later | Perf; measure on Remix first |
| 742bf7be | 2026-06-15 | [d3d9] Add new spec constant declarations | refactor | skip | DXVK 2.x binding / shader interface |
| b01107b5 | 2026-06-15 | [d3d9] Set up new spec constant state | refactor | skip | DXVK 2.x binding / shader interface |
| 09d00342 | 2026-06-15 | [d3d9] Port shaders to use new spec constant state | refactor | skip | DXVK 2.x binding / shader interface |
| c7d42a85 | 2026-06-15 | [d3d9] Treat unused samplers as null for spec constants | refactor | skip | DXVK 2.x binding / shader interface |
| 5f769e69 | 2026-06-15 | [d3d9] Fix default color1 output for fixed-function with SM3 PS | correctness | later | Fix; needs a check against this tree |
| 3593884f | 2026-06-15 | [d3d9] Remove d3d9_spec_constants.h | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 058fd7fe | 2026-06-16 | [d3d9] Move spec constant logic to the backend | refactor | skip | DXVK 2.x binding / shader interface |
| 20b6b8f2 | 2026-06-18 | [d3d9] Ensure consistent matrix processing between FFVS and programmable | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| f1fa1a53 | 2026-06-18 | [d3d8] Add a UAF texture guard config option | irrelevant | skip | Profile comment or layout change |
| ec040172 | 2026-06-18 | [d3d9] Add new state bit to enable Z fog (instead of W fog) | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 9adfbcad | 2026-06-18 | [d3d9] Properly implement Z and W fog in fixed-function shaders | correctness | later | Fix; needs a check against this tree |
| 299af25c | 2026-06-18 | [d3d9] Properly implement Z and W fog in programmable shaders | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| cd786f48 | 2026-06-19 | [d3d9] Make FFVS lighting code more readable | refactor | skip | Refactor or cleanup |
| 8486e99b | 2026-06-19 | [d3d9] Simplify and fix light enablement | refactor | skip | Refactor or cleanup |
| bb623cc9 | 2026-06-19 | [d3d9] Consolidate FFVS dirty flags | refactor | skip | Refactor or cleanup |
| ec440643 | 2026-06-19 | [util] Force sampler anisotropy for Mafia | compatibility | later | D3D9 profile; not in the first batch |
| d40adda3 | 2026-06-19 | [d3d9] Fix invalid reinterpret_cast on CS data block | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 8c9b4822 | 2026-06-21 | [d3d9] Also enable W fog if PositionT is used | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 55872e24 | 2026-06-20 | [d3d9] Slightly clean up clip plane code in FFVS | refactor | skip | Refactor or cleanup |
| 467840b1 | 2026-06-20 | [d3d9] Factor out texcoord transforms in FFVS | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 1cb2b9f4 | 2026-06-20 | [d3d9] Factor out lighting in FFVS | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 46ed65b0 | 2026-06-20 | [d3d9] Factor out vertex transform in FFVS | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 3f9640d7 | 2026-06-21 | [d3d9] Pass texture stage constant color as raw D3DCOLOR | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 2bd12fe4 | 2026-06-21 | [d3d9] Pass global ambient as raw D3DCOLOR | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 39e1d414 | 2026-06-21 | [d3d9] Implement D3DTOP_PREMODULATE in fixed-function PS | correctness | later | Fix; needs a check against this tree |
| f572c58d | 2026-06-21 | [d3d9] Add more exhaustive check for FFPS texture usage | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| a336e57b | 2026-06-21 | [d3d9] Track actively used textures for fixed-function PS. | correctness | later | Fix; needs a check against this tree |
| 016af1cc | 2026-06-22 | [d3d9] Normalize unused texture stage arguments | refactor | skip | Refactor or cleanup |
| a4653815 | 2026-06-22 | [d3d9] Discard fixed-function color output for depth-only rendering | correctness | later | Fix; needs a check against this tree |
| 8c3eaf98 | 2026-06-23 | [d3d9] Use correct type for constant buffer load in SWVP shaders | refactor | skip | Constant buffer rework (DXVK 2.x) |
| 6f279d4d | 2026-06-25 | [util] Enable Strict floatEmulation for Gothic 3 | compatibility | later | D3D9 profile; not in the first batch |
| e496eae8 | 2026-06-25 | [util] Hide amd in Splinter Cell Double Agent | compatibility | later | D3D9 profile; not in the first batch |
| 41939db8 | 2026-06-26 | [d3d9] Only validate color args if texture is unbound | correctness | later | API conformance (validation or return codes) |
| 0b49a398 | 2026-06-26 | [d3d9] Return null vector when sampling null texture in fixed-function | correctness | later | API conformance (validation or return codes) |
| a4f7d106 | 2026-06-27 | [d3d9] Fix compiler warning | irrelevant | skip | Build or platform change |
| 3d0ac2e7 | 2026-06-29 | [dxvk] Make (almost) all atomics sequentially consistent | refactor | skip | DXVK backend change that follows the 2.x backend |
| f2f7b508 | 2026-07-01 | [util] Add FPS limit for Manhunt | compatibility | later | D3D9 profile; uses a negative maxFrameRate (needs 9a8009cd) |
| aaf360b0 | 2026-07-02 | [d3d9] Do not eliminate texture stages with BUMPENV stages | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| cc13f9de | 2026-07-02 | Revert "[d3d9] Ignore buffer lock range for static D3DPOOL_DEFAULT buffers" | refactor | skip | Revert |
| 568b8836 | 2026-07-03 | [util] Remove floatEmulation config for Gothic 3 | compatibility | later | D3D9 profile; not in the first batch |
| aff1b8b8 | 2026-07-03 | [util] Update Gothic 3 comment after removal of floatEmulation config | irrelevant | skip | Profile comment or layout change |
| 82bf8b1e | 2026-07-02 | [d3d9] Do not transform normals if WorldView is not invertible | correctness | later | Non-invertible WorldView normals; FF code differs |
| 031a33b3 | 2026-07-05 | [d3d9] Properly propagate bump mapping textures to next stage | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| d31ba3e1 | 2026-07-04 | [d3d9] Use new unmapping code for textures | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 76801f2f | 2026-07-04 | [d3d9] Use new unmapping code for shaders | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| d117019f | 2026-07-04 | [d3d9] Remove old unmapping code | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 00023717 | 2026-07-06 | [d3d9] Add option to ignore buffer locking ranges | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| c2f9dfca | 2026-07-06 | [util] Enable ignoring default lock ranges for Splinter Cell Chaos Theory | compatibility | later | D3D9 profile; not in the first batch |
| b5bebe57 | 2026-07-09 | [util] Enable ignoring default lock ranges for Halo Trial and Halo Custom Edition | compatibility | later | D3D9 profile; not in the first batch |
| ef81c30f | 2026-07-21 | [d3d9] Fix linear fog edge case | correctness | later | Linear fog edge case; FF fog code differs |
| 836b6418 | 2026-07-21 | [d3d9] Clean up fixed-function fog calculation | refactor | skip | Refactor or cleanup |
| 6c7162fb | 2026-07-28 | [d3d8] Rework legacy D3D compatibility mode handling | irrelevant | skip | D3D8 frontend, not in this tree |
| 0f939f31 | 2026-08-01 | [d3d9] Rename FormatColor to Format | refactor | skip | Refactor or cleanup |
| 19dd2bb4 | 2026-08-01 | [d3d9] Use fixed-up format for fine-grained MSAA support check | correctness | later | Fix; needs a check against this tree |
| bee7816e | 2026-08-03 | [d3d9] Handle D3D9Ex checks with the D3D compatibility flags | correctness | later | Fix; needs a check against this tree |
| d2762330 | 2026-08-03 | [d3d9] Ignore zero extent stretch rect | correctness | later | Fix; needs a check against this tree |
| 6910e579 | 2026-08-08 | [d3d9] Add debug name for textures | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 3a4c6fa3 | 2026-08-08 | [d3d9] Don't test clip planes for pre-transformed vertices | correctness | later | Clip planes with pre-transformed vertices; FF code differs |
| b07a4783 | 2026-08-12 | [d3d9] Set non-empty sampler LOD range for single-mip textures | correctness | later | LOD range for single-mip textures; sampler code differs |
| 1db34b56 | 2026-08-20 | [util] Add missing includes | irrelevant | skip | Profile comment or layout change |
| cf824cc3 | 2026-08-24 | [dxvk,d3d11,d3d9] Support clearing buffer attachments | compatibility | later | Interop / shared resources; low priority for Remix |
| 2e65d208 | 2026-08-25 | [dxvk] Pass VkPresentRegionsKHR to the swapchain if supported | refactor | skip | DXVK backend change that follows the 2.x backend |
| d647fdf3 | 2026-08-26 | [d3d9] Pass validation for imported D3D11 textures. | compatibility | later | Interop / shared resources; low priority for Remix |
| 360bde44 | 2026-09-02 | [dxvk] Clear swap chain back buffer to app-provided value as necessary | refactor | skip | DXVK backend change that follows the 2.x backend |
| d7ac2580 | 2026-09-03 | [d3d9] Ensure to always submit if ThrottleAllocation hits threshold | perf | later | Perf; measure on Remix first |
| c833ea7e | 2026-09-08 | [d3d8] Extend legacy discard handling to handle legacy buffer mapping | irrelevant | skip | D3D8 frontend, not in this tree |
| 6bc6d9ec | 2026-09-11 | [d3d9] Update window handle during swapchain reset | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| e5bf902d | 2026-09-14 | [d3d9] Use new ScopedDeviceLock for device lock | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 878473ba | 2026-09-10 | [util] Enable cached dynamic resources for Skyrim SE | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 5cc97888 | 2026-09-15 | [d3d9] Correctly set up flat shading mask for SM 1/2 | correctness | later | Fix; needs a check against this tree |
| dc7f97df | 2026-09-24 | [d3d9] Return -1 from D3DPERF_BeginEvent/EndEvent when not annotating | correctness | later | API conformance (validation or return codes) |
| b419e808 | 2026-09-25 | [d3d9] Initialize the device child device pointer | refactor | skip | Refactor or cleanup |
| 5870d2e2 | 2026-09-25 | [meta] Update dxbc-spirv | refactor | skip | dxbc-spirv shader compiler rewrite (2026) |
| 271ad46d | 2026-09-29 | [d3d9] Clean up repeated 9Ex usage checks | refactor | skip | Refactor or cleanup |
| a25e33e9 | 2026-09-29 | [d3d9] Improve GetRenderTargetData validation | correctness | later | API conformance (validation or return codes) |
| e2c890ca | 2026-09-29 | [d3d9] Don't clear imported textures | correctness | later | Fix; needs a check against this tree |
| ec93112f | 2026-09-30 | [d3d9] Fix oversight with systemmem surfaces | correctness | later | Fix; needs a check against this tree |
| 68530156 | 2026-09-30 | [d3d9] Check the correct surface for multisampling in GetRTData | correctness | later | Fix; needs a check against this tree |
| 4bdfb9a1 | 2026-10-01 | [util] Enable more workarounds for Total War Pharaoh | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| 8d252af1 | 2026-10-05 | [d3d9] Preserve color inputs for D3DTOP_DOTPRODUCT3 | correctness | later | DOTPRODUCT3 color inputs; FF PS code differs |
| 1cad6cd1 | 2026-10-09 | [dxvk] Rework submission synchronization | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| 2c94c5f2 | 2026-08-21 | [util] Only spoof vendor id in New Vegas | compatibility | later | D3D9 profile; not in the first batch |
| b2c2622c | 2026-10-03 | [d3d9] Fix window handle device resets in fullscreen mode | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 3c9e4c3c | 2026-10-03 | [d3d8] Use switch statements in all format helpers | irrelevant | skip | D3D8 frontend, not in this tree |
| 17a85e6b | 2026-10-03 | [d3d9] Fix POSTBLENDSRGBCONVERT cap reporting and RS placement | correctness | later | Fix; needs a check against this tree |
| a57923a9 | 2026-10-03 | [d3d9] Leave device in a not reset state on validation failure | correctness | later | Window, mode or reset handling; Remix changed the swap chain |
| 098e41a3 | 2026-10-03 | [d3d9] Ignore SetLOD calls on non-MANAGED textures | correctness | port | Ported in 75f3972ea |
| 66f252f0 | 2026-10-03 | [d3d9] Report full constant set when in SWVP mode | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 6753e0c4 | 2026-10-01 | [d3d9] Slightly clean StretchRect | refactor | skip | Refactor or cleanup |
| 44d1e38d | 2026-10-01 | [d3d9] Remove automatic resolving in LockImage | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| d263e090 | 2026-10-01 | [d3d9] Stretch rect merge fast path EmitCs calls | perf | later | Perf; measure on Remix first |
| 2ebe9a6f | 2026-10-01 | [d3d9] Improve GetMipSize naming | refactor | skip | Refactor or cleanup |
| 9692a69e | 2026-10-01 | [d3d9] Move size padding to Getter | refactor | skip | Refactor or cleanup |
| 39e5367f | 2026-10-01 | [d3d9] Improve naming of late mapping buffer creation function | refactor | skip | Refactor or cleanup |
| 7bcd0231 | 2026-10-01 | [d3d9] Prevent creation of MANAGED surfaces | correctness | later | Fix; needs a check against this tree |
| 67fc6eb2 | 2026-10-02 | [d3d9] Correct typo | refactor | skip | Refactor or cleanup |
| 40b2ed20 | 2026-10-02 | [d3d9] Validate texture dimensions | correctness | later | API conformance (validation or return codes) |
| 89e9eca9 | 2026-10-02 | [d3d9] Align cubemap face mapping data to 16 bytes | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| f12a4348 | 2026-10-15 | [util] Enable scratch zero-initialization for Call of Duty Ghosts | irrelevant | skip | Profile change for D3D11/DXGI/other APIs only |
| e8c80a8d | 2026-10-15 | [d3d9] Don't enable point scaling with weird projection matrices | correctness | later | Point scaling with odd projections; FF code differs |
| 89b5490f | 2026-10-12 | [d3d9] Mark texture as uploaded in FlushImage | correctness | later | Upload/lock path; Remix reads mapped data, needs care |
| 6cd3cbc9 | 2026-10-11 | [d3d9] Add warning for stalls in UpdateTextureFromBuffer | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 3a71f3cc | 2026-10-13 | [d3d9] Switch texture map data to buffers early | refactor | skip | Part of the 2026 shader, constant and fixed-function rework on dxbc-spirv |
| 227b83b3 | 2026-10-01 | [d3d8] Reimplement CopyRects in the d3d8<->d3d9 bridge directly | irrelevant | skip | D3D8 frontend, not in this tree |
| 2f0a0f62 | 2026-10-17 | [d3d8] Clean up CopyRects related helpers | irrelevant | skip | D3D8 frontend, not in this tree |
| 7d248817 | 2026-10-18 | Revert "[d3d9] Return null vector when sampling null texture in fixed-function" | refactor | skip | Revert |
| 879fdc82 | 2026-10-20 | [d3d11,d3d9] Set infinite submission timeout for reproducibleCommandStreams | refactor | skip | Depends on DXVK 2.x resource, memory or sync model |
| ed73f1f2 | 2026-10-23 | [util] Remove textureMemory config from Dark Sector | compatibility | later | D3D9 profile; not in the first batch |
| 9c5551cb | 2026-10-27 | [d3d9] Fix integer divisions in YUV conversion | correctness | port | Ported in 3c6603a74 |
| 93f24465 | 2026-11-01 | [dxvk] Factor HUD instance out of swapchain blitter | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
| bcfab0c4 | 2026-11-01 | [hud] Explicitly pass presenter to HUD | refactor | skip | DXVK 2.x presenter / HUD; Remix has its own |
