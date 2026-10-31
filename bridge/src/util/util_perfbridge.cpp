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
#include "util_perfbridge.h"

#include "util_common.h"
#include "util_commands.h"
#include "log/log.h"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

namespace bridge_util {
  namespace perf {
    std::atomic<uint32_t> g_intervalFrames { 0 };

    static_assert(Commands::IDirect3DQuery9_GetData < kMaxCommands, "perf::kMaxCommands is too small for the command enum");

    namespace {
#if defined(REMIX_BRIDGE_CLIENT)
      constexpr char kSide[] = "client";
#else
      constexpr char kSide[] = "server";
#endif
      // Frames between the always-on async create failure reports.
      constexpr uint32_t kCreateFailureReportFrames = 300;
      // Individually logged async create failures.
      constexpr uint64_t kCreateFailuresLoggedIndividually = 20;
      constexpr size_t kTopCommands = 12;
      constexpr size_t kTopBlocking = 8;
      // Seconds between the default [PerfBridge] lines.
      constexpr double kShortLineSeconds = 30.0;

      // Static storage: zero-initialised before any constructor runs.
      std::atomic<uint32_t> s_cmdCount[kMaxCommands];
      std::atomic<uint64_t> s_cmdTicksByType[kMaxCommands];
      std::atomic<uint32_t> s_respWaitCount[kMaxCommands];
      std::atomic<uint64_t> s_respWaitTicks[kMaxCommands];
      std::atomic<uint32_t> s_droppedCount[kMaxCommands];
      std::atomic<uint64_t> s_records;
      std::atomic<uint64_t> s_commands;
      std::atomic<uint64_t> s_dataWords;
      std::atomic<uint64_t> s_cmdTicks;
      std::atomic<uint64_t> s_uploadBytes;
      std::atomic<uint64_t> s_presentWaits;
      std::atomic<uint64_t> s_presentWaitTicks;
      std::atomic<uint32_t> s_lastSent;
      std::atomic<uint64_t> s_queryCalls[size_t(QueryPath::Count)];
      std::atomic<uint64_t> s_querySignalled[size_t(QueryPath::Count)];
      std::atomic<uint64_t> s_asyncCreateFailures;

      struct QueueSnapshot {
        uint64_t waits = 0, spinTicks = 0, blockTicks = 0, blocks = 0, wakesSent = 0;

        static QueueSnapshot take(const QueueWaitStats& s) {
          QueueSnapshot q;
          q.waits = s.waits.load(std::memory_order_relaxed);
          q.spinTicks = s.spinTicks.load(std::memory_order_relaxed);
          q.blockTicks = s.blockTicks.load(std::memory_order_relaxed);
          q.blocks = s.blocks.load(std::memory_order_relaxed);
          q.wakesSent = s.wakesSent.load(std::memory_order_relaxed);
          return q;
        }

        QueueSnapshot operator-(const QueueSnapshot& o) const {
          QueueSnapshot d;
          d.waits = waits - o.waits;
          d.spinTicks = spinTicks - o.spinTicks;
          d.blockTicks = blockTicks - o.blockTicks;
          d.blocks = blocks - o.blocks;
          d.wakesSent = wakesSent - o.wakesSent;
          return d;
        }
      };

      // The counters at one moment. The counters are never reset: each line subtracts the totals taken at its start.
      // The unsigned differences stay right across a wrap of a 32-bit counter.
      struct Totals {
        uint32_t cmdCount[kMaxCommands];
        uint64_t cmdTicksByType[kMaxCommands];
        uint32_t respWaitCount[kMaxCommands];
        uint64_t respWaitTicks[kMaxCommands];
        uint32_t droppedCount[kMaxCommands];
        uint64_t records, commands, dataWords, cmdTicks, uploadBytes, presentWaits, presentWaitTicks;
        uint64_t queryCalls[size_t(QueryPath::Count)];
        uint64_t querySignalled[size_t(QueryPath::Count)];
        QueueSnapshot writerFull, readerEmpty;

        void take(const QueueWaitStats& writer, const QueueWaitStats& reader) {
          for (uint32_t i = 0; i < kMaxCommands; ++i) {
            cmdCount[i] = s_cmdCount[i].load(std::memory_order_relaxed);
            cmdTicksByType[i] = s_cmdTicksByType[i].load(std::memory_order_relaxed);
            respWaitCount[i] = s_respWaitCount[i].load(std::memory_order_relaxed);
            respWaitTicks[i] = s_respWaitTicks[i].load(std::memory_order_relaxed);
            droppedCount[i] = s_droppedCount[i].load(std::memory_order_relaxed);
          }
          records = s_records.load(std::memory_order_relaxed);
          commands = s_commands.load(std::memory_order_relaxed);
          dataWords = s_dataWords.load(std::memory_order_relaxed);
          cmdTicks = s_cmdTicks.load(std::memory_order_relaxed);
          uploadBytes = s_uploadBytes.load(std::memory_order_relaxed);
          presentWaits = s_presentWaits.load(std::memory_order_relaxed);
          presentWaitTicks = s_presentWaitTicks.load(std::memory_order_relaxed);
          for (size_t i = 0; i < size_t(QueryPath::Count); ++i) {
            queryCalls[i] = s_queryCalls[i].load(std::memory_order_relaxed);
            querySignalled[i] = s_querySignalled[i].load(std::memory_order_relaxed);
          }
          writerFull = QueueSnapshot::take(writer);
          readerEmpty = QueueSnapshot::take(reader);
        }

        // Sets this to now - base.
        void diff(const Totals& now, const Totals& base) {
          for (uint32_t i = 0; i < kMaxCommands; ++i) {
            cmdCount[i] = now.cmdCount[i] - base.cmdCount[i];
            cmdTicksByType[i] = now.cmdTicksByType[i] - base.cmdTicksByType[i];
            respWaitCount[i] = now.respWaitCount[i] - base.respWaitCount[i];
            respWaitTicks[i] = now.respWaitTicks[i] - base.respWaitTicks[i];
            droppedCount[i] = now.droppedCount[i] - base.droppedCount[i];
          }
          records = now.records - base.records;
          commands = now.commands - base.commands;
          dataWords = now.dataWords - base.dataWords;
          cmdTicks = now.cmdTicks - base.cmdTicks;
          uploadBytes = now.uploadBytes - base.uploadBytes;
          presentWaits = now.presentWaits - base.presentWaits;
          presentWaitTicks = now.presentWaitTicks - base.presentWaitTicks;
          for (size_t i = 0; i < size_t(QueryPath::Count); ++i) {
            queryCalls[i] = now.queryCalls[i] - base.queryCalls[i];
            querySignalled[i] = now.querySignalled[i] - base.querySignalled[i];
          }
          writerFull = now.writerFull - base.writerFull;
          readerEmpty = now.readerEmpty - base.readerEmpty;
        }
      };

      // One line's interval: the 30 s short line, or the perfLogIntervalFrames breakdown. Presenting thread only.
      struct Interval {
        bool started = false;
        uint32_t frames = 0;
        int64_t start = 0;
        Totals base;
        bool cpuValid = false;
        DWORD cpuThreadId = 0;
        uint64_t cpuCycles = 0;
        uint64_t tscStart = 0;
      };

      // Presenting thread only. Static: the totals are too big for the game thread's stack.
      Interval s_short;
      Interval s_full;
      Totals s_now;
      Totals s_delta;
      uint32_t s_createReportFrames = 0;
      uint64_t s_createFailuresReported = 0;

      double ticksPerUs() {
        static const double value = [] {
          LARGE_INTEGER f;
          QueryPerformanceFrequency(&f);
          return double(f.QuadPart) / 1e6;
        }();
        return value;
      }

      double toUs(uint64_t ticks) {
        return double(ticks) / ticksPerUs();
      }

      // The calling thread's CPU cycles. GetThreadTimes is sampled at the scheduler tick and misses short bursts, so
      // the cycle counter is used instead. Windows reports thread cycles in time stamp counter units.
      bool threadCycles(uint64_t& out) {
        ULONG64 cycles = 0;
        if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
          return false;
        }
        out = cycles;
        return true;
      }

      uint64_t tsc() {
#if defined(_M_IX86) || defined(_M_X64)
        return __rdtsc();
#else
        return 0;
#endif
      }

      // "IDirect3DDevice9Ex_SetRenderState" -> "Device9Ex_SetRenderState".
      std::string shortName(uint32_t command) {
        std::string name = Commands::toString(static_cast<Commands::D3D9Command>(command));
        static constexpr char kPrefix[] = "IDirect3D";
        if (name.rfind(kPrefix, 0) == 0) {
          name = name.substr(sizeof(kPrefix) - 1);
        }
        return name;
      }

      // Adds to a counter. The server counts on its device thread only, so a relaxed load and store replace the
      // locked add there. The client counts on every thread that calls into the device.
      template<typename T, typename V>
      void bump(std::atomic<T>& counter, V value) {
#if defined(REMIX_BRIDGE_CLIENT)
        counter.fetch_add(T(value), std::memory_order_relaxed);
#else
        counter.store(counter.load(std::memory_order_relaxed) + T(value), std::memory_order_relaxed);
#endif
      }

      bool isPresent(uint32_t command) {
        return command == Commands::IDirect3DDevice9Ex_Present || command == Commands::IDirect3DDevice9Ex_PresentEx ||
               command == Commands::IDirect3DSwapChain9_Present;
      }

      enum Class { kSetters, kDraws, kUploads, kLocks, kCreates, kDestroys, kGets, kOther, kClassCount };
      constexpr const char* kClassNames[kClassCount] = { "setters", "draws", "uploads", "locks", "creates", "destroys", "gets", "other" };

      // By the method name after the interface prefix ("..._SetRenderState" -> setters).
      Class classify(uint32_t command) {
        const std::string name = Commands::toString(static_cast<Commands::D3D9Command>(command));
        const size_t sep = name.find('_');
        const std::string method = (sep == std::string::npos) ? name : name.substr(sep + 1);
        const auto starts = [&method](const char* prefix) { return method.rfind(prefix, 0) == 0; };
        if (starts("Set")) return kSetters;
        if (starts("Draw")) return kDraws;
        if (starts("Unlock") || starts("UpdateSurface") || starts("UpdateTexture")) return kUploads;
        if (starts("Lock")) return kLocks;
        if (starts("Create")) return kCreates;
        if (starts("Destroy")) return kDestroys;
        if (starts("Get")) return kGets;
        return kOther;
      }

      void begin(Interval& iv, int64_t t) {
        iv.started = true;
        iv.frames = 0;
        iv.start = t;
        iv.base = s_now;
        uint64_t cpu = 0;
        iv.cpuValid = threadCycles(cpu);
        iv.cpuCycles = cpu;
        iv.tscStart = tsc();
        iv.cpuThreadId = GetCurrentThreadId();
      }

      // The presenting thread's CPU time since the interval began, in milliseconds. The cycles are converted with the
      // time stamp counter rate measured against QPC over the same interval. False when there is no time stamp counter.
      bool cpuMsSince(const Interval& iv, int64_t t, double& out) {
        uint64_t cpu = 0;
        const uint64_t tscEnd = tsc();
        if (!iv.cpuValid || !threadCycles(cpu) || GetCurrentThreadId() != iv.cpuThreadId || cpu < iv.cpuCycles ||
            tscEnd <= iv.tscStart || t <= iv.start) {
          return false;
        }
        const double tscPerUs = double(tscEnd - iv.tscStart) / toUs(uint64_t(t - iv.start));
        out = double(cpu - iv.cpuCycles) / tscPerUs / 1e3;
        return true;
      }

      void reportAsyncCreateFailures() {
        if (++s_createReportFrames < kCreateFailureReportFrames) {
          return;
        }
        s_createReportFrames = 0;
        const uint64_t total = s_asyncCreateFailures.load(std::memory_order_relaxed);
        if (total == s_createFailuresReported) {
          return;
        }
        std::ostringstream ss;
        ss << "[Bridge] async creates: " << (total - s_createFailuresReported) << " create call(s) failed on the "
           << "server in the last " << kCreateFailureReportFrames << " frames (" << total << " in total). The game was "
           << "told D3D_OK for each; buffer and surface uploads to those objects are skipped. Set sendCreateFunctionServerResponses = True "
           << "in bridge.conf to make creates synchronous again.";
        Logger::warn(ss.str());
        s_createFailuresReported = total;
      }

      // The default line: where the presenting thread's frame went.
      void logShort(const Totals& d, const Interval& iv, int64_t t) {
        const double perFrame = 1.0 / double(std::max(1u, iv.frames));
        const double seconds = toUs(uint64_t(t - iv.start)) / 1e6;
        const QueueSnapshot& full = d.writerFull;
        double cpuMs = 0.0;
        const bool cpuValid = cpuMsSince(iv, t, cpuMs);

        uint64_t blockingCount = 0, blockingTicks = 0, topTicks = 0;
        uint32_t topCommand = kMaxCommands;
        for (uint32_t i = 0; i < kMaxCommands; ++i) {
          const uint32_t w = d.respWaitCount[i];
          if (w != 0) {
            const uint64_t wt = d.respWaitTicks[i];
            blockingCount += w;
            blockingTicks += wt;
            if (wt > topTicks) {
              topTicks = wt;
              topCommand = i;
            }
          }
        }

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << "[PerfBridge] " << kSide << " " << seconds << " s, " << iv.frames << " frames, "
           << (seconds > 0.0 ? double(iv.frames) / seconds : 0.0) << " fps";
#if defined(REMIX_BRIDGE_CLIENT)
        ss << " | game thread per frame: CPU ";
        if (cpuValid) {
          ss << cpuMs * perFrame << " ms";
        } else {
          ss << "?";
        }
        ss << ", in bridge sends " << toUs(d.cmdTicks) / 1e3 * perFrame << " ms ("
           << double(d.commands) * perFrame << " commands, "
           << double(d.records) * perFrame << " batched setters, "
           << double(d.uploadBytes) / 1024.0 * perFrame << " KB uploaded)"
           << ", waiting in Present " << toUs(d.presentWaitTicks) / 1e3 * perFrame << " ms"
           << ", round trips " << double(blockingCount) * perFrame << " (" << toUs(blockingTicks) / 1e3 * perFrame << " ms";
        if (topCommand < kMaxCommands) {
          ss << ", most " << shortName(topCommand);
        }
        ss << "), command ring full " << toUs(full.spinTicks + full.blockTicks) / 1e3 * perFrame << " ms";
#else
        ss << " | server thread per frame: processing commands " << toUs(d.cmdTicks) / 1e3 * perFrame
           << " ms (" << double(d.commands) * perFrame << " commands), CPU ";
        if (cpuValid) {
          ss << cpuMs * perFrame << " ms";
        } else {
          ss << "?";
        }
        const QueueSnapshot& empty = d.readerEmpty;
        ss << ", waiting for the game's commands " << toUs(empty.spinTicks + empty.blockTicks) / 1e3 * perFrame << " ms"
           << ", response ring full " << toUs(full.spinTicks + full.blockTicks) / 1e3 * perFrame << " ms";
#endif
        Logger::info(ss.str());
      }

      void logInterval(const Totals& d, const Interval& iv, int64_t t) {
        const double frames = double(iv.frames);
        const double perFrame = 1.0 / frames;
        const QueueSnapshot& full = d.writerFull;
        const QueueSnapshot& empty = d.readerEmpty;

        double cpuMs = 0.0;
        const bool cpuValid = cpuMsSince(iv, t, cpuMs);
        const double cpuMsPerFrame = cpuValid ? cpuMs * perFrame : -1.0;
        // Spinning in either wait is CPU time the thread counter includes but no work.
        const double spinMsPerFrame = toUs(full.spinTicks + empty.spinTicks) / 1e3 * perFrame;

        uint64_t classCount[kClassCount] = {};
        std::vector<std::pair<uint32_t, uint32_t>> commands;  // count, command
        std::vector<std::pair<uint64_t, uint32_t>> timed;     // ticks, command
        uint64_t presentTicks = 0;
        std::vector<std::pair<uint64_t, uint32_t>> blocking;  // ticks, command
        std::vector<std::pair<uint32_t, uint32_t>> dropped;   // count, command
        uint64_t blockingCount = 0, blockingTicks = 0, droppedCount = 0;
        for (uint32_t i = 0; i < kMaxCommands; ++i) {
          const uint32_t dr = d.droppedCount[i];
          if (dr != 0) {
            dropped.emplace_back(dr, i);
            droppedCount += dr;
          }
          const uint32_t n = d.cmdCount[i];
          if (n != 0) {
            classCount[classify(i)] += n;
            commands.emplace_back(n, i);
          }
          const uint64_t ct = d.cmdTicksByType[i];
          if (ct != 0) {
            timed.emplace_back(ct, i);
            if (isPresent(i)) {
              presentTicks += ct;
            }
          }
          const uint32_t w = d.respWaitCount[i];
          if (w != 0) {
            const uint64_t wt = d.respWaitTicks[i];
            blocking.emplace_back(wt, i);
            blockingCount += w;
            blockingTicks += wt;
          }
        }
        std::sort(commands.begin(), commands.end(), std::greater<>());
        std::sort(timed.begin(), timed.end(), std::greater<>());
        std::sort(blocking.begin(), blocking.end(), std::greater<>());
        std::sort(dropped.begin(), dropped.end(), std::greater<>());

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << "[PerfBridge] " << kSide << " frames=" << iv.frames
           << " frameMs=" << std::setprecision(2) << toUs(uint64_t(t - iv.start)) / 1e3 * perFrame;
        if (cpuValid) {
          ss << " threadCpuMsPerFrame=" << cpuMsPerFrame << " waitSpinMsPerFrame=" << spinMsPerFrame
             << " workCpuMsPerFrame=" << std::max(0.0, cpuMsPerFrame - spinMsPerFrame);
        }
        ss << std::setprecision(1)
           << " | cmdsPerFrame=" << double(d.commands) * perFrame
           << " stateRecordsPerFrame=" << double(d.records) * perFrame
           << " (by type, records included: ";
        for (uint32_t c = 0; c < kClassCount; ++c) {
          ss << (c ? " " : "") << kClassNames[c] << "=" << double(classCount[c]) * perFrame;
        }
        ss << ") dataKBPerFrame=" << double(d.dataWords) * 4.0 / 1024.0 * perFrame
           << " uploadKBPerFrame=" << double(d.uploadBytes) / 1024.0 * perFrame
#if defined(REMIX_BRIDGE_CLIENT)
           << " sendUsPerFrame="
#else
           << " processUsPerFrame="
#endif
           << toUs(d.cmdTicks) * perFrame;
#if defined(REMIX_BRIDGE_CLIENT)
        ss << " | presentWaitUsPerFrame=" << toUs(d.presentWaitTicks) * perFrame
           << " blockingPerFrame=" << double(blockingCount) * perFrame
           << " blockingUsPerFrame=" << toUs(blockingTicks) * perFrame
           << " | ringFull: waitsPerFrame=" << double(full.waits) * perFrame
           << " spinUsPerFrame=" << toUs(full.spinTicks) * perFrame
           << " blockedUsPerFrame=" << toUs(full.blockTicks) * perFrame
           << " | responseWait: spinUsPerFrame=" << toUs(empty.spinTicks) * perFrame
           << " blockedUsPerFrame=" << toUs(empty.blockTicks) * perFrame;
        {
          // Query GetData calls per frame by path; after "/" the ones that returned the result (S_OK).
          constexpr const char* kQueryPathNames[size_t(QueryPath::Count)] = { "asyncCached", "asyncRequest", "blocking" };
          ss << " | queryGetDataPerFrame:";
          for (size_t i = 0; i < size_t(QueryPath::Count); ++i) {
            ss << " " << kQueryPathNames[i] << "=" << double(d.queryCalls[i]) * perFrame
               << "/" << double(d.querySignalled[i]) * perFrame;
          }
        }
#else
        ss << " | idle: waitsPerFrame=" << double(empty.waits) * perFrame
           << " spinUsPerFrame=" << toUs(empty.spinTicks) * perFrame
           << " blockedUsPerFrame=" << toUs(empty.blockTicks) * perFrame
           << " blocksPerFrame=" << double(empty.blocks) * perFrame
           << " | responseRingFull: waitsPerFrame=" << double(full.waits) * perFrame
           << " spinUsPerFrame=" << toUs(full.spinTicks) * perFrame;
#endif
        ss << " | wakesSentPerFrame=" << double(full.wakesSent + empty.wakesSent) * perFrame;
        Logger::info(ss.str());

        std::ostringstream top;
        top << std::fixed << std::setprecision(1) << "[PerfBridge] " << kSide << " top commands per frame:";
        for (size_t i = 0; i < commands.size() && i < kTopCommands; ++i) {
          top << " " << shortName(commands[i].second) << "=" << double(commands[i].first) * perFrame;
        }
        Logger::info(top.str());

        // Time per command type, as a share of all command time. Records of a state packet count in the packet.
        if (d.cmdTicks != 0) {
          const auto share = [&d](uint64_t ticks) { return 100.0 * double(ticks) / double(d.cmdTicks); };
          std::ostringstream byTime;
          byTime << std::fixed << std::setprecision(1) << "[PerfBridge] " << kSide << " top commands by time per frame: present="
                 << toUs(presentTicks) * perFrame << "us (" << share(presentTicks) << "%) |";
          for (size_t i = 0; i < timed.size() && i < kTopCommands; ++i) {
            byTime << " " << shortName(timed[i].second) << "=" << toUs(timed[i].first) * perFrame << "us ("
                   << share(timed[i].first) << "%)";
          }
          Logger::info(byTime.str());
        }

        if (!dropped.empty()) {
          // Setters sent appear in "top commands"; these are the ones not sent (already on the server).
          std::ostringstream drp;
          drp << std::fixed << std::setprecision(1) << "[PerfBridge] " << kSide << " redundant setters dropped per frame: total="
              << double(droppedCount) * perFrame << " |";
          for (size_t i = 0; i < dropped.size() && i < kTopCommands; ++i) {
            drp << " " << shortName(dropped[i].second) << "=" << double(dropped[i].first) * perFrame;
          }
          Logger::info(drp.str());
        }

        if (!blocking.empty()) {
          std::ostringstream blk;
          blk << std::fixed << std::setprecision(1) << "[PerfBridge] " << kSide << " blocking calls per frame:";
          for (size_t i = 0; i < blocking.size() && i < kTopBlocking; ++i) {
            const uint32_t cmd = blocking[i].second;
            blk << " " << shortName(cmd) << "=" << double(d.respWaitCount[cmd]) * perFrame
                << " (" << toUs(blocking[i].first) * perFrame << "us)";
          }
          Logger::info(blk.str());
        }
      }
    }

    void init(uint32_t intervalFrames) {
      g_intervalFrames.store(intervalFrames, std::memory_order_relaxed);
      if (intervalFrames != 0) {
        Logger::info(format_string("[PerfBridge] %s full counters every %u frames.", kSide, intervalFrames));
      }
    }

    int64_t now() {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      return t.QuadPart;
    }

    void noteCommand(uint16_t command, uint32_t dataWords, int64_t ticks) {
      const uint64_t positiveTicks = uint64_t(std::max<int64_t>(ticks, 0));
      if (command < kMaxCommands) {
        bump(s_cmdCount[command], 1);
        bump(s_cmdTicksByType[command], positiveTicks);
      }
      bump(s_commands, 1);
      bump(s_dataWords, dataWords);
      bump(s_cmdTicks, positiveTicks);
    }

    void noteRecord(uint16_t command) {
      if (command < kMaxCommands) {
        bump(s_cmdCount[command], 1);
      }
      bump(s_records, 1);
    }

    void noteDropped(uint16_t command) {
      if (command < kMaxCommands) {
        s_droppedCount[command].fetch_add(1, std::memory_order_relaxed);
      }
    }

    void noteSent(uint16_t command) {
      s_lastSent.store(command, std::memory_order_relaxed);
    }

    void noteResponseWait(int64_t ticks) {
      const uint32_t command = s_lastSent.load(std::memory_order_relaxed);
      if (command < kMaxCommands) {
        s_respWaitCount[command].fetch_add(1, std::memory_order_relaxed);
        s_respWaitTicks[command].fetch_add(uint64_t(std::max<int64_t>(ticks, 0)), std::memory_order_relaxed);
      }
    }

    void notePresentWait(int64_t ticks) {
      s_presentWaits.fetch_add(1, std::memory_order_relaxed);
      s_presentWaitTicks.fetch_add(uint64_t(std::max<int64_t>(ticks, 0)), std::memory_order_relaxed);
    }

    void noteQuery(QueryPath path, bool signalled) {
      const size_t i = size_t(path);
      if (i < size_t(QueryPath::Count)) {
        s_queryCalls[i].fetch_add(1, std::memory_order_relaxed);
        if (signalled) {
          s_querySignalled[i].fetch_add(1, std::memory_order_relaxed);
        }
      }
    }

    void noteUpload(uint64_t bytes) {
      bump(s_uploadBytes, bytes);
    }

    void noteAsyncCreateFailure(uint16_t command, long hresult) {
      const uint64_t n = s_asyncCreateFailures.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= kCreateFailuresLoggedIndividually) {
        Logger::err(format_string("[Bridge] async create %s failed on the server with 0x%08x; the game was told D3D_OK "
                                  "(failure %u of the first %u logged individually).",
                                  Commands::toString(static_cast<Commands::D3D9Command>(command)).c_str(),
                                  static_cast<unsigned>(hresult), static_cast<unsigned>(n),
                                  static_cast<unsigned>(kCreateFailuresLoggedIndividually)));
      }
    }

    void onFrame(const QueueWaitStats& writerFull, const QueueWaitStats& readerEmpty) {
      reportAsyncCreateFailures();

      const uint32_t interval = g_intervalFrames.load(std::memory_order_relaxed);
      const int64_t t = now();
      if (!s_short.started) {
        s_now.take(writerFull, readerEmpty);
        begin(s_short, t);
        begin(s_full, t);
        return;
      }
      ++s_full.frames;
      ++s_short.frames;

      // The full breakdown every perfLogIntervalFrames frames when that is set, and the short line every 30 s always.
      const bool fullDue = interval != 0 && s_full.frames >= interval;
      const double seconds = toUs(uint64_t(t - s_short.start)) / 1e6;
      const bool shortDue = seconds >= kShortLineSeconds;
      if (!fullDue && !shortDue) {
        return;
      }
      s_now.take(writerFull, readerEmpty);
      if (fullDue) {
        s_delta.diff(s_now, s_full.base);
        logInterval(s_delta, s_full, t);
        begin(s_full, t);
      }
      if (shortDue) {
        // An interval stretched by a long pause (the game minimised, a stalled load) is restarted unlogged.
        if (seconds < 2.0 * kShortLineSeconds) {
          s_delta.diff(s_now, s_short.base);
          logShort(s_delta, s_short, t);
        }
        begin(s_short, t);
      }
    }
  }
}
