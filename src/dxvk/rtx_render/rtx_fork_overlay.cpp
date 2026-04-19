// src/dxvk/rtx_render/rtx_fork_overlay.cpp
//
// Fork-owned file. Contains the implementations of fork_hooks:: functions
// for the overlay subsystem, lifted from rtx_overlay_window.cpp and
// rtx_context.cpp during the 2026-04-18 fork touchpoint-pattern refactor.
//
// See docs/fork-touchpoints.md for the full fork-hooks catalogue.
//
// NOTE: overlayKeyboardForward accesses GameOverlay::m_hwnd (private).
// dispatchScreenOverlay accesses multiple private members of RtxContext
// (m_pendingScreenOverlay, m_screenOverlay*) and uses ScreenOverlayShader.
// Both classes require the corresponding fork_hooks functions to be declared
// as friends — see rtx_overlay_window.h and rtx_context.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rtx_fork_hooks.h"

#include "rtx_overlay_window.h"       // GameOverlay
#include "imgui/imgui_impl_win32.h"   // ImGui_ImplWin32_WndProcHandler

#include "rtx_context.h"              // RtxContext
#include "rtx_resources.h"            // Resources::RaytracingOutput
#include "rtx/pass/screen_overlay/screen_overlay.h"
#include <rtx_shaders/screen_overlay.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dxvk {

  namespace {
    // ScreenOverlayShader — the compute shader that alpha-composites the
    // plugin-uploaded RGBA overlay over the final tone-mapped output image.
    // Moved here from the anonymous namespace in rtx_context.cpp so that
    // dispatchScreenOverlay (a fork_hooks function) can reference it without
    // requiring a separate TU.
    class ScreenOverlayShader : public ManagedShader {
      SHADER_SOURCE(ScreenOverlayShader, VK_SHADER_STAGE_COMPUTE_BIT, screen_overlay)

      PUSH_CONSTANTS(ScreenOverlayArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(SCREEN_OVERLAY_INPUT_OUTPUT)
        SAMPLER2D(SCREEN_OVERLAY_TEXTURE)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(ScreenOverlayShader);
  } // anonymous namespace

namespace fork_hooks {

  // ---------------------------------------------------------------------------
  // overlayKeyboardForward
  //
  // Forwards keyboard and character WM_* messages to ImGui's Win32 backend
  // so its io.KeyAlt / io.KeysDown state stays in sync when the legacy
  // WndProc path is the only delivery path (e.g. when a game menu captures
  // raw input and overlayWndProc is not receiving messages directly).
  //
  // Mouse messages are intentionally not forwarded: the overlay's WM_INPUT
  // path already synthesizes scaled mouse events.
  //
  // ACCESS NOTE: this function reads GameOverlay::m_hwnd (private atomic).
  // A friend declaration for this function is required in GameOverlay.
  // ---------------------------------------------------------------------------
  void overlayKeyboardForward(
      GameOverlay& overlay, HWND gameHwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    {
      HWND overlayHwnd = overlay.m_hwnd.load();
      ImGui_ImplWin32_WndProcHandler(overlayHwnd ? overlayHwnd : gameHwnd, msg, wParam, lParam);
      break;
    }
    default:
      break;
    }
  }

  // ---------------------------------------------------------------------------
  // dispatchScreenOverlay
  //
  // Alpha-composites a plugin-uploaded RGBA pixel buffer over the final
  // tone-mapped output image using the ScreenOverlayShader compute shader.
  // Called from RtxContext::dispatchScreenOverlay (which is now a one-line
  // delegate) after tone mapping and before screenshot capture.
  //
  // If no overlay has been queued this frame (m_pendingScreenOverlay is
  // empty), returns immediately with no GPU work.
  //
  // The overlay GPU image is recreated whenever dimensions or format change;
  // the staging buffer is copied in via copyBufferToImage each frame.
  //
  // ACCESS NOTE: reads and writes multiple private members of RtxContext:
  // m_pendingScreenOverlay, m_screenOverlayImage, m_screenOverlayView,
  // m_screenOverlayWidth, m_screenOverlayHeight, m_screenOverlayFormat,
  // and m_device. A friend declaration for this function is required in
  // RtxContext (see rtx_context.h).
  // ---------------------------------------------------------------------------
  void dispatchScreenOverlay(RtxContext& ctx, Resources::RaytracingOutput& rtOutput) {
    if (!ctx.m_pendingScreenOverlay.has_value()) {
      return;
    }

    ScopedGpuProfileZone(&ctx, "Screen Overlay");
    auto& overlay = *ctx.m_pendingScreenOverlay;

    // Recreate overlay image if dimensions or format changed.
    if (ctx.m_screenOverlayWidth  != overlay.width
     || ctx.m_screenOverlayHeight != overlay.height
     || ctx.m_screenOverlayFormat != overlay.format
     || !ctx.m_screenOverlayImage.ptr()) {
      DxvkImageCreateInfo imageInfo = {};
      imageInfo.type        = VK_IMAGE_TYPE_2D;
      imageInfo.format      = overlay.format;
      imageInfo.flags       = 0;
      imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.extent      = { overlay.width, overlay.height, 1 };
      imageInfo.numLayers   = 1;
      imageInfo.mipLevels   = 1;
      imageInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imageInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      imageInfo.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.layout      = VK_IMAGE_LAYOUT_UNDEFINED;

      ctx.m_screenOverlayImage = ctx.m_device->createImage(
        imageInfo,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXRenderTarget,
        "Screen overlay image");

      DxvkImageViewCreateInfo viewInfo = {};
      viewInfo.type      = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format    = overlay.format;
      viewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
      viewInfo.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
      viewInfo.minLevel  = 0;
      viewInfo.numLevels = 1;
      viewInfo.minLayer  = 0;
      viewInfo.numLayers = 1;

      ctx.m_screenOverlayView = ctx.m_device->createImageView(ctx.m_screenOverlayImage, viewInfo);

      ctx.m_screenOverlayWidth  = overlay.width;
      ctx.m_screenOverlayHeight = overlay.height;
      ctx.m_screenOverlayFormat = overlay.format;
    }

    // Copy staging buffer to overlay image
    {
      VkImageSubresourceLayers subresource = {};
      subresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
      subresource.mipLevel       = 0;
      subresource.baseArrayLayer = 0;
      subresource.layerCount     = 1;

      VkOffset3D offset = { 0, 0, 0 };
      VkExtent3D extent = { overlay.width, overlay.height, 1 };

      ctx.copyBufferToImage(ctx.m_screenOverlayImage, subresource, offset, extent,
                            overlay.stagingBuffer, 0, 0, 0);
    }

    // Dispatch overlay blend compute shader
    ctx.setPushConstantBank(DxvkPushConstantBank::RTX);

    auto& finalOutput         = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D outputSize = finalOutput.image->info().extent;
    const VkExtent3D workgroups = util::computeBlockCount(
      outputSize, VkExtent3D { SCREEN_OVERLAY_TILE_SIZE, SCREEN_OVERLAY_TILE_SIZE, 1 });

    ScreenOverlayArgs pushArgs = {};
    pushArgs.imageSize = { outputSize.width, outputSize.height };
    pushArgs.opacity   = overlay.opacity;
    ctx.pushConstants(0, sizeof(pushArgs), &pushArgs);

    ctx.bindResourceView(SCREEN_OVERLAY_INPUT_OUTPUT, finalOutput.view, nullptr);
    ctx.bindResourceView(SCREEN_OVERLAY_TEXTURE, ctx.m_screenOverlayView, nullptr);
    ctx.bindResourceSampler(SCREEN_OVERLAY_TEXTURE,
      ctx.getResourceManager().getSampler(
        VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));

    ctx.bindShader(VK_SHADER_STAGE_COMPUTE_BIT, ScreenOverlayShader::getShader());
    ctx.dispatch(workgroups.width, workgroups.height, workgroups.depth);

    // Clear pending overlay after dispatch
    ctx.m_pendingScreenOverlay.reset();
  }

} // namespace fork_hooks
} // namespace dxvk
