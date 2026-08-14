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
#include "dxvk_imgui_first_use_guide.h"

#include "imgui.h"
#include "../rtx_render/rtx_options.h"
#include "../rtx_render/rtx_utils.h"
#include "../../util/util_env.h"
#include "../../util/util_sentry.h"
#include "../../util/util_local_data.h"

namespace dxvk {

  static constexpr const char* kLicenseUrl = "https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/LICENSE-MIT";
  static constexpr const char* kPrivacyPolicyUrl = "https://www.nvidia.com/en-us/about-nvidia/privacy-policy/";
  static constexpr const char* kNeverShowFirstUseGuideKey = "neverShowFirstUseGuide";
  static constexpr const char* kallowUsageReportingKey = "allowUsageReporting";

  namespace {
    bool aeroButton(const char* label, const ImVec2& size) {
      ImGui::InvisibleButton(label, size);
      const bool hovered = ImGui::IsItemHovered();
      const bool held = ImGui::IsItemActive();
      const ImVec2 min = ImGui::GetItemRectMin();
      const ImVec2 max = ImGui::GetItemRectMax();
      ImDrawList* draw = ImGui::GetWindowDrawList();
      const ImU32 top = held ? IM_COL32(6, 94, 166, 255) : hovered ? IM_COL32(90, 205, 246, 255) : IM_COL32(75, 184, 231, 255);
      const ImU32 bottom = held ? IM_COL32(29, 145, 209, 255) : IM_COL32(8, 111, 184, 255);
      draw->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
      draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(max.x - 2.0f, min.y + (max.y - min.y) * 0.42f), IM_COL32(220, 250, 255, hovered ? 95 : 70), 6.0f);
      draw->AddRect(min, max, IM_COL32(15, 110, 174, 255), 6.0f, 0, 1.0f);
      const ImVec2 text = ImGui::CalcTextSize(label);
      draw->AddText(ImVec2(min.x + (max.x - min.x - text.x) * 0.5f, min.y + (max.y - min.y - text.y) * 0.5f), IM_COL32(245, 253, 255, 255), label);
      return ImGui::IsItemClicked();
    }

    void showPrivacyPolicyLink() {
      if (ImGui::SmallButton("NVIDIA Privacy Policy")) {
        env::openUrlInBrowser(kPrivacyPolicyUrl);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kPrivacyPolicyUrl);
      }
    }

    bool showPermissionsCheckboxes(bool& allowCrash, bool& allowUsage) {
      ImGui::TextWrapped("Help improve RTX Remix by allowing NVIDIA to collect diagnostic data:");
      ImGui::Spacing();

      bool changed = false;
      changed |= ImGui::Checkbox("Basic session and system information collected each time RTX Remix is launched", &allowUsage);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Includes game name, Remix version, GPU model and driver, OS version,\nRAM, and session configuration flags. Does not include user files, game\ncontent, or broad system state. NVIDIA will use this data to prioritize\ndevelopment work.");
      }
      changed |= ImGui::Checkbox("Technical crash information automatically submitted when RTX Remix crashes", &allowCrash);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Includes the same basic system information above plus crash type, error\nseverity, and GPU diagnostic state at the time of the crash, without identifying\nyou. NVIDIA will use this data to prioritize, investigate, and fix issues.");
      }

      ImGui::Spacing();
      ImGui::TextWrapped("Data collected will be used to improve RTX Remix, and is kept in compliance with NVIDIA's Privacy Policy:");

      showPrivacyPolicyLink();
      return changed;
    }
  }

  bool ImGuiFirstUseGuide::shouldShow() {
    LocalData::app().load();
    return !LocalData::app().get<bool>(kNeverShowFirstUseGuideKey, false)
           && !RtxOptions::Automation::disableBlockingDialogBoxes();
  }

  void ImGuiFirstUseGuide::showPermissionsUI() {
    bool allowCrash = LocalData::app().get<bool>(sentry::kAllowCrashReportingKey, true);
    bool allowUsage = LocalData::app().get<bool>(sentry::kAllowUsageReportingKey, true);

    if (showPermissionsCheckboxes(allowCrash, allowUsage)) {
      LocalData::app().set(sentry::kAllowCrashReportingKey, allowCrash);
      LocalData::app().set(sentry::kAllowUsageReportingKey, allowUsage);
      LocalData::app().save();
      sentry::syncConsentFromUserSettings();
    }
  }

  bool ImGuiFirstUseGuide::show(ImFont* boldFont) {
    bool wantClose = false;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 windowSize(520.0f, 0.0f);
    ImGui::SetNextWindowPos(
      ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.5f),
      ImGuiCond_Always,
      ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);

    constexpr ImGuiWindowFlags kWindowFlags =
      ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("Welcome to RTX Remix", nullptr, kWindowFlags)) {
      ImGui::End();
      return false;
    }

    ImGui::Spacing();

    {
      const auto keyBindDescriptor = buildKeyBindDescriptorStringForDisplay(RtxOptions::remixMenuKeyBinds());
      ImGui::TextWrapped(
        "RTX Remix is now active. To open or close the Remix menus press:");
      ImGui::Spacing();

      if (boldFont) {
        ImGui::PushFont(boldFont);
      }
      ImGui::Indent(20.0f);
      ImGui::Text("%s", keyBindDescriptor.c_str());
      ImGui::Unindent(20.0f);
      if (boldFont) {
        ImGui::PopFont();
      }
      ImGui::TextWrapped(
        "RTX Remix Menus can be navigated using mouse, keyboard, or gamepad. On keyboard, use the arrow keys to navigate and the spacebar to select.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("By using this software, you agree to RTX Remix's License:");
    ImGui::Spacing();

    if (ImGui::SmallButton("RTX Remix License")) {
      env::openUrlInBrowser(kLicenseUrl);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", kLicenseUrl);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    showPermissionsUI();
    ImGui::Spacing();
    ImGui::TextWrapped("These settings can also be changed at any time in the permissions tab of Remix's User Graphics Settings Menu.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
      const float buttonWidth = 180.0f;
      const float availableWidth = ImGui::GetContentRegionAvail().x;
      const float spacing = ImGui::GetStyle().ItemSpacing.x;
      const float totalButtonsWidth = buttonWidth * 2.0f + spacing;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - totalButtonsWidth) * 0.5f);

      if (aeroButton("Close", ImVec2(buttonWidth, ImGui::GetFrameHeight()))) {
        wantClose = true;
      }

      ImGui::SameLine();

      if (aeroButton("Never Show This Again", ImVec2(buttonWidth, ImGui::GetFrameHeight()))) {
        LocalData::app().set(kNeverShowFirstUseGuideKey, true);
        wantClose = true;
      }
    }

    if (wantClose) {
      // Persist whatever consent state is currently shown (including defaults if the user
      // did not interact with the checkboxes), so closing the dialog always leaves a
      // stored preference. showPermissionsUI already saves+syncs when checkboxes change,
      // so this is a no-op for values the user explicitly set this frame.
      bool allowCrash = LocalData::app().get<bool>(sentry::kAllowCrashReportingKey, true);
      bool allowUsage = LocalData::app().get<bool>(sentry::kAllowUsageReportingKey, true);
      LocalData::app().set(sentry::kAllowCrashReportingKey, allowCrash);
      LocalData::app().set(sentry::kAllowUsageReportingKey, allowUsage);
      LocalData::app().save();
      sentry::syncConsentFromUserSettings();
    }

    ImGui::Spacing();
    ImGui::End();

    return wantClose;
  }

}
