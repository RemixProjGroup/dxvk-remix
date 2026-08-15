#include <algorithm>
#include <array>
#include <sstream>

#include "dxvk_objects.h"
#include "rtx_fork_hooks.h"
#include "rtx_context.h"
#include "rtx_bloom.h"
#include "rtx_external_effects.h"
#include "rtx_postFx.h"
#include "rtx_tone_mapping.h"
#include "rtx_srgb_dither.h"
#include "rtx_imgui.h"
#include "rtx_fork_post_processing.h"

#include "../imgui/imgui.h"

namespace dxvk {

  namespace {
    constexpr size_t kInvalidIndex = static_cast<size_t>(-1);
    constexpr size_t kEffectCount = 7;

    bool inlineOptionCheckbox(const char* id, RtxOption<bool>* option) {
      bool value = option->get();
      const bool changed = ImGui::Checkbox(id, &value);

      if (changed) {
        RemixGui::CheckRtxOptionPopups(option);
        option->setDeferred(value);
      }

      if (ImGui::IsItemHovered()) {
        const std::string tooltip = RemixGui::BuildRtxOptionTooltip(option);
        RemixGui::SetTooltipUnformatted(tooltip.c_str());
      }

      return changed;
    }
  }

  const RtxPostProcessingStack::EffectDescriptor& RtxPostProcessingStack::descriptor(EffectId id) {
    static constexpr std::array<EffectDescriptor, kEffectCount> descriptors = {{
      { EffectId::Bloom,       "Bloom",          "bloom",        EffectDomain::HDR,      true  },
      { EffectId::MotionBlur,  "Motion Blur",    "motion_blur",  EffectDomain::HDR,      true  },
      { EffectId::DepthOfField, "Depth of Field", "depth_of_field", EffectDomain::HDR,    true  },
      { EffectId::Tonemapping, "Tonemapping",    "tonemapping",  EffectDomain::HDR,      false },
      { EffectId::NtscVhs,     "NTSC / VHS",     "ntsc_vhs",     EffectDomain::Display, true  },
      { EffectId::LensEffects, "Lens Effects",   "lens_effects", EffectDomain::Display,  true  },
      { EffectId::SRGBDither,  "sRGB + Dither",  "srgb_dither",  EffectDomain::Terminal, false },
    }};

    return descriptors[static_cast<size_t>(id)];
  }

  std::string RtxPostProcessingStack::configId(const EffectEntry& entry) {
    return entry.external
      ? "external:" + entry.externalId
      : descriptor(entry.builtInId).configId;
  }

  std::string RtxPostProcessingStack::name(const EffectEntry& entry) {
    if (!entry.external) {
      return descriptor(entry.builtInId).name;
    }

    const auto infos = RtxExternalEffects::instance().effectInfos();
    const auto info = std::find_if(infos.begin(), infos.end(), [&](const RtxExternalEffectInfo& candidate) {
      return candidate.id == entry.externalId;
    });
    return info == infos.end() ? entry.externalId : info->name;
  }

  RtxPostProcessingStack::EffectDomain RtxPostProcessingStack::domain(const EffectEntry& entry) {
    if (!entry.external) {
      return descriptor(entry.builtInId).domain;
    }

    const auto infos = RtxExternalEffects::instance().effectInfos();
    const auto info = std::find_if(infos.begin(), infos.end(), [&](const RtxExternalEffectInfo& candidate) {
      return candidate.id == entry.externalId;
    });
    return info != infos.end() && info->domain == RtxExternalEffectDomain::HDR
      ? EffectDomain::HDR
      : EffectDomain::Display;
  }

  bool RtxPostProcessingStack::reorderable(const EffectEntry& entry) {
    return entry.external || descriptor(entry.builtInId).reorderable;
  }

  std::vector<RtxPostProcessingStack::EffectEntry> RtxPostProcessingStack::defaultOrder() {
    return {
      { false, EffectId::Bloom, {} },
      { false, EffectId::MotionBlur, {} },
      { false, EffectId::DepthOfField, {} },
      { false, EffectId::Tonemapping, {} },
      { false, EffectId::NtscVhs, {} },
      { false, EffectId::LensEffects, {} },
      { false, EffectId::SRGBDither, {} },
    };
  }

  size_t RtxPostProcessingStack::findEffect(
    const std::vector<EffectEntry>& order,
    const std::string& effectConfigId) {
    const auto it = std::find_if(order.begin(), order.end(), [&](const EffectEntry& entry) {
      return configId(entry) == effectConfigId;
    });
    return it == order.end() ? kInvalidIndex : static_cast<size_t>(it - order.begin());
  }

  std::string RtxPostProcessingStack::trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return {};
    }

    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
  }

  RtxPostProcessingStack::EffectEntry RtxPostProcessingStack::effectFromConfig(
    const std::string& configId,
    bool& valid) {
    for (size_t i = 0; i < kEffectCount; i++) {
      const EffectId id = static_cast<EffectId>(i);
      if (configId == descriptor(id).configId) {
        valid = true;
        return { false, id, {} };
      }
    }

    constexpr const char* kExternalPrefix = "external:";
    if (configId.rfind(kExternalPrefix, 0) == 0) {
      const std::string id = configId.substr(std::char_traits<char>::length(kExternalPrefix));
      if (RtxExternalEffects::instance().hasEffect(id)) {
        valid = true;
        return { true, EffectId::Bloom, id };
      }
    }

    valid = false;
    return {};
  }

  std::vector<RtxPostProcessingStack::EffectEntry> RtxPostProcessingStack::resolvedOrder() {
    std::vector<EffectEntry> parsedOrder;
    std::stringstream stream(stackOrder());
    std::string token;

    while (std::getline(stream, token, ',')) {
      bool valid = false;
      const EffectEntry entry = effectFromConfig(trim(token), valid);
      if (valid && findEffect(parsedOrder, configId(entry)) == kInvalidIndex) {
        parsedOrder.push_back(entry);
      }
    }

    for (const EffectEntry& entry : defaultOrder()) {
      if (findEffect(parsedOrder, configId(entry)) == kInvalidIndex) {
        parsedOrder.push_back(entry);
      }
    }

    for (const RtxExternalEffectInfo& info : RtxExternalEffects::instance().effectInfos()) {
      const EffectEntry entry { true, EffectId::Bloom, info.id };
      if (findEffect(parsedOrder, configId(entry)) == kInvalidIndex) {
        parsedOrder.push_back(entry);
      }
    }

    std::vector<EffectEntry> result;
    result.reserve(parsedOrder.size());

    for (const EffectEntry& entry : parsedOrder) {
      if (domain(entry) == EffectDomain::HDR
       && (entry.external || entry.builtInId != EffectId::Tonemapping)) {
        result.push_back(entry);
      }
    }

    // Tonemapping is the fixed HDR-to-display boundary. It is never allowed
    // to move into either reorderable lane.
    result.push_back({ false, EffectId::Tonemapping, {} });

    for (const EffectEntry& entry : parsedOrder) {
      if (domain(entry) == EffectDomain::Display) {
        result.push_back(entry);
      }
    }

    result.push_back({ false, EffectId::SRGBDither, {} });
    return result;
  }

  std::string RtxPostProcessingStack::serializeOrder(const std::vector<EffectEntry>& order) {
    std::string result;
    for (const EffectEntry& entry : order) {
      if (!result.empty()) {
        result += ",";
      }
      result += configId(entry);
    }
    return result;
  }

  bool RtxPostProcessingStack::canMove(
    const std::vector<EffectEntry>& order,
    size_t from,
    size_t to) {
    if (from >= order.size() || to >= order.size() || from == to) {
      return false;
    }

    return reorderable(order[from])
        && reorderable(order[to])
        && domain(order[from]) == domain(order[to]);
  }

  bool RtxPostProcessingStack::moveEffect(
    std::vector<EffectEntry>& order,
    size_t from,
    size_t to) {
    if (!canMove(order, from, to)) {
      return false;
    }

    const EffectEntry entry = order[from];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, entry);
    return true;
  }

  void RtxPostProcessingStack::dispatch(
    Rc<RtxContext> ctx,
    Resources::RaytracingOutput& rtOutput,
    bool performSRGBConversion,
    bool updateAutoExposure) {
    ScopedCpuProfileZone();
    RtxExternalEffects::instance().ensureLoaded(ctx->getDevice().ptr());

    // The legacy post-FX option is now the stack's global optional-effect
    // switch. Tonemapping and the terminal sRGB/dither conversion remain
    // pipeline anchors because skipping either would change the output format.
    const bool optionalEffectsEnabled = ctx->getCommonObjects()->metaPostFx().enable();

    for (const EffectEntry& entry : resolvedOrder()) {
      if (entry.external) {
        if (optionalEffectsEnabled) {
          RtxExternalEffects::instance().dispatch(ctx, rtOutput, entry.externalId);
        }
        continue;
      }

      const EffectId id = entry.builtInId;
      if (!optionalEffectsEnabled
          && id != EffectId::Tonemapping
          && id != EffectId::SRGBDither) {
        continue;
      }

      switch (id) {
      case EffectId::Bloom:
        ctx->dispatchBloom(rtOutput);
        break;
      case EffectId::MotionBlur:
        ctx->dispatchPostFxMotionBlur(rtOutput);
        break;
      case EffectId::DepthOfField:
        ctx->dispatchPostFxDof(rtOutput);
        break;
      case EffectId::Tonemapping:
        // Explicitly forwarded, never defaulted: false here means DLSS-NR
        // already ran auto exposure for this frame.
        ctx->dispatchToneMapping(rtOutput, updateAutoExposure);
        break;
      case EffectId::NtscVhs:
        ctx->dispatchPostFxNtsc(rtOutput);
        break;
      case EffectId::LensEffects:
        ctx->dispatchPostFxLensEffects(rtOutput);
        break;
      case EffectId::SRGBDither:
        ctx->dispatchSRGBDither(rtOutput, performSRGBConversion);
        break;
      }
    }
  }

  void RtxPostProcessingStack::showSettings(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();
    auto& postFx = common->metaPostFx();
    auto& externalEffects = RtxExternalEffects::instance();
    externalEffects.ensureLoaded(ctx->getDevice().ptr());

    RemixGui::Checkbox("Post FX Enabled", &postFx.enableObject());
    if (ImGui::TreeNodeEx("External Effect Files", ImGuiTreeNodeFlags_DefaultOpen)) {
      externalEffects.showGlobalSettings(ctx->getDevice().ptr());
      ImGui::TreePop();
    }
    ImGui::TextDisabled("Drag the grip to reorder effects within the same color domain.");
    ImGui::Spacing();

    std::vector<EffectEntry> order = resolvedOrder();
    for (size_t i = 0; i < order.size(); i++) {
      const EffectEntry& entry = order[i];
      const EffectDomain effectDomain = domain(entry);
      const bool effectReorderable = reorderable(entry);
      const std::string effectConfigId = configId(entry);
      const std::string effectName = name(entry);
      const char* domainName = effectDomain == EffectDomain::HDR
        ? "HDR"
        : effectDomain == EffectDomain::Display
          ? "Display"
          : "Terminal";

      ImGui::PushID(effectConfigId.c_str());

      RtxOption<bool>* enabledOption = nullptr;
      RtxExternalEffectInfo externalInfo = {};
      if (entry.external) {
        const auto infos = externalEffects.effectInfos();
        const auto info = std::find_if(infos.begin(), infos.end(), [&](const RtxExternalEffectInfo& candidate) {
          return candidate.id == entry.externalId;
        });
        if (info != infos.end()) {
          externalInfo = *info;
        }
      } else {
        switch (entry.builtInId) {
        case EffectId::Bloom:
          enabledOption = &common->metaBloom().enableObject();
          break;
        case EffectId::MotionBlur:
          enabledOption = &postFx.enableMotionBlurObject();
          break;
        case EffectId::DepthOfField:
          enabledOption = &postFx.dofEnableObject();
          break;
        case EffectId::Tonemapping:
          enabledOption = &common->metaToneMapping().tonemappingEnabledObject();
          break;
        case EffectId::NtscVhs:
          enabledOption = &postFx.ntscEnableObject();
          break;
        case EffectId::LensEffects:
          enabledOption = &postFx.enableLensEffectsObject();
          break;
        case EffectId::SRGBDither:
          break;
        }
      }

      ImGui::BeginGroup();

      const ImVec2 dragHandleSize(42.0f, ImGui::GetFrameHeight());
      if (effectReorderable) {
        ImGui::Button("::##drag", dragHandleSize);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Drag to reorder %s effects", domainName);
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          ImGui::SetDragDropPayload(
            "RTX_POSTFX_EFFECT_ID", effectConfigId.c_str(), effectConfigId.size() + 1);
          ImGui::Text("Move %s", effectName.c_str());
          ImGui::EndDragDropSource();
        }
      } else {
        ImGui::BeginDisabled();
        ImGui::Button("LOCK##drag", dragHandleSize);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("Fixed pipeline anchor");
        }
      }

      ImGui::SameLine();
      if (entry.external) {
        bool enabled = externalInfo.enabled;
        if (ImGui::Checkbox("##enabled", &enabled)) {
          externalEffects.setEffectEnabled(entry.externalId, enabled);
        }
        if (!externalInfo.ready && ImGui::IsItemHovered()) {
          ImGui::SetTooltip("This effect has no loadable shader.");
        }
        ImGui::SameLine();
      } else if (enabledOption != nullptr) {
        inlineOptionCheckbox("##enabled", enabledOption);
        ImGui::SameLine();
      } else {
        bool required = true;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##required", &required);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("Required terminal conversion");
        }
        ImGui::SameLine();
      }

      const std::string headerLabel = effectName + "  [" + domainName + "]";
      const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_SpanAvailWidth
        | ImGuiTreeNodeFlags_FramePadding
        | ImGuiTreeNodeFlags_OpenOnArrow
        | ImGuiTreeNodeFlags_OpenOnDoubleClick;
      const bool showEffectSettings = ImGui::TreeNodeEx("##settings", headerFlags, "%s", headerLabel.c_str());

      ImGui::EndGroup();

      // The whole compact row is a generous drop target. Payloads identify an
      // effect, not a transient row index, and are applied only on delivery so
      // hovering cannot repeatedly reshuffle the stack.
      if (effectReorderable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RTX_POSTFX_EFFECT_ID")) {
          if (payload->IsDelivery() && payload->DataSize > 1
           && static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            const std::string sourceConfigId(static_cast<const char*>(payload->Data));
            const size_t sourceIndex = findEffect(order, sourceConfigId);
            if (sourceIndex != kInvalidIndex && moveEffect(order, sourceIndex, i)) {
              stackOrderObject().setDeferred(serializeOrder(order));
            }
          }
        }
        ImGui::EndDragDropTarget();
      }

      if (showEffectSettings) {
        ImGui::Indent();
        if (entry.external) {
          externalEffects.showEffectSettings(entry.externalId);
        } else {
          switch (entry.builtInId) {
          case EffectId::Bloom:
            common->metaBloom().showEffectSettings();
            break;
          case EffectId::MotionBlur:
            postFx.showMotionBlurImguiSettings();
            break;
          case EffectId::DepthOfField:
            postFx.showDofImguiSettings();
            break;
          case EffectId::Tonemapping:
            common->metaAutoExposure().showImguiSettings();
            // Carried over from the pre-stack Tonemapping header in
            // dxvk_imgui.cpp; these two fork options have no other UI surface.
            RemixGui::SliderInt("User Brightness", &RtxOptions::userBrightnessObject(), 0, 100, "%d");
            RemixGui::DragFloat("User Brightness EV Range", &RtxOptions::userBrightnessEVRangeObject(), 0.5f, 0.f, 10.f, "%.1f");
            RemixGui::Separator();
            common->metaToneMapping().showEffectSettings();
            break;
          case EffectId::NtscVhs:
            postFx.showNtscImguiSettings();
            break;
          case EffectId::LensEffects:
            postFx.showLensEffectsImguiSettings();
            break;
          case EffectId::SRGBDither:
            common->metaSRGBDither().showImguiSettings();
            break;
          }
        }
        ImGui::Unindent();
        ImGui::TreePop();
      }

      ImGui::PopID();
    }

    if (ImGui::Button("Reset to Default Order")) {
      stackOrderObject().setDeferred(serializeOrder(defaultOrder()));
    }

    ImGui::Separator();
  }

  namespace fork_hooks {

    void dispatchPostProcessingStack(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      bool performSRGBConversion,
      bool updateAutoExposure) {
      RtxPostProcessingStack::dispatch(
        ctx, rtOutput, performSRGBConversion, updateAutoExposure);
    }

    void showPostProcessingStackSettings(const Rc<DxvkContext>& ctx) {
      RtxPostProcessingStack::showSettings(ctx);
    }

  } // namespace fork_hooks

} // namespace dxvk
