// src/dxvk/rtx_render/rtx_fork_overlay.cpp
//
// Fork-owned file. Contains the implementations of fork_hooks:: functions
// for the GameOverlay / ImGUI overlay path, lifted from
// rtx_overlay_window.cpp during the 2026-04-18 fork touchpoint-pattern refactor.
//
// See docs/fork-touchpoints.md for the full fork-hooks catalogue.
//
// NOTE: overlayKeyboardForward accesses GameOverlay::m_hwnd, which is a
// private member. This file requires that GameOverlay declare
// fork_hooks::overlayKeyboardForward as a friend — see rtx_overlay_window.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rtx_fork_hooks.h"

#include "rtx_overlay_window.h"       // GameOverlay
#include "imgui/imgui_impl_win32.h"   // ImGui_ImplWin32_WndProcHandler

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dxvk {
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

} // namespace fork_hooks
} // namespace dxvk
