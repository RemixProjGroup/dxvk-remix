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

#pragma once

#include "imgui.h"

#include <chrono>
#include <iterator>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/util_keybind.h"

#include "../dxvk_format.h"
#include "../dxvk_util.h"
#include "../dxvk_bind_mask.h"
#include "../dxvk_graphics_state.h"
#include "../dxvk_constant_state.h"
#include "../rtx_render/rtx_options.h"
#include "dxvk_imgui_option_search.h"

struct ImPlotContext;

namespace dxvk {
  class ImGuiAbout;
  class ImGuiFirstUseGuide;
  class ImGuiSplash;
  class ImGuiCapture;
  class DxvkDevice;
  class DxvkContext;
  class DxvkObjects;
  class RtxGraphGUI;

  /**
   * \brief DXVK ImGUI
   * 
   * GUI for manipulating settings at runtime
   */
  class ImGUI {
    
  public:
    // if set for a texture category, only textures with that flag can be set to that category.
    static const uint32_t kTextureFlagsNone = 0; 
    // Use this for regular textures
    static const uint32_t kTextureFlagsDefault = 1 << 0; 
    // Use this for render target textures
    static const uint32_t kTextureFlagsRenderTarget = 1 << 1; 
    
    enum class Theme {
      Toolkit,
      Legacy,
      Nvidia,
      RemixPlus
    };

    ImGUI(DxvkDevice* device);
    ~ImGUI();

    bool isInit() const {
      return m_init;
    }
    
    /**
     * \brief Handle windows messages
     * 
     * see: https://docs.microsoft.com/en-us/previous-versions/windows/desktop/legacy/ms633573(v=vs.85)
     */
    void wndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /**
     * \brief Render ImGUI
     * 
     * Renders the ImGUI to the given context.
     * \param [in] ctx Device context
     * \param [in] surfaceSize Image size, in pixels
     */
    void render(const Rc<DxvkContext>&  ctx,  VkExtent2D surfaceSize);
    
    static void AddTexture(const XXH64_hash_t hash, const Rc<DxvkImageView>& imageView, uint32_t textureFeatureFlags);
    static void ReleaseTexture(const XXH64_hash_t hash);
    static bool checkHotkeyState(const VirtualKeys& virtKeys, const bool allowContinuousPress = false);
    // Draws the Open Log Folder button and the folder that really holds the logs.
    // With withReportHelp, it also draws rtx.gui.logHelpText above them and the rtx.gui.reportFile buttons below.
    static void showLogFolder(bool withReportHelp = true);
    static void SetFogStates(const fast_unordered_cache<FogState>& fogStates, XXH64_hash_t usedFogHash);

    void switchMenu(UIType type, bool force = false);
    
    enum Tabs {
      kTab_Rendering = 0,
      kTab_HostSettings,
      kTab_Setup,
      kTab_Enhancements,
      kTab_About,
      kTab_Development,
      kTab_Wrapper,
      kTab_Count
    };
    template<Tabs tab>
    void openTab() {
      RtxOptions::showUI.setDeferred(UIType::Advanced);
      triggerTab(tab);
    }
    template<Tabs tab>
    bool isTabOpen() const {
      if(RtxOptions::showUI() != UIType::Advanced) {
        return false;
      } else {
        return m_curTab == tab;
      }
    }

  private:
    enum class ShaderMessageType {
      None,
      Ok,
      Error
    };

    DxvkDevice*           m_device;
    
    Rc<DxvkImage>         m_fontTexture;
    Rc<DxvkImageView>     m_fontTextureView;
    DxvkRasterizerState   m_rsState;
    DxvkBlendMode         m_blendMode;
    VkDescriptorPool      m_imguiPool;
    Rc<ImGuiAbout>        m_about;
    Rc<ImGuiFirstUseGuide> m_firstUseGuide;
    Rc<ImGuiSplash>       m_splash;
    Rc<ImGuiCapture>      m_capture;
    ImGuiOptionSearch     m_optionSearch;
    Rc<class GameOverlay>  m_overlayWin;

    // Note: May be NULL until the font loads, needs to be checked before use.
    ImFont*               m_regularFont = nullptr;
    ImFont*               m_boldFont = nullptr;
    // Note: May be NULL until the font loads, needs to be checked before use.
    ImFont*               m_largeFont = nullptr;
    // Medium-weight fonts the user menu picks from to scale its text with the output height, smallest first.
    // Note: May be NULL until the fonts load.
    static constexpr float kUserMenuFontSizes[] = { 20.0f, 24.0f, 28.0f, 32.0f, 40.0f, 48.0f };
    ImFont*               m_userMenuFonts[std::size(kUserMenuFontSizes)] = {};

    ImGuiContext*         m_context;
    ImPlotContext*        m_plotContext;

    HWND                  m_gameHwnd;
    bool                  m_init = false;
    bool                  m_prevCursorVisible = false;

    int                   m_cachedGameCursorX = 0;
    int                   m_cachedGameCursorY = 0;

    // Width of developer menu in regular mode
    float                 m_regularWindowWidth = 450.0f;
    // Width of item+label widgets in regular mode (developer menu)
    float                 m_regularWindowWidgetWidth = 150.f;

    // Width of developer menu in large mode
    float                 m_largeWindowWidth = 670.0f;
    // Width of item+label widgets in large mode (developer menu)
    float                 m_largeWindowWidgetWidth = 364.0f;

    bool                  m_windowOnRight = true;
    bool                  m_pendingUIOptionsScroll = false;


    float                 m_windowWidth = m_regularWindowWidth;

    const char*           m_userGraphicsWindowTitle = "User Graphics Settings";
    bool m_hudMessageTimeReset = false;
    std::chrono::time_point<std::chrono::steady_clock> m_hudMessageStartTime;
    // The shader prewarm wait screen (rtx.shader.blockUntilPrewarmed) is up, and the game's input is held back.
    bool m_prewarmScreenActive = false;
    bool m_reflexRangesInitialized = false;
    float m_currentGameToRenderDurationMin;
    float m_currentGameToRenderDurationMax;
    float m_currentCombinedDurationMin;
    float m_currentCombinedDurationMax;
    float m_reflexLatencyStatsWindowWidth = 500.f;
    float m_reflexLatencyStatsWindowHeight = 650.f;
    bool m_reflexLatencyStatsOpen = false;
    bool m_lastRenderVsyncStatus = false;
    std::unique_ptr<RtxGraphGUI> m_graphGUI;

    static constexpr const char* tabNames[] = { "Rendering", "Host Settings", "Game Setup", "Enhancements", "About" , "Dev Settings", "Plugin"};
    Tabs m_curTab = kTab_Count;
    Tabs m_triggerTab = kTab_Count;
    void triggerTab(const Tabs tab) {
      m_triggerTab = tab;
    }

    void update(const Rc<DxvkContext>& ctx);

    void updateQuickActions(const Rc<DxvkContext>& ctx);

    void showDebugVisualizations(const Rc<DxvkContext>& ctx);

    void showMainMenu(const Rc<DxvkContext>& ctx);

    void showUserMenu(const Rc<DxvkContext>& ctx);
    // Returns the user menu font closest to targetSize pixels.
    // Returns null while the fonts are not loaded.
    ImFont* getUserMenuFont(float targetSize) const;
    // Draws the fixed area at the top of the user menu: the frame rate limit, the video memory use and the Donate button.
    void showUserMenuHeader() const;
    // Draws the user menu's bottom row of buttons.
    void showUserMenuFooter();
    void showUserGeneralSettings(const Rc<DxvkContext>& ctx);
    void showUserRenderingSettings(const Rc<DxvkContext>& ctx);
    void showUserContentSettings();
    void showUserDeveloperMenuGuide();

    void showHudMessages(const Rc<DxvkContext>& ctx);

    // Draws the full-screen wait screen while the startup shader prewarm compiles (rtx.shader.blockUntilPrewarmed).
    void showShaderPrewarmScreen(const Rc<DxvkContext>& ctx);
    void setPrewarmScreenActive(bool active);

    void showRenderingSettings(const Rc<DxvkContext>& ctx);
    // The SCCR block of the Displacement settings, shown in SCCR mode.
    void showSccrSettings(DxvkObjects* common);
    void showDeveloperSection(const Rc<DxvkContext>& ctx);

    void showDLFGOptions(const Rc<DxvkContext>& ctx);
    void showReflexOptions(const Rc<DxvkContext>& ctx, bool displayStatsWindowToggle);
    void showReflexLatencyStats();

    void showSetupWindow(const Rc<DxvkContext>& ctx);

    void showMaterialOptions();

    void showEnhancementsWindow(const Rc<DxvkContext>& ctx);
    void showEnhancementsTab(const Rc<DxvkContext>& ctx);
    void showDevelopmentSettings(const Rc<DxvkContext>& ctx);

    // helper to display a configurable grid of all textures currently hooked to ImGUI
    void showTextureSelectionGrid(const Rc<DxvkContext>& ctx, const char* uniqueId, const uint32_t texturesPerRow, const float thumbnailSize, const float minChildHeight = 600.0f);

    void createFontsTexture(const Rc<DxvkContext>& ctx);

    // Adjust Alpha of currently set background color
    void adjustStyleBackgroundAlpha(const float& alpha, ImGuiStyle* dst = NULL);
    // Adjusts window widths based on various UI settings
    void updateWindowWidths();
    // Sets default remix UI theme
    void setLegacyStyle(ImGuiStyle* dst);
    // Sets toolkit inspired UI theme
    void setToolkitStyle(ImGuiStyle* dst);
    // Sets Nvidia inspired UI theme
    void setNvidiaStyle(ImGuiStyle* dst);
    void setRemixPlusStyle(ImGuiStyle* dst);
    // Custom style
    void setupStyle(ImGuiStyle* dst = NULL);

    void showVsyncOptions(bool enableDLFGGuard);
    void processHotkeys();

    struct VideoMemoryUsage {
      float usedMiB = 0.0f;
      float totalMiB = 0.0f;
      float freeMiB = 0.0f;
      // Goes from green to red as the free memory runs out.
      ImVec4 barColor;
    };
    VideoMemoryUsage getVideoMemoryUsage() const;
    void showMemoryStats() const;
    // Draws the allocator's chunk use per memory type and chunk class, and its allocation counters.
    void showAllocatorChunks() const;
    // Draws the video memory use as one labeled bar that starts controlOffsetX from the start of the line.
    void showCompactMemoryStats(float controlOffsetX) const;
    bool showRayReconstructionEnable(bool supportsRR);

    RTX_OPTION_ARGS("rtx.gui", bool, showLegacyTextureGui, false, "A setting to toggle the old texture selection GUI, where each texture category is represented as its own list.", args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.gui", bool, legacyTextureGuiShowAssignedOnly, false, "A setting to show only the textures in a category that are assigned to it (Unassigned textures are found in the new \"Uncategorized\" list at the top).\nRequires: \'Split Texture Category List\' option to be enabled.", args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.gui", std::uint32_t, hudMessageAnimatedDotDurationMilliseconds, 1000, "A duration in milliseconds between each dot in the animated dot sequence for HUD messages. Must be greater than 0.\nThese dots help indicate progress is happening to the user with a bit of animation which can be configured to animate at whatever speed is desired.");
    RTX_OPTION_ARGS("rtx.gui", float, reflexStatRangeInterpolationRate, 0.05f, "A value controlling the interpolation rate applied to the Reflex stat graph ranges for smoother visualization.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.gui", float, reflexStatRangePaddingRatio, 0.05f, "A value specifying the amount of padding applied to the Reflex stat graph ranges as a ratio to the calculated range.",
                    args.minValue = 0.0f);
    RTX_OPTION("rtx.gui", bool, enableExternalPresenter, false, "Enables GUI rendering for external presenters.");
    RTX_OPTION("rtx.gui", Vector4, subRect, Vector4(0.f, 0.f, 0.f, 0.f), "Used for toolkit.");
    RTX_OPTION("rtx.gui", std::string, logHelpText, std::string(""),
               "The instructions the log section of the menus shows above the log folder, such as which file to send with a bug report and where to send it.\n"
               "Empty shows a short generic instruction.");
    RTX_OPTION_FLAG("rtx.gui", std::string, reportFile, std::string(""), RtxOptionFlags::NoSave,
                    "Set by the game integration (remix API SetConfigVariable): the full path of the newest report file, such as a zip of a session's logs. "
                    "When set, the log section of the menus names the file and gets buttons that show it in Explorer and copy its path.");
    RTX_OPTION("rtx.gui", std::string, productName, std::string("Remix Plus"),
               "The product name the menus show to the player: in the startup message, the welcome window, the developer menu title and the About tab.\n"
               "A game's rtx.conf can set it to the name of its RTX release. The license text keeps the name RTX Remix.");
    RTX_OPTION("rtx.gui", std::string, donateUrl, std::string(""),
               "A link the welcome window, the About tab and the top of the user graphics menu show as a Donate button. Empty hides the button. Only https links are opened.");

    // The release version: the last word of the build ID when it starts with a digit ("1.0.4" from
    // "Game Release 1.0.4"), else empty.
    public: static std::string productVersion();
    // The product name followed by the release version when there is one.
    public: static std::string productNameWithVersion();

    // Returns true when rtx.gui.donateUrl holds an https link.
    public: static bool hasDonateLink();
    // Draws the Donate line: the text and a small button on one line by default.
    // With a buttonSize width above 0, the text is centered above a button of that size.
    public: static void showDonateButton(const ImVec2& buttonSize = ImVec2(0.0f, 0.0f));
    
    public: static void onThemeChange(DxvkDevice* device);
    public: static void onBackgroundAlphaChange(DxvkDevice* device);
    RTX_OPTION_ARGS("rtx.gui", bool, compactGui, false, "A setting to toggle between compact and spacious GUI modes.",
                    args.onChangeCallback = &onThemeChange, args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.gui", float, backgroundAlpha, 1.f, "A value controlling the alpha of the GUI background.",
                    args.onChangeCallback = &onBackgroundAlphaChange, args.minValue = 0.0f, args.maxValue = 1.0f, args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.gui", Theme, themeGui, Theme::RemixPlus, "A setting controlling the active GUI theme.",
                    args.onChangeCallback = &onThemeChange, args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.gui", bool, largeUiMode, false, "Toggles between Large and Regular GUI Scale Modes.",
                    args.onChangeCallback = &onThemeChange, args.flags = RtxOptionFlags::UserSetting);

    private:

    void onCloseMenus();
    void onOpenMenus();
    void freeUnusedMemory();

  };
  
}
