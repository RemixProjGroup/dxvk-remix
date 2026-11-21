#include "rtx_sky_ui.h"

#include <algorithm>
#include <cfloat>

#include "imgui/imgui.h"
#include "../../util/util_bit.h"

#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_numos_ui.h"

namespace dxvk {

  namespace {
    RemixGui::ComboWithKey<SkyMode> skyModeCombo {
      "Sky Mode",
      RemixGui::ComboWithKey<SkyMode>::ComboEntries { {
          {SkyMode::SkyboxRasterization, "Original game sky"},
          {SkyMode::Numos, "Numos atmosphere"}
      } }
    };

    auto skyAutoDetectCombo = RemixGui::ComboWithKey<SkyAutoDetectMode>(
      "Sky Auto-Detect",
      RemixGui::ComboWithKey<SkyAutoDetectMode>::ComboEntries{ {
        {SkyAutoDetectMode::None, "Off"},
        {SkyAutoDetectMode::CameraPosition, "By Camera Position"},
        {SkyAutoDetectMode::CameraPositionAndDepthFlags, "By Camera Position and Depth Flags"}
    } });

    void showSkyDetection() {
      if (ImGui::TreeNode("Sky detection")) {
        RemixGui::InputInt("First N Untextured Draw Calls", &RtxOptions::skyDrawcallIdThresholdObject(), 1, 1, 0);
        RemixGui::SliderFloat("Sky Min Z Threshold", &RtxOptions::skyMinZThresholdObject(), 0.0f, 1.0f);
        skyAutoDetectCombo.getKey(&RtxOptions::skyAutoDetectObject());
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Sky capture & reflections")) {
        RemixGui::Checkbox("Reproject Sky to Main Camera", &RtxOptions::skyReprojectToMainCameraSpaceObject());
        {
          ImGui::BeginDisabled(!RtxOptions::skyReprojectToMainCameraSpace());
          RemixGui::DragFloat("Reprojected Sky Scale", &RtxOptions::skyReprojectScaleObject(), 1.0f, 0.1f, 1000.0f);
          RemixGui::Checkbox("Force Auto-Detected Sky to Reproject", &RtxOptions::skyForceAutoDetectedToReprojectObject());
          ImGui::EndDisabled();
        }
        RemixGui::DragFloat("Sky Auto-Detect Unique Camera Search Distance", &RtxOptions::skyAutoDetectUniqueCameraDistanceObject(), 1.0f, 0.1f, 1000.0f);

        RemixGui::Checkbox("Force HDR sky", &RtxOptions::skyForceHDRObject());

        static const char* exts[] = { "256 (1.5MB vidmem)", "512 (6MB vidmem)", "1024 (24MB vidmem)",
          "2048 (96MB vidmem)", "4096 (384MB vidmem)", "8192 (1.5GB vidmem)" };

        int extIdx = std::clamp(bit::tzcnt(RtxOptions::skyProbeSide()), 8u, 13u) - 8;

        if (RemixGui::Combo("Sky Probe Extent", &extIdx, exts, IM_ARRAYSIZE(exts))) {
          RemixGui::CheckRtxOptionPopups(&RtxOptions::skyProbeSideObject());
          RtxOptions::skyProbeSideObject().setDeferred(1 << (extIdx + 8));
        }
        ImGui::TreePop();
      }
    }
  }

  void showSkyImguiSettings(DxvkContext* ctx) {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    const bool skyProvider = RtxOptions::skyMode() == SkyMode::Numos;

    ImGui::PushID("SkySettings");
    skyModeCombo.getKey(&RtxOptions::skyModeObject());
    RemixGui::SetTooltipToLastWidgetOnHover("Original game sky uses the game's skybox. Numos creates a sky with atmosphere, clouds, and weather.");

    if (skyProvider) {
      showNumosNotes(ctx);
    }
    if (ImGui::BeginTabBar("SkyPages", ImGuiTabBarFlags_FittingPolicyScroll)) {
      if (skyProvider) {
        showNumosTabs(ctx);
      } else if (ImGui::BeginTabItem("Sky")) {
        RemixGui::DragFloat("Sky Brightness", &RtxOptions::skyBrightnessObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Setup")) {
        if (skyProvider) {
          ImGui::TextWrapped("Set the game's orientation and scale once. Cloud height and size are under Clouds > Placement & scale.");
        }
        RemixGui::Checkbox("Z is up", &RtxOptions::zUpObject());
        RemixGui::SetTooltipToLastWidgetOnHover("Global game orientation. Enable for Z-up games; disable for Y-up games. This also affects other Remix systems.");
        if (skyProvider) {
          showNumosSetup(ctx);
        }
        ImGui::Separator();
        showSkyDetection();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::PopID();
  }
}
