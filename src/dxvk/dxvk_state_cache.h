/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "dxvk_state_cache_types.h"
// NV-DXVK start: compile rt shaders on shader compilation threads
#include "dxvk_raytracing.h"
// NV-DXVK end

namespace dxvk {

  class DxvkDevice;

  /**
   * \brief State cache
   * 
   * The shader state cache stores state vectors and
   * render pass formats of all pipelines used in a
   * game, which allows DXVK to compile them ahead
   * of time instead of compiling them on the first
   * draw.
   */
  class DxvkStateCache : public RcObject {

  public:

    DxvkStateCache(
      const DxvkDevice*           device,
            DxvkPipelineManager*  pipeManager,
            DxvkRenderPassPool*   passManager);
    
    ~DxvkStateCache();

    /**
     * Adds a graphics pipeline to the cache
     * 
     * If the pipeline is not already cached, this
     * will write a new pipeline to the cache file.
     * \param [in] shaders Shader keys
     * \param [in] state Graphics pipeline state
     * \param [in] format Render pass format
     */
    void addGraphicsPipeline(
      const DxvkStateCacheKey&              shaders,
      const DxvkGraphicsPipelineStateInfo&  state,
      const DxvkRenderPassFormat&           format);

    /**
     * Adds a compute pipeline to the cache
     * 
     * If the pipeline is not already cached, this
     * will write a new pipeline to the cache file.
     * \param [in] shaders Shader keys
     * \param [in] state Compute pipeline state
     */
    void addComputePipeline(
      const DxvkStateCacheKey&              shaders,
      const DxvkComputePipelineStateInfo&   state);

    /**
     * \brief Registers a newly compiled shader
     * 
     * Makes the shader available to the pipeline
     * compiler, and starts compiling all pipelines
     * for which all shaders become available.
     * \param [in] shader The shader to add
     */
    void registerShader(
      const Rc<DxvkShader>&                 shader,
// NV-DXVK start
      bool                                  isRemixShader = false);
// NV-DXVK end

    // NV-DXVK start: compile raytracing shaders on shader compilation threads
    /**
     * \brief Registers a set of raytracing shaders
     * 
     * Makes the shader available to the pipeline
     * compiler, and starts compiling all pipelines
     * for which all shaders become available.
     * \param [in] shaders The shaders to add
     */
    void registerRaytracingShaders(
      const DxvkRaytracingPipelineShaders& shaders);
    // NV-DXVK end
    
    /**
     * \brief Explicitly stops worker threads
     */
    void stopWorkerThreads();

    /**
     * \brief Checks whether compiler threads are busy
     * \returns \c true if we're compiling shaders
     */
    bool isCompilingShaders() {
      return m_workerBusy.load() > 0;
    }

// NV-DXVK start
    uint32_t remixShaderCompilationCount() const {
      return m_workerCompilingRemixShaders.load();
    }

    // Remix pipelines a worker has been creating for longer than kSlowCompileMs. A pipeline served from the
    // pipeline cache (or the driver's) takes a few ms, so these are real compiles; queued pipelines and cache hits
    // do not count. VK_PIPELINE_CREATION_FEEDBACK only reports a hit after the creation, too late for an in-flight
    // count, so the HUD uses this time test while the logs and the summary use the feedback.
    static constexpr int64_t kSlowCompileMs = 50;
    uint32_t remixShaderSlowCompileCount() const;
// NV-DXVK end

    // Remix pipelines queued for the workers since the device was created, finished or not.
    uint32_t remixShaderQueuedCount() const {
      return m_remixItemsQueued.load();
    }

    // Pipelines of shaders marked with DxvkShader::markBackgroundCompile that are queued or compiling.
    // The Remix counts above leave them out.
    uint32_t backgroundShaderCompilationCount() const {
      return m_workerCompilingBackgroundShaders.load();
    }

  private:

    using WriterItem = DxvkStateCacheEntry;

    struct WorkerItem {
      DxvkGraphicsPipelineShaders gp;
      DxvkComputePipelineShaders  cp;

      // NV-DXVK start: compile rt shaders on shader compilation threads
      DxvkRaytracingPipelineShaders rt;
      // NV-DXVK end
      // NV-DXVK start
      bool isRemixShader = false;
      // NV-DXVK end
      // A Remix compute shader marked with DxvkShader::markBackgroundCompile.
      bool isBackground = false;

      // NV-DXVK start: do not compile same shader multiple times
      size_t hash() const {
        // raytracing shader group hash is NOT guaranteed to be zero
        if (!rt.groups.empty()) {
          return rt.hash();
        }
        // note that one of these is guaranteed to be zero
        return cp.hash() ^ gp.hash();
      }
      // NV-DXVK end

      // The key of m_workerItemsInFlight. It adds the shader objects to hash(), because two shader objects with
      // identical SPIR-V (two variant names that compile to one binary) have separate pipeline objects, and each
      // must be compiled.
      size_t inFlightKey() const {
        DxvkHashState state;
        state.add(hash());
        state.add(reinterpret_cast<size_t>(cp.cs.ptr()));
        for (const DxvkShader* shader : { gp.vs.ptr(), gp.tcs.ptr(), gp.tes.ptr(), gp.gs.ptr(), gp.fs.ptr() })
          state.add(reinterpret_cast<size_t>(shader));
        for (const auto& group : rt.groups) {
          state.add(reinterpret_cast<size_t>(group.generalShader.ptr()));
          state.add(reinterpret_cast<size_t>(group.closestHitShader.ptr()));
          state.add(reinterpret_cast<size_t>(group.anyHitShader.ptr()));
          state.add(reinterpret_cast<size_t>(group.intersectionShader.ptr()));
        }
        return state;
      }
    };

    DxvkPipelineManager*              m_pipeManager;
    DxvkRenderPassPool*               m_passManager;

    std::vector<DxvkStateCacheEntry>  m_entries;
    std::atomic<bool>                 m_stopThreads = { false };

    dxvk::mutex                       m_entryLock;

    std::unordered_multimap<
      DxvkStateCacheKey, size_t,
      DxvkHash, DxvkEq> m_entryMap;

    std::unordered_multimap<
      DxvkShaderKey, DxvkStateCacheKey,
      DxvkHash, DxvkEq> m_pipelineMap;
    
    std::unordered_map<
      DxvkShaderKey, Rc<DxvkShader>,
      DxvkHash, DxvkEq> m_shaderMap;

    dxvk::mutex                       m_workerLock;
    dxvk::condition_variable          m_workerCond;
    std::queue<WorkerItem>            m_workerQueue;
    // NV-DXVK start: do not compile same shader multiple times
    std::unordered_set<size_t>        m_workerItemsInFlight;  // stores hashes for work items in the queue
    // NV-DXVK end
    std::atomic<uint32_t>             m_workerBusy;
    // NV-DXVK start
    std::atomic<uint32_t>             m_workerCompilingRemixShaders;
    // NV-DXVK end
    std::atomic<uint32_t>             m_remixItemsQueued = { 0 };
    std::atomic<uint32_t>             m_workerCompilingBackgroundShaders = { 0 };
    std::vector<dxvk::thread>         m_workerThreads;
    // Per worker, the steady_clock time (ms) its current Remix pipeline started, 0 when idle.
    std::unique_ptr<std::atomic<int64_t>[]> m_workerRemixStartMs;
    uint32_t                          m_workerCount = 0;

    dxvk::mutex                       m_writerLock;
    dxvk::condition_variable          m_writerCond;
    std::queue<WriterItem>            m_writerQueue;
    dxvk::thread                      m_writerThread;

    DxvkShaderKey getShaderKey(
      const Rc<DxvkShader>&           shader) const;

    bool getShaderByKey(
      const DxvkShaderKey&            key,
            Rc<DxvkShader>&           shader) const;
    
    void mapPipelineToEntry(
      const DxvkStateCacheKey&        key,
            size_t                    entryId);
    
    void mapShaderToPipeline(
      const DxvkShaderKey&            shader,
      const DxvkStateCacheKey&        key);

    void compilePipelines(
      const WorkerItem&               item);

    bool readCacheFile();

    bool readCacheHeader(
            std::istream&             stream,
            DxvkStateCacheHeader&     header) const;

    bool readCacheEntryV7(
            uint32_t                  version,
            std::istream&             stream, 
            DxvkStateCacheEntry&      entry) const;
    
    bool readCacheEntry(
            uint32_t                  version,
            std::istream&             stream, 
            DxvkStateCacheEntry&      entry) const;
    
    void writeCacheEntry(
            std::ostream&             stream, 
            DxvkStateCacheEntry&      entry) const;
    
    bool convertEntryV2(
            DxvkStateCacheEntryV4&    entry) const;
    
    bool convertEntryV4(
      const DxvkStateCacheEntryV4&    in,
            DxvkStateCacheEntryV6&    out) const;
    
    bool convertEntryV5(
      const DxvkStateCacheEntryV5&    in,
            DxvkStateCacheEntryV6&    out) const;
    
    bool convertEntryV6(
      const DxvkStateCacheEntryV6&    in,
            DxvkStateCacheEntry&      out) const;
    
    void workerFunc(uint32_t workerIndex);

    void writerFunc();

    std::wstring getCacheFileName() const;
    
    std::string getCacheDir() const;

    static uint8_t packImageLayout(
            VkImageLayout             layout);

    static VkImageLayout unpackImageLayout(
            uint8_t                   layout);

    static bool validateRenderPassFormat(
      const DxvkRenderPassFormat&     format);

  };

}
