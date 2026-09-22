#pragma once

// Fork-owned post-processing stack orchestration.
//
// The individual effect implementations remain in their existing classes for
// now. This module owns the stable effect IDs, ordering rules, configuration,
// and the single dispatch boundary used by the frame renderer.

#include <cstddef>
#include <string>
#include <vector>

#include "rtx_resources.h"

namespace dxvk {

  class DxvkContext;
  class RtxContext;

  class RtxPostProcessingStack {
  public:
    // updateAutoExposure is forwarded verbatim to
    // RtxContext::dispatchToneMapping. It has no default value on purpose:
    // DLSS-NR dispatches auto exposure itself, and letting dispatchToneMapping's
    // own `= true` default apply here would silently re-enable it.
    static void dispatch(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      bool performSRGBConversion,
      bool updateAutoExposure);

    static void showSettings(const Rc<DxvkContext>& ctx);

  private:
    enum class EffectId {
      Bloom,
      MotionBlur,
      DepthOfField,
      Tonemapping,
      NtscVhs,
      LensEffects,
      SRGBDither,
    };

    enum class EffectDomain {
      HDR,
      Display,
      Terminal,
    };

    struct EffectDescriptor {
      EffectId id;
      const char* name;
      const char* configId;
      EffectDomain domain;
      bool reorderable;
    };

    // This is a compile-time registry, not a public plugin ABI. A new effect
    // must provide a real dispatch adapter before it is added here; keeping
    // placeholder entries out of the table prevents a saved order from
    // silently claiming that an effect ran when it did not.

    static const EffectDescriptor& descriptor(EffectId id);
    static std::vector<EffectId> defaultOrder();
    static std::vector<EffectId> resolvedOrder();
    static size_t findEffect(const std::vector<EffectId>& order, EffectId id);
    static std::string serializeOrder(const std::vector<EffectId>& order);
    static bool moveEffect(std::vector<EffectId>& order, size_t from, size_t to);
    static bool canMove(const std::vector<EffectId>& order, size_t from, size_t to);
    static EffectId effectIdFromConfig(const std::string& configId, bool& valid);
    static std::string trim(const std::string& value);

    RTX_OPTION("rtx.postfx", std::string, stackOrder,
               std::string("bloom,motion_blur,depth_of_field,tonemapping,ntsc_vhs,lens_effects,srgb_dither"),
               "Comma-separated post-processing effect order. Effects may be reordered within their color domain; tonemapping and sRGB/dither remain fixed pipeline anchors.");
  };

} // namespace dxvk
