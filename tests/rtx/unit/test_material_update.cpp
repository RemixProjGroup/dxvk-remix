// Checks how remixapi_UpdateMaterial classifies a new description of a live material
// (src/dxvk/rtx_render/rtx_material_update.h):
// - plain parameters and textures other than the albedo stay on the fast path;
// - alpha, blend, opacity, the albedo texture, the sampler, the sprite sheet, displacement, the subsurface profile and
//   the asset key rebuild the users;
// - a change of material kind is rejected, and an identical description is no change.

#include <cstdio>

#include "../../../src/dxvk/rtx_render/rtx_material_update.h"

namespace {
  using namespace dxvk;

  int g_failures = 0;
  constexpr uint64_t kApiVersion = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);

  void check(bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL: %s\n", what);
      ++g_failures;
    }
  }

  struct Opaque {
    remixapi_MaterialInfoOpaqueEXT ext {};
    remixapi_MaterialInfoOpaqueSubsurfaceEXT subsurface {};
    remixapi_AssetKeyEXT key {};
    remixapi_MaterialInfo info {};

    Opaque() {
      ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      subsurface.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
      key.sType = REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT;
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &ext;
      ext.roughnessConstant = 0.5f;
      ext.opacityConstant = 1.f;
      ext.alphaTestType = 7;
    }
  };

  template<typename Change>
  uint64_t rebuildStateAfter(Change change) {
    Opaque m;
    change(m);
    return apiMaterialRebuildState(m.info, kApiVersion);
  }

  void testParametersKeepTheRebuildState() {
    const uint64_t base = rebuildStateAfter([](Opaque&) {});
    check(rebuildStateAfter([](Opaque& m) { m.ext.roughnessConstant = 0.1f; }) == base, "roughness constant");
    check(rebuildStateAfter([](Opaque& m) { m.ext.metallicConstant = 0.9f; }) == base, "metallic constant");
    check(rebuildStateAfter([](Opaque& m) { m.ext.albedoConstant = { 1.f, 0.f, 0.f }; }) == base, "albedo constant");
    check(rebuildStateAfter([](Opaque& m) { m.info.emissiveIntensity = 4.f; }) == base, "emissive intensity");
    check(rebuildStateAfter([](Opaque& m) { m.info.emissiveColorConstant = { 1.f, 1.f, 0.f }; }) == base, "emissive color");
    check(rebuildStateAfter([](Opaque& m) { m.info.normalTexture = L"0x10"; }) == base, "normal texture");
    check(rebuildStateAfter([](Opaque& m) { m.info.emissiveTexture = L"0x11"; }) == base, "emissive texture");
    check(rebuildStateAfter([](Opaque& m) { m.ext.roughnessTexture = L"0x12"; }) == base, "roughness texture");
    check(rebuildStateAfter([](Opaque& m) { m.ext.metallicTexture = L"0x13"; }) == base, "metallic texture");
  }

  void testRebuildFields() {
    const uint64_t base = rebuildStateAfter([](Opaque&) {});
    check(rebuildStateAfter([](Opaque& m) { m.info.albedoTexture = L"0x20"; }) != base, "albedo texture");
    check(rebuildStateAfter([](Opaque& m) { m.ext.opacityConstant = 0.5f; }) != base, "opacity constant");
    check(rebuildStateAfter([](Opaque& m) { m.ext.alphaTestType = 4; }) != base, "alpha test type");
    check(rebuildStateAfter([](Opaque& m) { m.ext.alphaReferenceValue = 128; }) != base, "alpha reference");
    check(rebuildStateAfter([](Opaque& m) { m.ext.useDrawCallAlphaState = 1; }) != base, "draw call alpha state");
    check(rebuildStateAfter([](Opaque& m) { m.ext.blendType_hasvalue = 1; }) != base, "blend enabled");
    check(rebuildStateAfter([](Opaque& m) { m.ext.invertedBlend = 1; }) != base, "inverted blend");
    check(rebuildStateAfter([](Opaque& m) { m.ext.alphaIsThinFilmThickness = 1; }) != base, "alpha as thin film thickness");
    check(rebuildStateAfter([](Opaque& m) { m.ext.displaceIn = 0.1f; }) != base, "displacement");
    check(rebuildStateAfter([](Opaque& m) { m.ext.displaceOut = 0.1f; }) != base, "outward displacement");
    check(rebuildStateAfter([](Opaque& m) { m.ext.heightTexture = L"0x21"; }) != base, "height texture");
    check(rebuildStateAfter([](Opaque& m) { m.info.spriteSheetFps = 8; }) != base, "sprite sheet");
    check(rebuildStateAfter([](Opaque& m) { m.info.filterMode = 1; }) != base, "filter mode");
    check(rebuildStateAfter([](Opaque& m) { m.info.wrapModeU = 1; }) != base, "wrap mode");
    check(rebuildStateAfter([](Opaque& m) {
      m.subsurface.subsurfaceDiffusionProfile = 1;
      m.ext.pNext = &m.subsurface;
    }) != base, "subsurface diffusion profile");
    check(rebuildStateAfter([](Opaque& m) {
      m.key.key = 77;
      m.ext.pNext = &m.key;
    }) != base, "asset key");
  }

  void testClassification() {
    Opaque m;
    ApiMaterialSummary live { 1, apiMaterialRebuildState(m.info, kApiVersion), apiMaterialKind(m.info) };
    check(live.kind == ApiMaterialKind::Opaque, "an opaque chain is opaque");

    ApiMaterialSummary next = live;
    check(classifyApiMaterialChange(live, next) == ApiMaterialChange::None, "the same description is no change");
    next.description = 2;
    check(classifyApiMaterialChange(live, next) == ApiMaterialChange::Parameters, "another description with the same rebuild state");
    next.rebuildState = live.rebuildState + 1;
    check(classifyApiMaterialChange(live, next) == ApiMaterialChange::RebuildUsers, "another rebuild state");

    remixapi_MaterialInfoTranslucentEXT translucentExt {};
    translucentExt.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    remixapi_MaterialInfo translucent {};
    translucent.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    translucent.pNext = &translucentExt;
    check(apiMaterialKind(translucent) == ApiMaterialKind::Translucent, "a translucent chain is translucent");
    next = { 3, apiMaterialRebuildState(translucent, kApiVersion), apiMaterialKind(translucent) };
    check(classifyApiMaterialChange(live, next) == ApiMaterialChange::KindChange, "opaque to translucent");

    remixapi_MaterialInfoPortalEXT portalExt {};
    portalExt.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT;
    remixapi_MaterialInfo portal {};
    portal.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    portal.pNext = &portalExt;
    check(apiMaterialKind(portal) == ApiMaterialKind::Portal, "a portal chain is a portal");
    next.kind = ApiMaterialKind::Portal;
    check(classifyApiMaterialChange(live, next) == ApiMaterialChange::KindChange, "opaque to portal");
    // The kind is checked first, so a kind change is rejected even when the rest looks identical.
    next = live;
    next.kind = ApiMaterialKind::Translucent;
    check(classifyApiMaterialChange(live, next) == ApiMaterialChange::KindChange, "only the kind differs");

    remixapi_MaterialInfo none {};
    none.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    check(apiMaterialKind(none) == ApiMaterialKind::Unknown, "no chain is unknown");
  }

  // A host older than 0.4.2 has no displaceOut field, so it is not read.
  void testDisplaceOutNeedsVersion() {
    Opaque m;
    constexpr uint64_t kOld = REMIXAPI_VERSION_MAKE(0, 4, 1);
    const uint64_t base = apiMaterialRebuildState(m.info, kOld);
    m.ext.displaceOut = 0.25f;
    check(apiMaterialRebuildState(m.info, kOld) == base, "displaceOut of a 0.4.1 host");
    check(apiMaterialRebuildState(m.info, REMIXAPI_VERSION_MAKE(0, 4, 2)) != apiMaterialRebuildState(m.info, kOld), "displaceOut of a 0.4.2 host");
  }

  void testPortalMask() {
    remixapi_MaterialInfoPortalEXT portalExt {};
    portalExt.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT;
    remixapi_MaterialInfo portal {};
    portal.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    portal.pNext = &portalExt;
    const uint64_t base = apiMaterialRebuildState(portal, kApiVersion);
    portal.emissiveTexture = L"0x30";
    check(apiMaterialRebuildState(portal, kApiVersion) != base, "a portal's mask texture");
    portal.emissiveTexture = nullptr;
    portalExt.rotationSpeed = 3.f;
    check(apiMaterialRebuildState(portal, kApiVersion) == base, "a portal's rotation speed");
  }
}

int main() {
  testParametersKeepTheRebuildState();
  testRebuildFields();
  testClassification();
  testDisplaceOutNeedsVersion();
  testPortalMask();
  if (g_failures != 0) {
    std::printf("%d failures\n", g_failures);
    return -1;
  }
  std::printf("test_material_update: all passed\n");
  return 0;
}
