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

// Performance mode's cache counters (documentation/PtPerformance.md, section "Counters"). They are words of the
// LightSamplingStats buffer, which every ray tracing pass binds, after the software micromap counters.
// The SHARC query adds to them while RaytraceArgs::ptPerformanceCounters is 1.

#include "rtx/pass/opacity_micromap/software_omm_shared.h"

#define PT_PERFORMANCE_COUNTER_CACHE_QUERIES   0u // first indirect hits at which the SHARC query ran
#define PT_PERFORMANCE_COUNTER_CACHE_HITS      1u // the first indirect hits the cache supplied light for
#define PT_PERFORMANCE_COUNTER_EXTRA_QUERIES   2u // later indirect hits at which the SHARC query ran, after a cache miss
#define PT_PERFORMANCE_COUNTER_EXTRA_HITS      3u // the later indirect hits the cache supplied light for
// Why the cache supplied no light at a first indirect hit. Each counted first query falls in at most one of them.
#define PT_PERFORMANCE_COUNTER_MISS_REJECTED   4u // refused by the reject test (emission above the limit, or a non-diffuse arrival)
#define PT_PERFORMANCE_COUNTER_MISS_TOO_CLOSE  5u // closer to the primary surface than the voxel diagonal
#define PT_PERFORMANCE_COUNTER_MISS_FOOTPRINT  6u // a glossy arrival whose lobe is still narrower than the voxel
#define PT_PERFORMANCE_COUNTER_MISS_NO_ENTRY   7u // the cell has no entry in the hash grid
#define PT_PERFORMANCE_COUNTER_MISS_UNFILLED   8u // the cell has an entry but no resolved samples yet
#define PT_PERFORMANCE_COUNTER_SKY             9u // first indirect rays that left the scene, which no query counts
#define PT_PERFORMANCE_COUNTER_SLOTS           10u
#define PT_PERFORMANCE_COUNTER_BASE            (SOFT_OMM_TRAVERSAL_COUNTER_BASE + SOFT_OMM_TRAVERSAL_COUNTER_SLOTS)
