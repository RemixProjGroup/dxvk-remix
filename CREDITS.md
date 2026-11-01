# Credits and third-party code

This runtime is a fork of NVIDIA RTX Remix (`dxvk-remix`), which is built on DXVK. The
code is under two licenses, and each file's header says which applies:

- **zlib/libpng** (`LICENSE`, Philip Rebohle and Joshua Ashton): DXVK's own code, such as
  the D3D11/DXGI frontends (no file in `src/d3d11` carries an NVIDIA header), most of
  `src/d3d9` and most of `src/dxvk` outside `rtx_render`.
- **MIT** (`LICENSE-MIT`, NVIDIA): RTX Remix's code, in files with NVIDIA's MIT header (most
  of `src/dxvk/rtx_render`, the Remix API, 8 of the `src/d3d9` files). Files NVIDIA changed
  from DXVK carry both: DXVK's original code under zlib, NVIDIA's additions under MIT. A few
  NVIDIA files have no header (`rtx_gui_widgets.cpp`, `rtx_overlay_window.cpp`,
  `rtx_staging.*`); they are MIT too.
- **This fork's own files** (no header, added by Kim2091: `rtx_fork_*`, `rtx_host_settings.*`,
  `rtx_perf_log.*`, `rtx_weather*`, `rtx_remix_api_device.h` and others): our own work. Where
  such a file holds code moved out of an NVIDIA file, that code stays MIT.
- Bundled libraries: `ThirdPartyLicenses.txt`. That includes the NVIDIA SHARC SDK, used
  unmodified from the `submodules/sharc` submodule and compiled into the runtime under the NVIDIA
  RTX SDKs license (`submodules/sharc/License.md`, reproduced in `ThirdPartyLicenses.txt`), which
  requires the notice "This software contains source code provided by NVIDIA Corporation."

What that means for releases:

- **Binary releases:** ship `LICENSE`, `LICENSE-MIT`, `ThirdPartyLicenses.txt` and this file.
  MIT requires its notice in every copy; zlib does not for binaries, but asks for an
  acknowledgement, which these files give.
- **Source releases:** keep every license header; zlib also requires altered files to be
  marked as changed and never presented as the original DXVK.
- Closed-source binary releases are allowed under both licenses.

## Code from other forks

### doodlum/dxvk-remix (branch `skyrim`)

- **Retained instances.** The retained-draw table and its replay (register a draw once,
  replay the whole table with one call per frame, scene-origin rebase) were ported from
  commit `5e39a218a` ("feat(d3d11): host the Remix API and retain the scene") in our commit
  `caaa21b47`. That commit is about 268 lines, but most of them are our own additions; about
  45-50 lines trace to doodlum's code. They are the retained table in the scene manager (its
  entry struct, the swap-remove, the per-frame replay copy and the origin rebase) and the
  instance-handle validation in the Remix API. Today that table serves only the instances
  that need per-frame work; the other retained instances use our own direct path. The
  ported lines sit in `rtx_scene_manager.cpp`, `rtx_scene_manager.h` and
  `rtx_remix_api.cpp`, which carry NVIDIA's MIT header, so they are used under the MIT
  terms of `LICENSE-MIT`: Copyright (c) 2026 doodlum. None of doodlum's zlib-licensed D3D11
  frontend code is used. Our additions on top (the C API in `remix_c.h`, batched
  transform/visibility updates, batched destroy, handle-based identity, the visibility flag,
  the preserve path, the direct path and later CPU work) are our own. The ported table is
  planned to be replaced by our own implementation; this entry stays until no ported line
  remains.
- **DLFG pacing findings.** An experimental Reflex marker probe (commits `8e8a2c3ba`,
  `3429bed1d`, since removed) was written to follow up on doodlum's frame-generation
  pacing measurements from 2026-09-21. No code was taken; the findings that motivated it
  were theirs.
- **Native D3D11 host (idea only).** Hosting Fallout 4 on DXVK's own D3D11 device, sharing
  that device with Remix, was inspired by doodlum's D3D11 API host on that branch. The
  implementation here is independent: a separate design (one module sharing the Vulkan
  device with Remix), and none of that branch's D3D11 host code is used. A line comparison on
  2026-10-24 found only single API calls that any such host makes (for example
  `createRtxContext()` for the immediate context) in common.

### sambow23/dxvk-remix-gmod (BetaRT, branch `betart-numos3`)

- **Host world origin.** `rtx_fork_camera_origin.{h,cpp}` (the `__mcrtx.world_origin.x/y/z` game
  values, their parsing and the previous-camera history offset) and the idea of adding the
  published origin back to the cloud anchor and shifting the previous camera before its
  update were ported from commits `11761453a` ("add png screenshot support"), `963c402f5`
  ("fix atmos cloud snapping at chunk borders") and `92ea99a7b` ("cloud/camera stability
  fixes") by CattaRappa (sambow23), about 60 lines. The
  BetaRT files carry no license header; that repository ships the same `LICENSE` (zlib) and
  `LICENSE-MIT` as this one, and the code is used under the MIT terms. Ours on top: the
  `rtx.worldOrigin.enable` gate, reading the origin with `SetupCamera` so it stays in order
  with the host's draws, and re-expressing every instance's transform history on an origin
  move (`SceneManager::applyWorldOrigin`).

### Mochabeanproto/dxvk-remix (branch `legacyrt`)

- **Underwater look.** The `rtx.underwater.*` options and the direct-lighting block that
  dims direct light, adds a two-octave procedural caustic pattern and applies depth murk on
  primary surfaces seen through a medium (`integrator_direct.slangh`, gated per surface by
  `SharedMediumMaterialIndex`), with its RaytraceArgs fields, constant fill and dev-menu
  controls, were ported from commit `ef6f42f` ("LegacyRT runtime changes on top of Remix
  Plus"), used with the author's permission. The files carry NVIDIA's MIT header, so it is
  used under the MIT terms of `LICENSE-MIT`: Copyright (c) 2026 Mochabeanproto. Our changes:
  a master toggle (`rtx.underwater.enable`, off = no cost), a host-set water surface height
  in place of the game's per-frame camera depth, and depth/caustic scale in meters from the
  game's units per metre. Nothing else from that commit is used.

## Code from other projects

### NUMOS

- **The sky, atmosphere, volumetric clouds and weather** come from NUMOS, a separate project by
  Kim2091 under the Apache License 2.0 (public repository: https://github.com/Kim2091/numos-sky).
  It is used as the `submodules/numos` submodule and keeps its own `LICENSE`, `NOTICE` and
  third-party notices. The runtime's adapter for it (`rtx_numos*`) is part of this repository.

### The tone curve, Prism and the halocedlaart Halo CE path-tracer addon

- **Tone curve and color grade** (`tonemap_operator.slangh`): by TheGreatHmmmmm, a
  per-channel smooth Reinhard shoulder in a desaturated working basis. MIT: Copyright (c) 2026
  TheGreatHmmmmm (the notice is in the file and in `ThirdPartyLicenses.txt`).
- **Prism**, the code that inspired the tone curve: by Jon (OopyDoopy / KickFister / Jon) for
  RenoDX, its shoulder curve by Musa Haji. The tone curve is close to Prism's code, so Prism's
  MIT notice (Copyright (c) 2026 Musa Haji, Copyright (c) 2026 OopyDoopy / KickFister / Jon)
  is in the file and in `ThirdPartyLicenses.txt` too.
- **Auto-exposure meter** (`auto_exposure_halo_meter.comp.slang`,
  `auto_exposure_halo_resolve.comp.slang`): by TheGreatHmmmmm, ported from their halocedlaart
  Halo CE path-tracer addon's `composite.hlsl`. MIT: Copyright (c) 2026 TheGreatHmmmmm. The addon
  builds on Prism, so Prism's notice is included too.

### HDR output

- **HDR10 output**: by TheGreatHmmmmm. MIT: Copyright (c) 2026 TheGreatHmmmmm (the notice is in
  `ThirdPartyLicenses.txt`).

### PQ ICC profile of HDR screenshots

- `rtx_screenshot_pq_icc.h`: a libjxl-generated PQ ICC profile (desc `RGB_D65_202_Rel_PeQ`, 4344 bytes), the one
  SKIV (SpecialKO/SKIV) and the NVIDIA app embed in their HDR PNGs. Its copyright tag states CC0; included unmodified.
