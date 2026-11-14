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
#include "rtx/concept/surface_material/surface_material_hitgroup.h"

//!variant integrate_indirect_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=0


//!variant integrate_indirect_neeCache_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1


//!variant integrate_indirect_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=0


//!variant integrate_indirect_neeCache_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=1


//!variant integrate_indirect_pom_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1


//!variant integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1


//!variant integrate_indirect_pom_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1


//!variant integrate_indirect_neeCache_pom_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1


/* NRC */

//!variant integrate_indirect_nrc_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0


//!variant integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1


//!variant integrate_indirect_nrc_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0


//!variant integrate_indirect_nrc_neeCache_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1


//!variant integrate_indirect_nrc_pom_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1


//!variant integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1


//!variant integrate_indirect_nrc_pom_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1


//!variant integrate_indirect_nrc_neeCache_pom_material_rayportal_closestHit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1


// WBOIT

//!variant integrate_indirect_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_neeCache_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT


//!variant integrate_indirect_pom_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_pom_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_neeCache_pom_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

/* NRC */

//!variant integrate_indirect_nrc_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_nrc_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_nrc_neeCache_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT


//!variant integrate_indirect_nrc_pom_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_nrc_pom_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=0
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_nrc_neeCache_pom_material_rayportal_closestHit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT


/* SHARC */
// A token in the name means that feature is on.

//!variant integrate_indirect_sharc_query_closesthit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_query_closesthit_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_query_closesthit_pom.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1

//!variant integrate_indirect_sharc_query_closesthit_pom_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_query_closesthit_portals.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1

//!variant integrate_indirect_sharc_query_closesthit_portals_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_query_closesthit_portals_pom.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1

//!variant integrate_indirect_sharc_query_closesthit_portals_pom_wboit.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_ALL
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT

/* Slim and lean SHARC query hit shaders (see integrate_indirect.slang). */

//!variant integrate_indirect_sharc_query_closesthit_wboit_slim.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_query_closesthit_wboit_lean.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* Slim and lean SHARC query hit shaders that keep POM. */

//!variant integrate_indirect_sharc_query_closesthit_pom_wboit_slim.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_query_closesthit_pom_wboit_lean.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* Plain and NRC TraceRay hit shaders (NEE cache, WBOIT, no portals), with and without POM.
   Slim variants compile out the RIS and light BVH fallback of secondary NEE, and lean variants also compile out
   the translucent and thin-opaque handling of the secondary NEE visibility ray.
   The host binds them only on frames where the compiled-out branches cannot run. */

//!variant integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit_slim.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit_lean.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit_slim.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit_lean.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit_slim.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit_lean.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit_slim.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit_lean.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_NRC=1
//!>       NEE_CACHE_ENABLE=1
//!>       OPAQUE_MATERIAL_USE_POM=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* SHARC update hit shaders (deferred flush, WBOIT, no POM, no portals) for the TraceRay and SER update raygens
   in integrate_indirect.slang. There is one per propagation depth and slim level. */

//!variant integrate_indirect_sharc_update4_closesthit_wboit_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update4_closesthit_wboit_slim_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update4_closesthit_wboit_lean_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_closesthit_wboit_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT

//!variant integrate_indirect_sharc_update8_closesthit_wboit_slim_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1

//!variant integrate_indirect_sharc_update8_closesthit_wboit_lean_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_NEE_CACHE_ONLY=1
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

/* Lean SHARC hit shaders that keep the RIS and light BVH sampling of secondary NEE (_leanls, see
   integrate_indirect.slang). */

//!variant integrate_indirect_sharc_query_closesthit_wboit_leanls.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_QUERY=1
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update4_closesthit_wboit_leanls_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=4
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!variant integrate_indirect_sharc_update8_closesthit_wboit_leanls_df.rchit
//!>       SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK=SURFACE_MATERIAL_RESOLVE_TYPE_OPAQUE_TRANSLUCENT
//!>       ENABLE_SHARC=1
//!>       SHARC_UPDATE=1
//!>       SHARC_DEFERRED_FLUSH=1
//!>       SHARC_PROPAGATION_DEPTH=8
//!>       NEE_CACHE_ENABLE=1
//!>       UNORDERED_RESOLVE_WBOIT
//!>       INDIRECT_SLIM_LEAN_VISIBILITY=1

//!end-variants
#define ATMOSPHERE_AVAILABLE

#define RAY_PIPELINE
#define INTEGRATE_INDIRECT
#define RAB_HAS_PREVIOUS_GBUFFER 1
#define RAB_HAS_RTXDI_RESERVOIRS 1
#if ENABLE_SHARC && !defined(RAB_HAS_RTXDI_RESERVOIRS)
// Without these reservoirs a SHARC stage loads an empty reservoir and the NEE steal always fails.
// A failed steal skips the NEE cache and RIS fallback, so the hit gets no direct light.
#error "SHARC stages require RAB_HAS_RTXDI_RESERVOIRS."
#endif


// Note: ReSTIR GI Reservoirs are not available with NRC enabled, but 
// the usage of the reservoirs is gated on ReSTIR GI being enabled
#if !ENABLE_SHARC
// ReSTIR GI is a different integrate-indirect mode, so its reservoirs hold nothing under SHARC.
#define RAB_HAS_RESTIR_GI_RESERVOIRS 1
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

#include "rtx/algorithm/integrator_indirect.slangh"

[shader("closesthit")]
void main(inout PathState rayPayload : SV_RayPayload, in float2 barycentricCoordinates : SV_IntersectionAttributes)
{
  // Skips a hit that hardware micromaps would cull (rtx.alphaBake.softwareOmmResolveSkip).
  // It applies only with rtx.alphaBake.softwareOmmResolveSkipIndirect.
  if (softOmmClosestHitSkip(SOFT_OMM_RESOLVE_PASS_INDIRECT, rayPayload, barycentricCoordinates))
  {
    return;
  }

  // Construct a Ray Hit Info from the hit

  RAY_HIT_INFO_CREATE_TRACE_RAY_HIT(rayHitInfo);

  // Invoke the Integrate Path Vertex function and handle the payload

  IndirectPathTextures indirectPathTextures;

  indirectPathTextures.PrimaryWorldPositionWorldTriangleNormal = PrimaryWorldPositionWorldTriangleNormal;
  indirectPathTextures.IndirectRadianceHitDistance = IndirectRadianceHitDistance;

  integratePathVertex(indirectPathTextures, rayHitInfo, rayPayload);
}
