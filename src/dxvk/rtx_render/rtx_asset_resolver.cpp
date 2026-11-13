/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx_asset_resolver.h"

#include "rtx_material_data.h"

namespace dxvk {

  // The default API material key is the material's runtime content hash: every texture and constant of the material
  // lists in rtx_material_data.h. Adding a field changes every API material key, so mods keyed on material hashes stop
  // matching. The lists are not visible here, so the guard is the size of each material struct: change it only together
  // with a bump of AssetResolver::kKeyDerivationVersion (and a note for mod authors), unless the field only enters
  // the hash with a non-default value, as the texture transform fields do (isHashedOnlyWhenSet).
  template<size_t Size, size_t Expected> struct CheckMaterialKeyFields {
    static_assert(Size == Expected, "a material struct changed: check whether its hashed fields (material keys) changed");
  };
  [[maybe_unused]] CheckMaterialKeyFields<sizeof(OpaqueMaterialData), 728> kOpaqueKeyFields;
  [[maybe_unused]] CheckMaterialKeyFields<sizeof(TranslucentMaterialData), 232> kTranslucentKeyFields;
  [[maybe_unused]] CheckMaterialKeyFields<sizeof(RayPortalMaterialData), 136> kPortalKeyFields;

} // namespace dxvk
