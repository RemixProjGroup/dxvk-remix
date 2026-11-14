/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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

//!variant integrate_indirect_raygen_neeCache.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_raygen_ser_neeCache.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_rayquery_neeCache.comp
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_rayquery_raygen_neeCache.rgen
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_rayquery.comp
//!>       NEE_CACHE_ENABLE=0

//!variant integrate_indirect_rayquery_raygen.rgen
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=0

//!variant integrate_indirect_raygen.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=0

//!variant integrate_indirect_raygen_ser.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=0


/* NRC */

//!variant integrate_indirect_raygen_nrc_neeCache.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_raygen_ser_nrc_neeCache.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_rayquery_nrc_neeCache.comp
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_rayquery_raygen_nrc_neeCache.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_rayquery_nrc.comp
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0

//!variant integrate_indirect_rayquery_raygen_nrc.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0

//!variant integrate_indirect_raygen_nrc.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0

//!variant integrate_indirect_raygen_ser_nrc.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0


// WBOIT


//!variant integrate_indirect_raygen_neeCache_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_raygen_ser_neeCache_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_neeCache_wboit.comp
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_raygen_neeCache_wboit.rgen
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_wboit.comp
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_raygen_wboit.rgen
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_raygen_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_raygen_ser_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

/* NRC */

//!variant integrate_indirect_raygen_nrc_neeCache_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_raygen_ser_nrc_neeCache_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_nrc_neeCache_wboit.comp
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_raygen_nrc_neeCache_wboit.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_nrc_wboit.comp
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_rayquery_raygen_nrc_wboit.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_raygen_nrc_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_raygen_ser_nrc_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT


/* SHARC */
// Dedicated variants keep SHARC descriptors and payload state out of every other shader.
// The update stage traces inline RayQuery from a ray generation shader, except for the SER variants at the end.
// The query stage uses the same TraceRay hit and miss shaders as the ordinary indirect pass.

//!variant integrate_indirect_sharc_update4.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_update4_wboit.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update8.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_update8_wboit.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_query.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_query_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_query_ser.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_query_ser_wboit.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

/* Variants for rtx.foliageRays.fastRayMode = Omm2State. OMM_2_STATE_RAY_FLAG declares the opacity micromap
   capability so secondary segments can set the ForceOpacityMicromap2StateEXT ray flag.
   Other variants leave it out, so devices without VK_EXT_opacity_micromap never see the capability.
   Only the SHARC update and query and the non-NRC SER TraceRay paths have these variants. */

//!variant integrate_indirect_raygen_ser_neeCache_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=1
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_raygen_ser_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=0
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_raygen_ser_neeCache_wboit_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_raygen_ser_wboit_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_update4_omm2.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_update4_wboit_omm2.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_update8_omm2.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_update8_wboit_omm2.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_query_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_query_wboit_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_query_ser_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OMM_2_STATE_RAY_FLAG=1

//!variant integrate_indirect_sharc_query_ser_wboit_omm2.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       OMM_2_STATE_RAY_FLAG=1

/* Slim variants compile out POM and the RIS NEE fallback, and lean variants also compile out the translucent
   and thin-opaque NEE visibility handling. They are WBOIT only, without OMM 2-state, and the host binds them
   only on frames where the compiled-out branches cannot run. */

//!variant integrate_indirect_sharc_update4_wboit_slim.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update4_wboit_lean.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_wboit_slim.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update8_wboit_lean.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_query_wboit_slim.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_query_wboit_lean.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_query_ser_wboit_slim.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_query_ser_wboit_lean.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* Slim and lean SHARC update variants that keep POM, for frames that use it.
   The update traces inline, so the whole path is in this raygen. */

//!variant integrate_indirect_sharc_update4_wboit_slimpom.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update4_wboit_leanpom.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_wboit_slimpom.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update8_wboit_leanpom.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* A TraceRay raygen contains only the POM part of the slim features, because the NEE and visibility work runs
   in the closest hit. Non-SHARC slim levels therefore need one POM-less raygen per mode,
   and levels that keep POM use the base raygen. */

//!variant integrate_indirect_raygen_neeCache_wboit_nopom.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM

//!variant integrate_indirect_raygen_ser_neeCache_wboit_nopom.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM

//!variant integrate_indirect_raygen_nrc_neeCache_wboit_nopom.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM

//!variant integrate_indirect_raygen_ser_nrc_neeCache_wboit_nopom.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM

/* Deferred-flush variants for rtx.sharc.deferredFlush keep the path vertices of every WBOIT SHARC update variant
   in the per-tile buffer instead of registers, and sharc_update_flush.comp deposits them.
   Non-WBOIT and OMM 2-state updates keep the inline flush. */

//!variant integrate_indirect_sharc_update4_wboit_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update8_wboit_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update4_wboit_slim_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update4_wboit_lean_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_wboit_slim_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update8_wboit_lean_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update4_wboit_slimpom_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update4_wboit_leanpom_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_wboit_slimpom_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update8_wboit_leanpom_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* The deferred-flush WBOIT update (Full, Slim, Lean) through TraceRay with shader execution reordering.
   The hit and miss shaders run integratePathVertex with the update payload
   (integrate_indirect_sharc_update{4,8}_closesthit_wboit_df*, _miss_wboit_df).
   They are bound only with SER available, on frames without POM, portals or OMM 2-state.
   Every other frame uses the inline RayQuery update. */

//!variant integrate_indirect_sharc_update4_ser_wboit_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update4_ser_wboit_slim_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update4_ser_wboit_lean_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_ser_wboit_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update8_ser_wboit_slim_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update8_ser_wboit_lean_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* Lean SHARC variants that keep the RIS and light BVH sampling of secondary NEE (_leanls): POM and the
   translucent and thin-opaque NEE visibility handling are compiled out. The host binds them on frames where the
   NEE cache does not take every secondary light sample, such as performance mode, which runs without the NEE cache. */

//!variant integrate_indirect_sharc_update4_wboit_leanls.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update4_wboit_leanls_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update4_ser_wboit_leanls_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_wboit_leanls.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_wboit_leanls_df.rgen
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_ser_wboit_leanls_df.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_query_wboit_leanls.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_query_ser_wboit_leanls.rgen
//!>       RT_TRACE_RAY
//!>       RAY_PIPELINE
//!>       RT_SHADER_EXECUTION_REORDERING
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* SHARC update and query as compute shaders with inline RayQuery, dispatched without a ray tracing pipeline.
   The host binds them where rtx.renderPassIntegrateIndirectRaytraceMode is RayQuery, as for the other indirect
   compute variants. They have no slim levels and no OMM 2-state ray flag. The update covers the inline flush and,
   for WBOIT, the deferred flush. */

//!variant integrate_indirect_sharc_update4_rayquery.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_update4_rayquery_wboit.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update4_rayquery_wboit_df.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update8_rayquery.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_update8_rayquery_wboit.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update8_rayquery_wboit_df.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_query_rayquery.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_query_rayquery_wboit.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

/* Lean compute variants (WBOIT) with the RIS and light BVH sampling of secondary NEE kept: the SHARC update and
   query (_leanls) and the indirect integration without the NEE cache (_lean). POM and the translucent and
   thin-opaque NEE visibility handling are compiled out. */

//!variant integrate_indirect_sharc_update4_rayquery_wboit_leanls.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update4_rayquery_wboit_leanls_df.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_rayquery_wboit_leanls.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_rayquery_wboit_leanls_df.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_query_rayquery_wboit_leanls.comp
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_rayquery_wboit_lean.comp
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NO_POM
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!end-variants
#define ATMOSPHERE_AVAILABLE

#define INTEGRATE_INDIRECT
#define RAB_HAS_PREVIOUS_GBUFFER 1
// INDIRECT_SLIM_NO_POM variants are bound only while POM is off.
#ifndef INDIRECT_SLIM_NO_POM
#define OPAQUE_MATERIAL_USE_POM 1
#endif
#if ENABLE_SHARC
// The cache update depends on secondary NEE sample stealing. Without it a cell, which is a mean,
// receives far noisier values that do not average out per pixel.
#define RAB_HAS_RTXDI_RESERVOIRS 1
#endif

// Note: Opacity lighting approximation used in the indirect integration pass due to particle lighting being somewhat
// important in reflections off shiny surfaces (as without this it can make areas with many particles look weird in reflections).
#define RESOLVE_OPACITY_LIGHTING_APPROXIMATION 1
#define RESOLVER_USE_VOLUMETRIC_ATTENUATION 1
// Slot of this pass in the rtx.alphaBake.softwareOmmCounters counters.
#define SOFT_OMM_COUNTER_PASS SOFT_OMM_COUNTER_PASS_INDIRECT

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/common_bindings.slangh"
#include "rtx/pass/integrate/integrate_indirect_binding_indices.h"
#include "rtx/pass/integrate/integrate_indirect_bindings.slangh"
// Note: Needed for declaration of integration ray payload structure
#include "rtx/algorithm/integrator.slangh"

#include "rtx/pass/integrate/integrate_indirect.slangh"

#ifdef RAY_PIPELINE
[shader("raygeneration")]
void main()
{
  integrate_indirect_pass(DispatchRaysIndex().xy);
}
#else
[shader("compute")]
[numthreads(16, 8, 1)]
void main(uint2 dispatchThreadId : SV_DispatchThreadID)
{
  integrate_indirect_pass(dispatchThreadId);
}
#endif
