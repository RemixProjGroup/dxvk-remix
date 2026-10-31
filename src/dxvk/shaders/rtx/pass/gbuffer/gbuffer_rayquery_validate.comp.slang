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

/* rtx.frameFeatures.validate. The Full RayQuery G-buffer primary-ray kernel (gbuffer_rayquery.comp.slang) with
   FRAME_FEATURES_VALIDATE. Every primary vertex ORs what it used (thin opaque, thin film, portal, POM, diffusion-profile
   SSS, unordered interaction, legacy op, eye) into the frame feature seen bits (rtx/pass/frame_features_validate.h).
   Only the shipping feature sets of gbuffer_rayquery_slim.comp.slang (WBOIT) have it. Other frames run the plain Full
   kernel and log that the G-buffer was not validated. PSR passes keep the plain Full kernels. */

//!variant gbuffer_rayquery_psr_prepare_wboit_validate.comp
//!>       RAY_TRACING_PRIMARY_RAY ATMOSPHERE_AVAILABLE
//!>       GBUFFER_FEATURE_DLSSRR=1 GBUFFER_FEATURE_OBJECT_PICKING=0 GBUFFER_FEATURE_RAYTRACED_RENDER_TARGET=0 GBUFFER_FEATURE_STOCHASTIC_ALPHA=0
//!>       GBUFFER_FEATURE_PSR_REPLACEMENT=1 GBUFFER_FEATURE_INLINE_PSR_SAMPLING=0
//!>       UNORDERED_RESOLVE_WBOIT FRAME_FEATURES_VALIDATE=1

//!variant gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_validate.comp
//!>       RAY_TRACING_PRIMARY_RAY ATMOSPHERE_AVAILABLE
//!>       GBUFFER_FEATURE_DLSSRR=1 GBUFFER_FEATURE_OBJECT_PICKING=0 GBUFFER_FEATURE_RAYTRACED_RENDER_TARGET=0 GBUFFER_FEATURE_STOCHASTIC_ALPHA=1
//!>       GBUFFER_FEATURE_PSR_REPLACEMENT=1 GBUFFER_FEATURE_INLINE_PSR_SAMPLING=0
//!>       UNORDERED_RESOLVE_WBOIT FRAME_FEATURES_VALIDATE=1

//!variant gbuffer_rayquery_psr_inline_nrc_wboit_validate.comp
//!>       RAY_TRACING_PRIMARY_RAY ATMOSPHERE_AVAILABLE
//!>       GBUFFER_FEATURE_DLSSRR=1 GBUFFER_FEATURE_OBJECT_PICKING=0 GBUFFER_FEATURE_RAYTRACED_RENDER_TARGET=0 GBUFFER_FEATURE_STOCHASTIC_ALPHA=0
//!>       GBUFFER_FEATURE_PSR_REPLACEMENT=1 GBUFFER_FEATURE_INLINE_PSR_SAMPLING=1
//!>       ENABLE_NRC=1 UNORDERED_RESOLVE_WBOIT FRAME_FEATURES_VALIDATE=1

//!variant gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_validate.comp
//!>       RAY_TRACING_PRIMARY_RAY ATMOSPHERE_AVAILABLE
//!>       GBUFFER_FEATURE_DLSSRR=1 GBUFFER_FEATURE_OBJECT_PICKING=0 GBUFFER_FEATURE_RAYTRACED_RENDER_TARGET=0 GBUFFER_FEATURE_STOCHASTIC_ALPHA=1
//!>       GBUFFER_FEATURE_PSR_REPLACEMENT=1 GBUFFER_FEATURE_INLINE_PSR_SAMPLING=1
//!>       ENABLE_NRC=1 UNORDERED_RESOLVE_WBOIT FRAME_FEATURES_VALIDATE=1

//!end-variants

#include "rtx/pass/gbuffer/gbuffer_rayquery.comp.slang"
