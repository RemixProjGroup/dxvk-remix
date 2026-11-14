/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

#include "dxvk_imgui.h"
#include "imgui.h"
#include "rtx_fault_notice.h"
#include "rtx_render/rtx_imgui.h"
#include "dxvk_device.h"
#include "rtx_render/rtx_context.h"
#include "rtx_render/rtx_options.h"
#include "rtx_render/rtx_dlss.h"
#include "rtx_render/rtx_neural_uplift.h"
#include "rtx_render/rtx_dlfg.h"
#include "rtx_render/rtx_reflex.h"
#include "rtx_render/rtx_ray_reconstruction.h"
#include "rtx_render/rtx_xess.h"
#include "rtx_render/rtx_fork_hooks.h"
#include "rtx_render/rtx_host_settings.h"
#include "rtx_render/rtx_postFx.h"
#include "rtx_render/rtx_tone_mapping.h"
#include "rtx_render/rtx_rtxdi_rayquery.h"
#include "rtx_render/rtx_restir_gi_rayquery.h"
#include "rtx_render/rtx_neural_radiance_cache.h"
#include "rtx_render/rtx_numos_ui.h"
#include "rtx_render/rtx_option_layer_gui.h"
#include "rtx_render/rtx_scene_manager.h"
#include "rtx_render/rtx_screenshot.h"
#include "rtx_render/rtx_color_grade.h"
#include "../util/util_string.h"

namespace dxvk {

  // Combo boxes shared with dxvk_imgui.cpp
  extern RemixGui::ComboWithKey<DLSSProfile> dlssProfileCombo;
  extern RemixGui::ComboWithKey<DxvkDLSS::DLSSPreset> dlssRenderPresetCombo;
  extern RemixGui::ComboWithKey<XeSSPreset> xessPresetCombo;

  // Combo boxes used only by the user menu
  static RemixGui::ComboWithKey<DlssPreset> dlssPresetCombo{
    "DLSS Preset",
    RemixGui::ComboWithKey<DlssPreset>::ComboEntries{ {
        {DlssPreset::Off, "Disabled"},
        {DlssPreset::On, "Enabled"},
        {DlssPreset::Custom, "Custom"},
    } }
  };

  static RemixGui::ComboWithKey<NisPreset> nisPresetCombo{
    "NIS Preset",
    RemixGui::ComboWithKey<NisPreset>::ComboEntries{ {
        {NisPreset::Performance, "Performance"},
        {NisPreset::Balanced, "Balanced"},
        {NisPreset::Quality, "Quality"},
        {NisPreset::Fullscreen, "Fullscreen"},
    } }
  };

  static RemixGui::ComboWithKey<TaauPreset> taauPresetCombo{
    "TAA-U Preset",
    RemixGui::ComboWithKey<TaauPreset>::ComboEntries{ {
        {TaauPreset::UltraPerformance, "Ultra Performance"},
        {TaauPreset::Performance, "Performance"},
        {TaauPreset::Balanced, "Balanced"},
        {TaauPreset::Quality, "Quality"},
        {TaauPreset::Fullscreen, "Fullscreen"},
    } }
  };

  static RemixGui::ComboWithKey<GraphicsPreset> graphicsPresetCombo{
    "Graphics Preset",
    RemixGui::ComboWithKey<GraphicsPreset>::ComboEntries{ {
        {GraphicsPreset::Ultra, "Ultra"},
        {GraphicsPreset::High, "High"},
        {GraphicsPreset::Medium, "Medium"},
        {GraphicsPreset::Low, "Low"},
        {GraphicsPreset::Custom, "Custom"},
    } }
  };

  static RemixGui::ComboWithKey<int> minPathBouncesCombo {
    "Min Light Bounces",
    RemixGui::ComboWithKey<int>::ComboEntries { {
        {0, "0"},
        {1, "1"},
    } }
  };

  static RemixGui::ComboWithKey<int> maxPathBouncesCombo {
    "Max Light Bounces",
    RemixGui::ComboWithKey<int>::ComboEntries { {
        {1, "1"},
        {2, "2"},
        {3, "3"},
        {4, "4"},
        {5, "5"},
        {6, "6"},
        {7, "7"},
        {8, "8"},
    } }
  };

  static RemixGui::ComboWithKey<int> indirectLightingParticlesCombo {
    "Particle Light",
    RemixGui::ComboWithKey<int>::ComboEntries { {
        {0, "None"},
        {1, "Low"},
        {2, "High"},
    } }
  };

  static RemixGui::ComboWithKey<NeuralRadianceCache::QualityPreset> neuralRadianceCacheQualityPresetCombo {
    "RTX Neural Radiance Cache Quality",
    RemixGui::ComboWithKey<NeuralRadianceCache::QualityPreset>::ComboEntries { {
        {NeuralRadianceCache::QualityPreset::Ultra, "Ultra"},
        {NeuralRadianceCache::QualityPreset::High, "High"},
        {NeuralRadianceCache::QualityPreset::Medium, "Medium"}
    } }
  };

  static RemixGui::ComboWithKey<bool> denoiserQualityCombo {
    "NRD Denoising Quality",
    RemixGui::ComboWithKey<bool>::ComboEntries { {
        {true, "High"},
        {false,"Low"},
    } }
  };

  namespace {
    // The color of section titles and of the frame rate limit's status: the theme's accent.
    ImVec4 accentColor() {
      return ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    }

    // A tab splits its sections into two columns only from this width, in font sizes, so each column keeps
    // room for its labels and values.
    constexpr float kTwoColumnMinWidthInFonts = 64.0f;

    // The vertical padding of the menu's buttons, in font sizes.
    constexpr float kButtonPaddingY = 0.35f;

    // Pushes the padded, rounded frame that all the menu's buttons share, so they have one height.
    void pushButtonStyle() {
      const float fontSize = ImGui::GetFontSize();
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, fontSize * 0.3f);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fontSize * 0.9f, fontSize * kButtonPaddingY));
    }

    void popButtonStyle() {
      ImGui::PopStyleVar(2);
    }

    float buttonHeight() {
      return ImGui::GetFontSize() * (1.0f + 2.0f * kButtonPaddingY);
    }

    // Edits rtx.frameRateLimit with a number box and a Set button, and states the limit in force beside them.
    // The box keeps the typed value until Set is pressed, so typing never changes the limit.
    // The box starts controlOffsetX from the start of the line.
    void showFrameRateLimitControl(float controlOffsetX) {
      static int pendingLimit = -1;
      const uint32_t limit = RtxReflex::frameRateLimit();
      if (pendingLimit < 0) {
        pendingLimit = static_cast<int>(limit);
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Frame Rate Limit");
      ImGui::SameLine(controlOffsetX);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
      ImGui::InputInt("##frameRateLimit", &pendingLimit, 0, 0);
      pendingLimit = std::clamp(pendingLimit, 0, 1000);
      RemixGui::SetTooltipToLastWidgetOnHover("The highest frame rate, in frames per second. 0 turns the limit off.");

      ImGui::SameLine();
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() * 0.3f);
      if (ImGui::Button("Set##frameRateLimit", ImVec2(ImGui::GetFontSize() * 3.0f, 0.0f))) {
        RtxReflex::frameRateLimitObject().setDeferred(static_cast<uint32_t>(pendingLimit));
      }
      ImGui::PopStyleVar();

      ImGui::SameLine();
      if (limit > 0) {
        ImGui::TextColored(accentColor(), "Limited to %u fps", limit);
      } else {
        ImGui::TextDisabled("Not limited");
      }
    }

    // Draws a section title in the accent color on a line that runs to the right edge of the column.
    void sectionHeader(const char* text) {
      ImGui::Spacing();
      const ImVec2 start = ImGui::GetCursorScreenPos();
      const float rightX = start.x + ImGui::GetContentRegionAvail().x;
      ImGui::SetCursorScreenPos(ImVec2(start.x + ImGui::GetFontSize() * 0.5f, start.y));
      ImGui::TextColored(accentColor(), "%s", text);

      const ImVec2 textMin = ImGui::GetItemRectMin();
      const ImVec2 textMax = ImGui::GetItemRectMax();
      const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
      const float lineY = std::floor((textMin.y + textMax.y) * 0.5f);
      const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_Border);
      ImDrawList* drawList = ImGui::GetWindowDrawList();
      drawList->AddLine(ImVec2(start.x, lineY), ImVec2(textMin.x - gap, lineY), lineColor);
      if (textMax.x + gap < rightX) {
        drawList->AddLine(ImVec2(textMax.x + gap, lineY), ImVec2(rightX, lineY), lineColor);
      }
    }

    // Lays a tab's sections out in two columns when the tab is wide enough, and in one column otherwise.
    // All labeled controls in a column get the same label width, so the controls line up.
    class SectionColumns {
    public:
      SectionColumns(const char* id, bool allowSplit) {
        const float fontSize = ImGui::GetFontSize();
        if (allowSplit && ImGui::GetContentRegionAvail().x >= fontSize * kTwoColumnMinWidthInFonts) {
          ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(fontSize * 0.75f, 0.0f));
          m_split = ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV |
                                             ImGuiTableFlags_NoSavedSettings);
          ImGui::PopStyleVar();
          if (m_split) {
            ImGui::TableNextColumn();
          }
        }
        beginColumn();
      }

      ~SectionColumns() {
        endColumn();
        if (m_split) {
          ImGui::EndTable();
        }
      }

      SectionColumns(const SectionColumns&) = delete;
      SectionColumns& operator=(const SectionColumns&) = delete;

      // Moves to the second column.
      // Does nothing in the one-column layout, so the sections continue below.
      void nextColumn() {
        if (m_split) {
          endColumn();
          ImGui::TableNextColumn();
          beginColumn();
        }
      }

      // Indents the rows that refine the row above them, and keeps their controls in line with the others.
      void beginSubItems() {
        ImGui::Indent(ImGui::GetFontSize());
        RemixGui::PushLabelColumnFixedWidth(m_labelWidth - ImGui::GetFontSize());
      }

      void endSubItems() {
        RemixGui::PopLabelColumnFixedWidth();
        ImGui::Unindent(ImGui::GetFontSize());
      }

    private:
      void beginColumn() {
        const float fontSize = ImGui::GetFontSize();
        const float width = ImGui::GetContentRegionAvail().x;
        m_labelWidth = std::floor(std::min(std::clamp(width * 0.42f, fontSize * 8.0f, fontSize * 17.0f), width * 0.6f));
        RemixGui::PushLabelColumnFixedWidth(m_labelWidth);

        // Plain ImGui controls put their label after the control, so they get the width of the control column.
        ImGui::PushItemWidth(std::max(width - m_labelWidth - fontSize * 2.5f, fontSize * 4.0f));
      }

      void endColumn() {
        ImGui::PopItemWidth();
        RemixGui::PopLabelColumnFixedWidth();
      }

      bool m_split = false;
      float m_labelWidth = 0.0f;
    };
  } // namespace

  // Helper functions defined in dxvk_imgui.cpp
  RemixGui::ComboWithKey<UpscalerType>& getUpscalerCombo(DxvkDLSS& dlss, DxvkRayReconstruction& rayReconstruction);

  ImFont* ImGUI::getUserMenuFont(float targetSize) const {
    ImFont* closest = nullptr;
    for (ImFont* font : m_userMenuFonts) {
      // Ties go to the larger font, since the sizes are in ascending order.
      if (font != nullptr && (closest == nullptr || std::abs(font->FontSize - targetSize) <= std::abs(closest->FontSize - targetSize))) {
        closest = font;
      }
    }
    return closest;
  }

  void ImGUI::showUserMenu(const Rc<DxvkContext>& ctx) {
    // Target user.conf layer for user menu changes
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // The menu is laid out for 1080p and grows with the output height. It draws in the atlas font closest to
    // the scaled size rather than scaling a font's glyphs, so the text stays sharp, and every other size
    // follows the font that was picked.
    const float outputScale = std::max(1.0f, viewport->Size.y / 1080.0f);
    const float baseFontSize = largeUiMode() ? 24.0f : 18.0f;
    ImFont* menuFont = getUserMenuFont(baseFontSize * outputScale);
    const float uiScale = (menuFont != nullptr ? menuFont->FontSize : ImGui::GetFontSize()) / 16.0f;
    if (menuFont != nullptr) {
      ImGui::PushFont(menuFont);
    }

    const ImGuiStyle& baseStyle = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(baseStyle.FramePadding.x * uiScale, baseStyle.FramePadding.y * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(baseStyle.ItemSpacing.x * uiScale, baseStyle.ItemSpacing.y * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(baseStyle.ItemInnerSpacing.x * uiScale, baseStyle.ItemInnerSpacing.y * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, baseStyle.ScrollbarSize * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, baseStyle.GrabMinSize * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, baseStyle.FrameRounding * uiScale);
    constexpr int kScaledStyleVarCount = 6;

    const float fontSize = ImGui::GetFontSize();

    // The ID after "###" stays fixed, so the title can show the product name and version.
    const std::string windowTitle = str::format(productNameWithVersion(), " Graphics Settings###", m_userGraphicsWindowTitle);
    ImGui::OpenPopup(windowTitle.c_str(), ImGuiPopupFlags_NoOpenOverExistingPopup);

    // The window opens centered at about half the screen's width and 70% of its height, and can be moved
    // and resized while it is open.
    const float windowWidth = std::min(viewport->Size.x * 0.95f,
                                       std::max(std::min(viewport->Size.x * 0.5f, viewport->Size.y * 0.85f), 600.0f * uiScale));
    const float windowHeight = viewport->Size.y * 0.7f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f * uiScale, 300.0f * uiScale), viewport->Size);

    // The padding follows the font, so the layout keeps its proportions at every output size.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fontSize * 0.75f, fontSize * 0.5f));

    // The window uses the background color of the other menus, but nearly opaque, so the text keeps its contrast
    // over a bright scene.
    ImVec4 backgroundColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    backgroundColor.w = std::max(backgroundColor.w, 0.92f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, backgroundColor);

    bool basicMenuOpen = RtxOptions::showUI() == UIType::Basic;
    const bool menuVisible = ImGui::BeginPopupModal(windowTitle.c_str(), &basicMenuOpen);

    // The window has taken its padding and background, so combo boxes and tooltips inside it use the theme's own.
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (menuVisible) {
      showUserMenuHeader();
      ImGui::Spacing();

      // Each tab's page fills the height that the footer buttons leave free.
      const float footerHeight = buttonHeight() + ImGui::GetStyle().ItemSpacing.y;
      auto beginTabPage = [footerHeight, fontSize](const char* id) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fontSize * 0.6f, fontSize * 0.4f));
        const float height = std::max(ImGui::GetContentRegionAvail().y - footerHeight, ImGui::GetFrameHeight() * 4.0f);
        ImGui::BeginChild(id, ImVec2(0.0f, height), true);
      };
      auto endTabPage = []() {
        ImGui::EndChild();
        ImGui::PopStyleVar();
      };

      constexpr ImGuiTabBarFlags kTabBarFlags = ImGuiTabBarFlags_NoCloseWithMiddleMouseButton;
      constexpr ImGuiTabItemFlags kTabItemFlags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;
      if (ImGui::BeginTabBar("Settings Tabs", kTabBarFlags)) {
        if (ImGui::BeginTabItem("General", nullptr, kTabItemFlags)) {
          beginTabPage("##tab_child_general");
          showUserGeneralSettings(ctx);
          endTabPage();
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Graphics", nullptr, kTabItemFlags)) {
          beginTabPage("##tab_child_graphics");
          showUserRenderingSettings(ctx);
          endTabPage();
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Content", nullptr, kTabItemFlags)) {
          beginTabPage("##tab_child_content");
          showUserContentSettings();
          endTabPage();
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Developer Menu Guide", nullptr, kTabItemFlags)) {
          beginTabPage("##tab_child_guide");
          showUserDeveloperMenuGuide();
          endTabPage();
          ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
      }

      showUserMenuFooter();

      ImGui::EndPopup();
    }

    // Close via titlebar close button
    if (!basicMenuOpen) {
      switchMenu(UIType::None);
    }

    ImGui::PopStyleVar(kScaledStyleVarCount);
    if (menuFont != nullptr) {
      ImGui::PopFont();
    }
  }

  void ImGUI::showUserMenuHeader() const {
    // The frame rate box and the memory bar start at the same distance from the line start.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float controlOffsetX = std::max(ImGui::CalcTextSize("Frame Rate Limit").x, ImGui::CalcTextSize("Video Memory").x) +
                                 style.ItemSpacing.x * 3.0f;

    // The Donate button sits beside the settings, in the part of the window that never scrolls.
    if (hasDonateLink()) {
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetFontSize() * 0.75f, 0.0f));
      const bool table = ImGui::BeginTable("##userMenuHeader", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings);
      ImGui::PopStyleVar();
      if (table) {
        ImGui::TableSetupColumn("##settings", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("##donate", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableNextColumn();
        showFrameRateLimitControl(controlOffsetX);
        showCompactMemoryStats(controlOffsetX);

        // The button fills its column, and with its text line it is as tall as the two rows beside it.
        ImGui::TableNextColumn();
        const float height = std::max(ImGui::GetFrameHeight() * 2.0f - ImGui::GetTextLineHeight(), ImGui::GetFrameHeight());
        showDonateButton(ImVec2(ImGui::GetContentRegionAvail().x, height));
        ImGui::EndTable();
        return;
      }
    }

    showFrameRateLimitControl(controlOffsetX);
    showCompactMemoryStats(controlOffsetX);
  }

  void ImGUI::showCompactMemoryStats(float controlOffsetX) const {
    if (RtxOptions::Automation::disableDisplayMemoryStatistics()) {
      return;
    }

    const VideoMemoryUsage usage = getVideoMemoryUsage();
    const float fraction = usage.totalMiB > 0.0f ? usage.usedMiB / usage.totalMiB : 0.0f;
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.f / %.f MiB (%.f%%)", usage.usedMiB, usage.totalMiB, fraction * 100.0f);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Video Memory");
    ImGui::SameLine(controlOffsetX);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, usage.barColor);
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, ImGui::GetFrameHeight()), overlay);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      const std::string tooltip = str::format(productName(), " dynamically uses available VRAM to maximize texture quality.\n",
                                              static_cast<uint32_t>(usage.freeMiB), " MiB are free.");
      RemixGui::SetTooltipUnformatted(tooltip.c_str());
    }
  }

  void ImGUI::showUserMenuFooter() {
    const RtxOptionLayer* userLayer = RtxOptionLayer::getUserLayer();
    const bool unsavedChanges = userLayer && userLayer->hasUnsavedChanges();

    pushButtonStyle();

    if (ImGui::Button("Developer Settings Menu")) {
      switchMenu(UIType::Advanced);
    }

    // Delete and Save sit at the right edge, or on a line of their own when the window is too narrow.
    constexpr const char* kDeleteLabel = "Delete All User Settings";
    constexpr const char* kSaveLabel = "Save Settings";
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rightWidth = ImGui::CalcTextSize(kDeleteLabel).x + ImGui::CalcTextSize(kSaveLabel).x +
                             style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SameLine();
    const float available = ImGui::GetContentRegionAvail().x;
    if (available >= rightWidth) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - rightWidth);
    } else {
      ImGui::NewLine();
    }

    // Delete turns red under the mouse, since it discards every user setting.
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.22f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.16f, 0.15f, 1.0f));
    if (ImGui::Button(kDeleteLabel) && userLayer) {
      userLayer->removeFromAllOptions();
    }
    ImGui::PopStyleColor(2);
    RemixGui::SetTooltipToLastWidgetOnHover("Resets all user settings to their default values.");

    // Save takes the theme's accent color while there are unsaved changes, and is disabled otherwise.
    ImGui::SameLine();
    ImGui::BeginDisabled(!unsavedChanges);
    if (unsavedChanges) {
      const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_TabActive);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(accent.x * 1.25f, 1.0f), std::min(accent.y * 1.25f, 1.0f), std::min(accent.z * 1.25f, 1.0f), 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
    if (ImGui::Button(kSaveLabel)) {
      const_cast<RtxOptionLayer*>(userLayer)->save();
    }
    if (unsavedChanges) {
      ImGui::PopStyleColor(4);
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (unsavedChanges) {
        // Only build the expensive tooltip when hovered
        std::string tooltip = OptionLayerUI::renderToString(userLayer, "user.conf");
        tooltip += "\nSome settings may only take effect on next launch.";
        RemixGui::SetTooltipUnformatted(tooltip.c_str());
      } else {
        RemixGui::SetTooltipUnformatted("No unsaved changes in user.conf.\nSome settings may only take effect on next launch.");
      }
    }

    popButtonStyle();
  }

  void ImGUI::showUserGeneralSettings(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();
    DxvkDLSS& dlss = common->metaDLSS();
    DxvkRayReconstruction& rayReconstruction = common->metaRayReconstruction();
    DxvkDLFG& dlfg = common->metaDLFG();
    const RtxReflex& reflex = m_device->getCommon()->metaReflex();

    const bool dlssSupported = dlss.supportsDLSS();
    const bool dlfgSupported = dlfg.supportsDLFG();
    const bool dlssRRSupported = rayReconstruction.supportsRayReconstruction();
    const bool reflexInitialized = reflex.reflexInitialized();

    // Describe the tab

    const char* tabDescriptionText = "General performance settings. Enabling upscaling is recommended to significantly increase performance.";

    // Note: Specifically reference the DLSS preset when present.
    if (dlssSupported) {
      tabDescriptionText = "General performance settings. Enabling the DLSS preset is recommended to significantly increase performance.";
    }

    ImGui::TextWrapped("%s", tabDescriptionText);

    SectionColumns columns("##generalColumns", true);

    // Preset Settings

    if (dlssSupported) {
      const DlssPreset prevDlssPreset = RtxOptions::dlssPreset();

      sectionHeader("Preset Settings");

      dlssPresetCombo.getKey(&RtxOptions::dlssPresetObject());

      // Revert back to default DLSS settings when switch from Off to Custom
      if (prevDlssPreset == DlssPreset::Off && RtxOptions::dlssPreset() == DlssPreset::Custom) {
        RtxOptions::resetUpscaler();
      }

      RtxOptions::updateUpscalerFromDlssPreset();
    }

    // Note: Disable all settings in this section beyond the preset when a non-Custom DLSS preset is in use,
    // but only when DLSS is actually supported.
    // Note: This is stored as a bool and applied in a SetDisabled per-section so that the section labels do not get disabled
    // (as this changes the color of the line and text which is undesirable).
    const bool disableNonPresetSettings = RtxOptions::dlssPreset() != DlssPreset::Custom && dlssSupported;

    // Upscaling Settings

    sectionHeader("Upscaling Settings");

    {
      ImGui::BeginDisabled(disableNonPresetSettings);

      // Upscaler Type

      // Note: Use a different combo box without DLSS's upscaler listed if DLSS overall is unsupported.
      auto oldUpscalerType = RtxOptions::upscalerType();
      bool oldDLSSRREnabled = RtxOptions::enableRayReconstruction();

      if (dlss.supportsDLSS()) {
        getUpscalerCombo(dlss, rayReconstruction).getKey(&RtxOptions::upscalerTypeObject());
      }

      columns.beginSubItems();

      if (dlss.supportsDLSS()) {
        showRayReconstructionEnable(dlssRRSupported);

        // If DLSS-RR is toggled, need to update some path tracer options accordingly to improve quality
        if (oldUpscalerType != RtxOptions::upscalerType() || oldDLSSRREnabled != RtxOptions::enableRayReconstruction()) {
          RtxOptions::updateLightingSetting();
        }
      } else {
        getUpscalerCombo(dlss, rayReconstruction).getKey(&RtxOptions::upscalerTypeObject());
      }

      // Upscaler Preset


      switch (RtxOptions::upscalerType()) {
        case UpscalerType::DLSS: {
          // Preset is shown before the mode. Ray Reconstruction has its own preset selector
          // (rendered by showRayReconstructionEnable above), so only show the SR preset here.
          if (!RtxOptions::enableRayReconstruction()) {
            dlssRenderPresetCombo.getKey(&DxvkDLSS::presetObject());
          }

          dlssProfileCombo.getKey(&RtxOptions::qualityDLSSObject());

          // Display DLSS Upscaling Information

          const auto currentDLSSProfile = RtxOptions::enableRayReconstruction() ? rayReconstruction.getCurrentProfile() : dlss.getCurrentProfile();
          uint32_t dlssInputWidth, dlssInputHeight;

          if (RtxOptions::enableRayReconstruction()) {
            rayReconstruction.getInputSize(dlssInputWidth, dlssInputHeight);
          } else {
            dlss.getInputSize(dlssInputWidth, dlssInputHeight);
          }

          ImGui::TextWrapped(str::format("Computed DLSS Mode: ", dlssProfileToString(currentDLSSProfile), ", Render Resolution: ", dlssInputWidth, "x", dlssInputHeight).c_str());

          break;
        }
        case UpscalerType::NIS: {
          nisPresetCombo.getKey(&RtxOptions::nisPresetObject());
          RtxOptions::updateUpscalerFromNisPreset();

          // Display NIS Upscaling Information

          auto resolutionScale = RtxOptions::resolutionScale();

          ImGui::TextWrapped(str::format("NIS Resolution Scale: ", resolutionScale).c_str());

          break;
        }
        case UpscalerType::TAAU: {
          taauPresetCombo.getKey(&RtxOptions::taauPresetObject());
          RtxOptions::updateUpscalerFromTaauPreset();

          // Display TAA-U Upscaling Information

          auto resolutionScale = RtxOptions::resolutionScale();

          ImGui::TextWrapped(str::format("TAA-U Resolution Scale: ", resolutionScale).c_str());

          break;
        }
        case UpscalerType::XeSS: {
          xessPresetCombo.getKey(&DxvkXeSS::XessOptions::presetObject());

          // Show resolution slider only for Custom preset
          if (DxvkXeSS::XessOptions::preset() == XeSSPreset::Custom) {
            RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.1f, 1.0f, "%.2f");
          }

          // Display XeSS internal resolution
          auto& xess = ctx->getCommonObjects()->metaXeSS();

          uint32_t inputWidth;
          uint32_t inputHeight;
          xess.getInputSize(inputWidth, inputHeight);
          ImGui::TextWrapped(str::format("Render Resolution: ", inputWidth, "x", inputHeight).c_str());

          break;
        }
        case UpscalerType::FSR: {
          fork_hooks::showFsrUpscalerSettings(ctx);
          break;
        }
        case UpscalerType::None: {
          // No custom UI here.
          break;
        }
      }

      fork_hooks::showSharedSharpnessSlider();

      columns.endSubItems();

      ImGui::EndDisabled();
    }

    if (fork_hooks::anyFrameGenerationSupported(ctx, dlfgSupported)) {
      sectionHeader("Frame Generation Settings");
      // NV-DXVK start: frame-generation panel (DLSS-G / FSR-FG selector)
      fork_hooks::showFrameGenerationOptions(ctx, dlfgSupported);
      // NV-DXVK end
    }

    if (reflexInitialized) {
      sectionHeader("Latency Reduction Settings");

      {
        ImGui::BeginDisabled(disableNonPresetSettings);

        // Note: Option to toggle the stats window is set to false here as this window is currently
        // set up to display only when the "advanced" developer settings UI is active.
        showReflexOptions(ctx, false);

        ImGui::EndDisabled();
      }
    }

    columns.nextColumn();

    // DLSS-NR is an opt-in effect, so the DLSS preset and the graphics presets leave it alone.
    {
      sectionHeader("Neural Generation Settings");

      DxvkNeuralUplift& neuralUplift = common->metaNeuralUplift();
      if (neuralUplift.isSupported()) {
        ImGui::TextWrapped("Neural enhancement of the final image. It costs significant GPU time.");
        ImGui::PushID("DLSS 3D-Guided Neural Generation");
        neuralUplift.showImguiSettings(false);
        ImGui::PopID();
      } else {
        ImGui::BeginDisabled();
        ImGui::TextWrapped("DLSS 3D-Guided Neural Generation is not supported on this GPU or driver. It needs an NVIDIA RTX GPU and a driver with DLSS-NR support.");
        ImGui::EndDisabled();
      }
    }

    if (host_settings::hasUserMenuSettings()) {
      sectionHeader("Game Settings");
      host_settings::showUserMenuSettings();
    }
    sectionHeader("Screenshots");

    if (ImGui::Button("Take Screenshot")) {
      common->metaScreenshot().request();
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Saves the next frame as a PNG, with the game's interface and without this menu. "
                                            "With HDR output active, an HDR PNG and an SDR PNG are saved.");
    RemixGui::InputKeyBind("Screenshot Key", &RtxScreenshot::hotkeyObject());
    RemixGui::InputText("Screenshot Folder", &RtxScreenshot::folderObject());
    ImGui::TextWrapped("Saved to: %s", RtxScreenshot::resolveFolder().c_str());

    sectionHeader("Problems");
    FaultNotice::showSection();

    sectionHeader("Logs");
    showLogFolder();
  }

  void ImGUI::showUserRenderingSettings(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();
    DxvkPostFx& postFx = common->metaPostFx();

    // Describe the tab

    ImGui::TextWrapped("Rendering-specific settings. Complexity of rendering may be adjusted to balance between performance and quality.");

    SectionColumns columns("##graphicsColumns", true);

    // Preset Settings

    sectionHeader("Preset Settings");

    graphicsPresetCombo.getKey(&RtxOptions::graphicsPresetObject());

    // Map settings to indirect particle level
    int indirectLightParticlesLevel = 0;
    if (RtxOptions::enableUnorderedResolveInIndirectRays()) {
      indirectLightParticlesLevel = RtxOptions::enableUnorderedEmissiveParticlesInIndirectRays() ? 2 : 1;
    }

    // Path Tracing Settings

    sectionHeader("Path Tracing Settings");

    {
      // Note: Disabled flags should match preset mapping above to prevent changing settings when a preset overrides them.
      ImGui::BeginDisabled(RtxOptions::graphicsPreset() != GraphicsPreset::Custom);

      minPathBouncesCombo.getKey(&RtxOptions::pathMinBouncesObject());
      maxPathBouncesCombo.getKey(&RtxOptions::pathMaxBouncesObject());
      indirectLightingParticlesCombo.getKey(&indirectLightParticlesLevel);
      RemixGui::SetTooltipToLastWidgetOnHover("Controls the quality of particles in indirect (reflection/GI) rays.");

      // NRC Quality Preset dropdown
      NeuralRadianceCache& nrc = common->metaNeuralRadianceCache();
      if (nrc.checkIsSupported(m_device)) {
        bool enableNeuralRadianceCache = RtxOptions::resolvedIntegrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache;

        // Disable NRC quality preset combo when NRC is not enabled.
        ImGui::BeginDisabled(!enableNeuralRadianceCache);
        
        neuralRadianceCacheQualityPresetCombo.getKey(&NeuralRadianceCache::NrcOptions::qualityPresetObject());

        ImGui::EndDisabled();
      }

      // Hide NRD denoiser quality list when DLSS-RR is enabled.
      bool useRayReconstruction = RtxOptions::isRayReconstructionEnabled();
      if (!useRayReconstruction) {
        denoiserQualityCombo.getKey(&RtxOptions::denoiseDirectAndIndirectLightingSeparatelyObject());
      }

      ImGui::EndDisabled();
    }

    // Haze and cloud settings belong to the NUMOS atmosphere, so they show only while it draws the sky.
    // They fill the second column when they show, and the post effects move there otherwise.
    const bool numosSky = RtxOptions::skyMode() == SkyMode::Numos;
    if (!numosSky) {
      columns.nextColumn();
    }

    // Post Effect Settings

    sectionHeader("Post Effect Settings");

    // The grade is a look, not a quality setting, so the graphics presets leave it alone.
    ColorGrade::showUserPresetPicker();

    {
      {
        // Note: All presets aside from Custom will overwrite this, so only enable for Custom.
        ImGui::BeginDisabled(RtxOptions::graphicsPreset() != GraphicsPreset::Custom);
        RemixGui::Checkbox("Enable Post Effects", &postFx.enableObject());
        ImGui::EndDisabled();
      }

      // Note: Medium and Low presets disable all post effects, so no value in changing the individual settings.
      // High and Ultra allow these to be changed without requiring Custom, so leave enabled for those.
      ImGui::BeginDisabled(RtxOptions::graphicsPreset() == GraphicsPreset::Medium || RtxOptions::graphicsPreset() == GraphicsPreset::Low);
      {
        columns.beginSubItems();

        ImGui::BeginDisabled(!postFx.enable());

        RemixGui::Checkbox("Enable Motion Blur", &postFx.enableMotionBlurObject());
        // NV-DXVK start: depth-of-field quick setting
        RemixGui::Checkbox("Enable Depth of Field", &postFx.dofEnableObject());
        RemixGui::Checkbox("DoF Auto Focus", &postFx.autoFocusEnableObject());
        // NV-DXVK end
        RemixGui::Checkbox("Enable Chromatic Aberration", &postFx.enableChromaticAberrationObject());
        RemixGui::Checkbox("Enable Vignette", &postFx.enableVignetteObject());

        ImGui::EndDisabled();

        columns.endSubItems();
      }

      ImGui::EndDisabled();
    }

    // Other Settings

    sectionHeader("Other Settings");

    {
      showVsyncOptions(true);
      // The swap chain is made when the game starts, so a change applies after a restart.
      RemixGui::Checkbox("Allow HDR Output (after restart)", &DxvkToneMapping::hdrEnabledObject());
    }

    // The graphics presets do not set the haze and cloud settings.
    if (numosSky) {
      columns.nextColumn();

      sectionHeader("Night Settings");
      showNumosUserNight(ctx.ptr());

      sectionHeader("Haze Settings");
      showNumosUserHaze(ctx.ptr());

      sectionHeader("Cloud Settings");
      showNumosUserClouds(ctx.ptr());
    }

    // Map indirect particle level back to settings
    if (RtxOptions::graphicsPreset() == GraphicsPreset::Custom) {
      switch (indirectLightParticlesLevel) {
      case 0:
        RtxOptions::enableUnorderedEmissiveParticlesInIndirectRays.setDeferred(false);
        RtxOptions::enableUnorderedResolveInIndirectRays.setDeferred(false);
        break;
      case 1:
        RtxOptions::enableUnorderedEmissiveParticlesInIndirectRays.setDeferred(false);
        RtxOptions::enableUnorderedResolveInIndirectRays.setDeferred(true);
        break;
      case 2:
        RtxOptions::enableUnorderedEmissiveParticlesInIndirectRays.setDeferred(true);
        RtxOptions::enableUnorderedResolveInIndirectRays.setDeferred(true);
        break;
      }
    }
  }

  void ImGUI::showUserDeveloperMenuGuide() {
    auto bullet = [](const char* text) {
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::TextWrapped("%s", text);
    };

    ImGui::TextWrapped("The developer menu holds every renderer setting, far more than this menu. Use it to fine-tune "
                       "the look or the performance yourself.");

    sectionHeader("Opening and Closing");
    bullet("Click Developer Settings Menu at the bottom of this window to open it. Click Graphics Settings Menu at the "
           "top of the developer menu to come back here.");
    bullet("Alt+X (the default key) opens and closes the menus. It opens this menu, unless you tick Default Menu at the "
           "top of the developer menu.");
    bullet("The << or >> button at the end of the developer menu's tab row moves it to the other side of the screen.");

    sectionHeader("Developer Menu Tabs");
    bullet("Rendering: almost every renderer setting, in sections you open by clicking their headers. General has "
           "upscaling, frame generation and DLSS 3D-Guided Neural Generation. Pathtracing, Lighting, RTX Volumetrics "
           "and Denoising control how light is traced and cleaned up. Post-Processing has tone mapping, auto exposure, "
           "bloom, motion blur and the order the effects run in.");
    bullet("Host Settings: appears only when the game's integration adds settings of its own.");
    bullet("Game Setup: how the renderer reads the game. Step 1 sorts the game's textures into categories. Step 2 holds "
           "scene parameters, and its Sky section has the sky, clouds, haze, night and weather pages.");
    bullet("Enhancements: the replacement asset mods that are loaded, and the capture tools.");
    bullet("About: the version, the credits and the licenses.");
    bullet("Dev Settings: tools for developers, such as performance logging, camera controls, option layers and UI "
           "options.");
    bullet("Plugin: appears only when a plugin adds a page of its own.");

    sectionHeader("What to Change");
    bullet("Safe to try: Rendering > General, Rendering > Post-Processing, and the Sky, Clouds, Haze and Night pages "
           "under Game Setup > Step 2 > Sky. They change the look or the performance, and you can always reset them.");
    bullet("Advanced: Pathtracing, Lighting, Denoising, Game Setup's texture categories and scene parameters, the "
           "Sky section's Setup page, Host Settings, Rendering > Debug and Dev Settings. Wrong values there can break "
           "the image or the lighting, so change them only if you know what they do.");

    sectionHeader("Saving and Resetting");
    bullet("Changes apply at once, but they are lost when you quit unless you save them.");
    bullet("In this menu, Save Settings writes your settings to user.conf in the game folder. In the developer menu, "
           "Settings Management at the bottom has a Save button for each file.");
    bullet("Player settings, such as the ones in this menu, go to user.conf. Other developer menu settings go to "
           "rtx.conf, the game's own renderer configuration. Back up rtx.conf before you save it.");
    bullet("The round button at the right of a setting resets that setting. Delete All User Settings at the bottom of "
           "this window resets everything in user.conf, and Save Settings makes that permanent. Deleting user.conf "
           "while the game is closed does the same.");

    sectionHeader("Mouse and Keyboard");
    bullet("Hover over any setting to see what it does.");
    bullet("Drag a slider or a number to change it, or Ctrl+click it to type an exact value.");
    bullet("Click a section header to open or close it, and scroll with the mouse wheel.");
    bullet("Alt+Delete shows or hides the mouse cursor if it is missing.");
  }

  void ImGUI::showUserContentSettings() {
    // Describe the tab

    ImGui::TextWrapped("Content-specific settings. Allows control of what types of assets Remix should replace (if any).");

    SectionColumns columns("##contentColumns", false);

    sectionHeader("Enhanced Assets");
    RemixGui::Checkbox("Enable All Enhanced Assets", &RtxOptions::enableReplacementAssetsObject());

    {
      columns.beginSubItems();

      ImGui::BeginDisabled(!RtxOptions::enableReplacementAssets());

      RemixGui::Checkbox("Enable Enhanced Materials", &RtxOptions::enableReplacementMaterialsObject());
      RemixGui::Checkbox("Enable Enhanced Meshes", &RtxOptions::enableReplacementMeshesObject());
      RemixGui::Checkbox("Enable Enhanced Lights", &RtxOptions::enableReplacementLightsObject());

      ImGui::EndDisabled();

      columns.endSubItems();
    }
  }

} // namespace dxvk
