# Audit Re-verification Notes — Workstream 01

Performed as part of Task 1 against upstream `origin/main` HEAD `17d74001`.

## Commit 23fc439c — imgui remix api wrapper

**Original audit:** LANDED (evidence: `11bb1270 Implement DXVK backend for ImGui`)
**Re-verification result:** NOT-LANDED
**Evidence:**

`git log origin/main --oneline | grep -iE "remix.*imgui|imgui.*remix|remix api.*wrapper"`:
```
784cb8c9 [REMIX-2476] Fixed a crash in game on resolution change by cleaning up Imgui before recreation and suppressing an assert about window being subclassed on display mode change.
16e0c0f0 [REMIX-4507] Fix Imgui Theme RtxOptions to use change handlers and RtxOption widgets.
7287bb3e [REMIX-2245] Fix ImGui visual corruptions/crashes
```

`git show 23fc439c --stat | head -20`:
```
commit 23fc439cc527adab66eb6668b320d3c96857a1de
Author: CattaRappa <sambow23@gmail.com>
Date:   Tue Mar 31 18:17:58 2026 -0500

    add imgui remix api wrapper

 src/dxvk/imgui/dxvk_imgui.cpp          |   6 +
 src/dxvk/imgui/dxvk_imgui.h            |   3 +-
 src/dxvk/imgui/imgui_remix_exports.cpp | 408 +++++++++++++++++++++++++++++++++
 src/dxvk/imgui/imgui_remix_exports.h   | 159 +++++++++++++
 src/dxvk/meson.build                   |   2 +
 5 files changed, 577 insertions(+), 1 deletion(-)
```

The earlier audit conflated "DXVK ImGui backend" (upstream's windowing/input layer for ImGui) with "Remix API ImGui wrapper" (the fork's API surface for consumers to call ImGui from outside DXVK). These are different components. The upstream grep matches are unrelated ImGui bug fixes (crash on resolution change, theme RtxOption handlers, visual corruption fixes) — none add a Remix API wrapper for ImGui. The fork's wrapper files `imgui_remix_exports.{cpp,h}` are not in upstream.

## Commit 1843558f — dxvk_GetTextureHash

**Original audit:** LANDED
**Re-verification result:** NOT-LANDED
**Evidence:**

`git show origin/main:public/include/remix/remix_c.h | grep -iE "GetTextureHash|dxvk_GetTextureHash"`:
```
no match
```

Upstream's remix_c.h does not contain GetTextureHash or dxvk_GetTextureHash. Port required.

## Commit fa4fad87 — texture upload API

**Original audit:** LANDED
**Re-verification result:** NOT-LANDED
**Evidence:**

`git show origin/main:public/include/remix/remix_c.h | grep -iE "CreateTexture|DestroyTexture|TextureInfo|TextureHandle|remixapi_Format"`:
```
no match
```

Upstream's remix_c.h has none of the texture-upload API symbols. Port required.

## Summary

All three audit inconsistencies resolve to **NOT-LANDED**. All three commits must be fully ported in Sub-feature 1.
