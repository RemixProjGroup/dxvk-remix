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
#pragma once

// DLSS-NR ("Neural Rendering" / Neural Uplift, the DLSS 5 generation) NGX definitions.
//
// NVIDIA does not ship a public SDK header for this feature yet, so the names below were
// recovered from the shipping snippet rather than guessed: every NVSDK_NGX_Parameter_DLSSNR_*
// string in this file was confirmed to exist verbatim in nvngx_dlssnr.dll 310.8.0 (Streamline
// 2.13). NGX silently ignores parameters it does not recognise - a misspelled name does not
// fail, it just does nothing - so anything not confirmed against the binary is deliberately
// absent here rather than defined on a guess.
//
// Names that appear in circulation but are NOT in the snippet, and so are intentionally not
// defined: DLSSNR.Preset (the real one is DLSSNR.Hint.Render.Preset), DLSSNR.AutoSkinMask
// (the real one is DLSSNR.UseAutoMask), DLSSNR.ColourEncode, DLSSNR.EncodeWhite, and the
// DLSSNR.Available / NeedsUpdatedDriver / MinDriverVersion* / FeatureInitResult capability
// family - the snippet exposes no availability parameters of its own, which is why
// NGXNeuralUpliftContext probes the feature by trying to create it instead of querying.
// DLSSNR.GlobalToneStrength exists only in sl.dlss_nr.dll: it is a Streamline plugin option,
// not an NGX parameter, and is not settable on this path.

// Self-contained on purpose: only parameter-name strings and one enum, so this pulls in no
// NGX header and stays usable from both the x86_64 and arm64 SDK include paths.

// The NVSDK_NGX_Feature enum value for DLSS-NR. Confirmed as 18 (0x12) by disassembly: the
// snippet materialises it in NVSDK_NGX_VULKAN_GetFeatureRequirements and in its own
// CreateFeature, and NVIDIA's sl.dlss_nr.dll passes the same constant. rtx.neuralUplift.featureId
// still overrides it at runtime, in case a later snippet renumbers.
#define kNgxFeatureDlssNrDefault 18

// Hardware and driver floor, from the snippet's own checks: NV GPU architecture 0x1B0 or newer -
// Blackwell. Turing through Ada (0x140-0x1A0) are each explicitly rejected with
// "Unsupported GPU architecture 0x%x, minimum required 0x1b0". Minimum driver 570.
//
// It also requires Vulkan device extensions VK_NVX_binary_import, VK_NVX_image_view_handle,
// VK_EXT_buffer_device_address and VK_KHR_push_descriptor, plus the timelineSemaphore,
// descriptorIndexing and bufferDeviceAddress 1.2 features. Feature creation is what reports a
// shortfall here; there is no capability parameter to ask in advance.

// --- Feature creation -------------------------------------------------------------------
#define NVSDK_NGX_Parameter_DLSSNR_Width                "DLSSNR.Width"
#define NVSDK_NGX_Parameter_DLSSNR_Height               "DLSSNR.Height"
#define NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset   "DLSSNR.Hint.Render.Preset"
#define NVSDK_NGX_Parameter_DLSSNR_Enabled              "DLSSNR.Enabled"
#define NVSDK_NGX_Parameter_DLSSNR_DepthInverted        "DLSSNR.DepthInverted"
#define NVSDK_NGX_Parameter_DLSSNR_ScalingRatio         "DLSSNR.ScalingRatio"

// --- Per-evaluation inputs / outputs ----------------------------------------------------
#define NVSDK_NGX_Parameter_DLSSNR_Color                "DLSSNR.Color"
#define NVSDK_NGX_Parameter_DLSSNR_Output               "DLSSNR.Output"
#define NVSDK_NGX_Parameter_DLSSNR_Depth                "DLSSNR.Depth"
#define NVSDK_NGX_Parameter_DLSSNR_MVec                 "DLSSNR.MVec"
#define NVSDK_NGX_Parameter_DLSSNR_MVecScaleX           "DLSSNR.MVecScaleX"
#define NVSDK_NGX_Parameter_DLSSNR_MVecScaleY           "DLSSNR.MVecScaleY"
#define NVSDK_NGX_Parameter_DLSSNR_Reset                "DLSSNR.Reset"

// Optional inputs the snippet accepts, none of them fed here. DLSSNR.UI / DLSSNR.UIAlpha /
// DLSSNR.UICorrection let it separate a composited HUD from the scene, and DLSSNR.ControlMask
// masks the effect per pixel. The path tracer runs this pass before the game's UI is drawn, so
// there is no HUD in the colour input to separate - UICorrection is set to 0 and the rest stay
// unbound.
#define NVSDK_NGX_Parameter_DLSSNR_UI                   "DLSSNR.UI"
#define NVSDK_NGX_Parameter_DLSSNR_UIAlpha              "DLSSNR.UIAlpha"
#define NVSDK_NGX_Parameter_DLSSNR_UICorrection         "DLSSNR.UICorrection"
#define NVSDK_NGX_Parameter_DLSSNR_Backbuffer           "DLSSNR.Backbuffer"
#define NVSDK_NGX_Parameter_DLSSNR_ControlMask          "DLSSNR.ControlMask"

// --- Effect controls --------------------------------------------------------------------
#define NVSDK_NGX_Parameter_DLSSNR_Style                    "DLSSNR.Style"
#define NVSDK_NGX_Parameter_DLSSNR_Intensity                "DLSSNR.Intensity"
#define NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength        "DLSSNR.LocalToneStrength"
#define NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength   "DLSSNR.LocalStructureStrength"
#define NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength    "DLSSNR.SkinStructureStrength"
#define NVSDK_NGX_Parameter_DLSSNR_UseAutoMask              "DLSSNR.UseAutoMask"

// --- Subrects ---------------------------------------------------------------------------
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX    "DLSSNR.ColorSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY    "DLSSNR.ColorSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth    "DLSSNR.ColorSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight   "DLSSNR.ColorSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX   "DLSSNR.OutputSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY   "DLSSNR.OutputSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth   "DLSSNR.OutputSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight  "DLSSNR.OutputSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX     "DLSSNR.MVecSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY     "DLSSNR.MVecSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth     "DLSSNR.MVecSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight    "DLSSNR.MVecSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX    "DLSSNR.DepthSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY    "DLSSNR.DepthSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth    "DLSSNR.DepthSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight   "DLSSNR.DepthSubrectHeight"

// Model selection hint, passed at feature creation. Default (0) leaves the choice to the
// snippet; 1-7 select a specific shipped model.
typedef enum NVSDK_NGX_DLSSNR_Hint_Render_Preset {
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_Default = 0,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_1       = 1,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_2       = 2,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_3       = 3,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_4       = 4,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_5       = 5,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_6       = 6,
  NVSDK_NGX_DLSSNR_Hint_Render_Preset_7       = 7,
} NVSDK_NGX_DLSSNR_Hint_Render_Preset;
