# stb_image

`stb_image.h` v2.27, public domain / MIT (the dual licence notice is at the
bottom of the header itself). Taken verbatim from
`submodules/dlss/NVIDIAImageScaling/samples/third_party/stb/stb_image.h`, which
is the newer of the two copies that already sit in submodule sample trees;
neither of those is a build input, so this is the vendored copy the runtime
actually compiles.

It lives here rather than under `submodules/` because `./include` is already the
tree's home for header-only third party libraries (`gli`, `glm`, `MathLib`) and
is already on the dxvk include path, so `#include <stb/stb_image.h>` resolves
without a new `include_directories` entry that could drift.

Exactly one translation unit defines `STB_IMAGE_IMPLEMENTATION`:
`src/dxvk/rtx_render/rtx_external_effect_image.cpp`.
