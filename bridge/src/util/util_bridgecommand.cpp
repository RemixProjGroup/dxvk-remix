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
#include "util_bridgecommand.h"
#include "log/log_strings.h"

#include <type_traits>

namespace {
  // [PerfBridge] counts the device bridge only (util_perfbridge.h).
  template<typename BridgeIdT>
  constexpr bool kIsDeviceBridge = std::is_same_v<BridgeIdT, ::BridgeId::Device>;

  // Client: times one wait for a server response and attributes it to the request sent last.
  struct ResponseWaitTimer {
    int64_t start;
    ~ResponseWaitTimer() {
      if (start != 0) {
        bridge_util::perf::noteResponseWait(bridge_util::perf::now() - start);
      }
    }
  };

  // The longest wait between two checks of the reader's position when the writer waits for space.
  constexpr DWORD kOverwritePollMs = 1;

  DWORD get_default_timeout() {
    const auto timeout = GlobalOptions::getCommandTimeout();
    const auto retries = GlobalOptions::getCommandRetries();
    const auto default = timeout * retries;
    // Catch overflow and return infinite in that case
    return ((timeout != 0) && (default / timeout != retries)) ? INFINITE : default;
  }
}

#define DECL_BRIDGE_FUNC(RETURN_T, NAME, ...) \
  template<typename BridgeId> \
  RETURN_T Bridge<BridgeId>::NAME(__VA_ARGS__)

DECL_BRIDGE_FUNC(void, init,
    const std::string baseName,
    const size_t writerChannelMemSize, const size_t writerChannelCmdQueueSize,
    const size_t writerChannelDataQueueSize, const size_t readerChannelMemSize,
    const size_t readerChannelCmdQueueSize, const size_t readerChannelDataQueueSize) {
  static bool bIsInit = false;
  if (bIsInit) {
    Logger::warn("Re-Init'ing Bridge type. May be sign of problem code.");
    return;
  }
  s_pWriterChannel = new WriterChannel(
    baseName + kWriterChannelName,
    writerChannelMemSize, writerChannelCmdQueueSize, writerChannelDataQueueSize);
  s_pReaderChannel = new ReaderChannel(
    baseName + kReaderChannelName,
    readerChannelMemSize, readerChannelCmdQueueSize, readerChannelDataQueueSize);
  // An item takes at most a sixteenth of the ring. A streamed command's unpublished data, at most two items and a
  // skipped ring tail, then always fits (see "Streamed commands").
  s_maxItemWords = std::clamp<size_t>(s_pWriterChannel->data->get_total_size() / 16, 16, kMaxItemWordsLimit);
  if constexpr (kIsDeviceBridge<BridgeId>) {
    // The device bridge's reader reports its position (releaseReadData), so the writer protects unread data from the
    // start. A position of -1 would leave the queue unprotected until the reader's first report.
    *s_pWriterChannel->serverDataPos = 0;
    bridge_util::perf::init(GlobalOptions::getPerfLogIntervalFrames());
  }
  bIsInit = true;
}

DECL_BRIDGE_FUNC(void, perfOnFrame) {
  if constexpr (kIsDeviceBridge<BridgeId>) {
    bridge_util::perf::onFrame(s_pWriterChannel->commands->fullWaitStats(),
                               s_pReaderChannel->commands->emptyWaitStats());
  }
}

DECL_BRIDGE_FUNC(void, syncDataQueue, size_t expectedMemUsage, bool posResetOnLastIndex) {
  // The flag is read before the position. The reader resets its position before it clears the flag, so a clear flag
  // is never paired with a position from the reader's previous lap.
  const bool serverLapBehind = *static_cast<volatile bool*>(s_pWriterChannel->serverResetPosRequired);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  int serverCount = (int) *static_cast<volatile int32_t*>(static_cast<void*>(s_pWriterChannel->serverDataPos));
  size_t currClientDataPos = s_pWriterChannel->get_data_pos();
  size_t expectedClientDataPos = currClientDataPos + ((expectedMemUsage != 0) ? expectedMemUsage : 1) - 1;
  size_t totalSize = s_pWriterChannel->data->get_total_size();

  // The current batch has run past the end of the queue and continues at its start.
  const bool batchWrapped = s_curBatchStartPos >= 0 && currClientDataPos < (size_t) s_curBatchStartPos;

  auto handleOverwriteCondition = [&]() {
    // Below variable is set to let the server know that a particular position
    // in the queue not yet accessed by it is going to be used
    const int64_t serverTargetPos = (int64_t) s_curBatchStartPos - 1;
    *s_pWriterChannel->clientDataExpectedPos = serverTargetPos;
    Logger::debug("Data Queue overwrite condition triggered");
    // Check to see if there is even enough space to ever succeed in pushing all the data
    if ((expectedMemUsage + (currClientDataPos >= s_curBatchStartPos ? currClientDataPos - s_curBatchStartPos : currClientDataPos + totalSize - s_curBatchStartPos)) > totalSize) {
      Logger::errLogMessageBoxAndExit(std::string(logger_strings::OutOfBufferMemory) + std::string(logger_strings::OutOfBufferMemory1) + logger_strings::bufferNameToOption(s_pWriterChannel->data->getName()));
    }
    // The server has read everything before this batch once it is past the batch start.
    // While the client is a lap ahead (serverResetPosRequired), the server must also have wrapped around.
    // A batch that has wrapped set that flag itself, and the server cannot wrap before the batch is published.
    // The server is then on the batch start's lap, so its position alone tells whether it is past the batch start.
    // The low word of the position is read because a 32-bit client cannot read the 64-bit value atomically.
    const auto serverReachedTarget = [&]() {
      if (!batchWrapped && *static_cast<volatile bool*>(s_pWriterChannel->serverResetPosRequired)) {
        return false;
      }
      std::atomic_thread_fence(std::memory_order_seq_cst);
      return (int64_t) *static_cast<volatile int32_t*>(static_cast<void*>(s_pWriterChannel->serverDataPos)) > serverTargetPos;
    };
    Logger::debug("Waiting on server to process enough data from data queue to prevent overwrite...");
    // The server signals the semaphore only when it passes the target while the flag is clear.
    // The positions are therefore polled between short waits.
    // The wait is limited to commandRetries times the semaphore timeout.
    const uint64_t timeoutMs = uint64_t(GlobalOptions::getSemaphoreTimeout()) * GlobalOptions::getCommandRetries();
    const ULONGLONG startTick = GetTickCount64();
    bool reached = true;
    while (!serverReachedTarget()) {
      if (!gbBridgeRunning || GetTickCount64() - startTick >= timeoutMs) {
        reached = false;
        break;
      }
      s_pWriterChannel->dataSemaphore->wait(kOverwritePollMs);
    }
    if (!reached) {
      Logger::err("Max retries reached waiting on the server to process enough data to prevent a overwrite!");
    }
    *s_pWriterChannel->clientDataExpectedPos = -1;
    Logger::debug("DataQueue overwrite condition resolved");
  };

  // A write that runs past the end of the queue continues at its start.
  // A blob restarts at the start of the queue when it would end on or after the queue's last word (ensure_space).
  const bool wraps = posResetOnLastIndex
    ? currClientDataPos + expectedMemUsage >= totalSize
    : expectedClientDataPos >= totalSize;
  // A write of single words that fills the queue's last word also moves the position to the queue's start.
  const bool endsAtQueueEnd = !posResetOnLastIndex && expectedClientDataPos + 1 == totalSize;
  if (wraps) {
    if (posResetOnLastIndex) {
      // Reset index pos to 0 if the size is larger than the remaining buffer
      expectedClientDataPos = expectedMemUsage - 1;
    } else {
      // Evaluate the respective pos when the end of the queue is reached
      expectedClientDataPos = expectedClientDataPos - totalSize;
    }
  }

  // The server reads none of the current batch before its header is pushed.
  // A write that reaches the batch's own start can therefore never be made safe by waiting for the server.
  if (s_curBatchStartPos >= 0) {
    const size_t batchStartPos = (size_t) s_curBatchStartPos;
    const bool batchWrapped = currClientDataPos < batchStartPos;
    const bool reachesBatchStart = batchWrapped
      ? wraps || expectedClientDataPos >= batchStartPos
      : wraps && expectedClientDataPos >= batchStartPos;
    if (reachesBatchStart) {
      Logger::err(format_string("[Bridge] a command needs more of the %s queue than it holds: the batch starts at word %zu, "
                                "the write of %zu words starts at word %zu, and the queue holds %zu words.",
                                s_pWriterChannel->data->getName().c_str(), batchStartPos, expectedMemUsage,
                                currClientDataPos, totalSize));
      Logger::errLogMessageBoxAndExit(std::string(logger_strings::OutOfBufferMemory) + std::string(logger_strings::OutOfBufferMemory1) + logger_strings::bufferNameToOption(s_pWriterChannel->data->getName()));
    }
  }

  if (wraps && serverLapBehind) {
    // Double Overflow Condition Detected, mitigate by stalling and waiting for a response
    handleOverwriteCondition();
  }

  /*
    * overwrite conditions
    * 1. client < server, expectedClient >= server
    * 2. client > server, expectedClient >= server, expectedClient < client
    */
  const bool clientLapAhead = wraps || serverLapBehind;
  if (expectedClientDataPos >= serverCount &&
      ((s_curBatchStartPos < serverCount)
       || (s_curBatchStartPos > serverCount && expectedClientDataPos < s_curBatchStartPos)
       || ((s_curBatchStartPos <= serverCount) && clientLapAhead))) {
    handleOverwriteCondition();
  }

  // The flag tells the server that it has to complete a loop to reach the client's position.
  // It is set only after the waits above, because the server releases a waiting client only while the flag is clear.
  if (wraps || endsAtQueueEnd) {
    *s_pWriterChannel->serverResetPosRequired = true;
  }
}

DECL_BRIDGE_FUNC(Header, pop_front) {
  ZoneScoped;
#ifdef REMIX_BRIDGE_CLIENT
  // The client pops a response after reading it. A streamed response's last chunk header is then at the front.
  if (s_readStreamed && !endReadCommand()) {
    Logger::err("[Bridge] the client did not read a streamed server response to its end; stopping the bridge.");
    gbBridgeRunning = false;
  }
#endif
  Result result;
  // No retries, but wait the same amount of time
  const auto response = getReaderChannel().commands->pull(result, get_default_timeout());
  if (RESULT_FAILURE(result)) {
    // For now just log when things go wrong, but could use some robustness improvements
    Logger::err("CommandQueue get_response: Failed to retrieve the command response!");
  }
#ifdef REMIX_BRIDGE_CLIENT
  if constexpr (kIsDeviceBridge<BridgeId>) {
    // The server writes its responses only into space the client has released.
    releaseReadData();
  }
  s_readStreamed = false;
  s_readScratch.clear();
#endif
  return response;
}

DECL_BRIDGE_FUNC(void, releaseReadData) {
  const ReaderChannel& channel = getReaderChannel();
  const int64_t pos = (int64_t) get_data_pos();
  // Every read of the released data happens before the writer can see the new position.
  std::atomic_thread_fence(std::memory_order_release);
  *channel.serverDataPos = pos;
  // The low word is read because a 32-bit reader cannot read the 64-bit value atomically.
  const int64_t expectedPos = (int64_t) *reinterpret_cast<volatile int32_t*>(channel.clientDataExpectedPos);
  if (expectedPos != -1 && pos > expectedPos && !*static_cast<volatile bool*>(channel.serverResetPosRequired)) {
    *channel.clientDataExpectedPos = -1;
    channel.dataSemaphore->release(1);
  }
}

DECL_BRIDGE_FUNC(void, beginReadCommand, const Header& header, bool headerAtFront) {
  s_readScratch.clear();
  s_readStreamed = Commands::IsDataStreamed(header.flags);
  s_readEnded = !s_readStreamed;
  s_readHeaderAtFront = headerAtFront;
  s_readEnd = header.dataOffset;
}

DECL_BRIDGE_FUNC(bool, endReadCommand) {
  bool complete = true;
  while (s_readStreamed && !s_readEnded) {
    if (get_data_pos() != s_readEnd || !readNextChunk()) {
      complete = false;
      break;
    }
  }
  complete = complete && get_data_pos() == s_readEnd;
  s_readStreamed = false;
  s_readScratch.clear();
  return complete;
}

DECL_BRIDGE_FUNC(bool, awaitDataSlow) {
  while (get_data_pos() == s_readEnd) {
    if (s_readEnded || !gbBridgeRunning) {
      Logger::err("[Bridge] a command read more data than its sender wrote; stopping the bridge.");
      gbBridgeRunning = false;
      return false;
    }
    if (!readNextChunk()) {
      return false;
    }
  }
  return true;
}

DECL_BRIDGE_FUNC(bool, readNextChunk) {
  ZoneScoped;
  // The writer may now reuse everything read so far.
  releaseReadData();
  auto& commands = *getReaderChannel().commands;
  Result result;
  if (s_readHeaderAtFront) {
    commands.pull(result, get_default_timeout());
    if (RESULT_FAILURE(result)) {
      Logger::err("[Bridge] could not take a streamed command's header from the queue; stopping the bridge.");
      gbBridgeRunning = false;
      return false;
    }
  }
  const DWORD timeoutMs = GlobalOptions::getCommandTimeout();
  const uint32_t maxAttempts = GlobalOptions::getCommandRetries();
  Header header;
  uint32_t attempt = 0;
  do {
    header = commands.peek(result, timeoutMs);
  } while (result == Result::Timeout && attempt++ < maxAttempts && gbBridgeRunning);
  if (result != Result::Success || header.command != Commands::Bridge_DataChunk) {
    Logger::err(format_string("[Bridge] expected the next part of a streamed command, got %s; stopping the bridge.",
                              result == Result::Success ? Commands::toString(header.command).c_str() : "nothing"));
    gbBridgeRunning = false;
    return false;
  }
  if (!s_readHeaderAtFront) {
    commands.pull(result, get_default_timeout());
  }
  s_readEnd = header.dataOffset;
  s_readEnded = Commands::IsDataStreamEnd(header.flags);
  return true;
}

DECL_BRIDGE_FUNC(typename Bridge<BridgeId>::DataT, readStreamedItem, void** obj) {
  get_data();
  const DataT size = get_data();
  uint8_t* const pItem = allocScratch(size);
  size_t offset = 0;
  while (offset < size) {
    void* pPiece = nullptr;
    const DataT pieceSize = get_data_view(&pPiece);
    if (pPiece == nullptr || pieceSize == 0 || pieceSize > size - offset) {
      Logger::err(format_string("[Bridge] a streamed item of %u bytes has a piece of %u bytes at byte %zu; stopping the bridge.",
                                size, pieceSize, offset));
      gbBridgeRunning = false;
      *obj = nullptr;
      return 0;
    }
    memcpy(pItem + offset, pPiece, pieceSize);
    offset += pieceSize;
  }
  *obj = pItem;
  return size;
}

DECL_BRIDGE_FUNC(uint8_t*, allocScratch, size_t size) {
  // The padding is zeroed, so a reader that takes a few bytes more than the item holds reads zeros.
  uint8_t* const pItem = s_readScratch.emplace_back(new uint8_t[size + kScratchPadding]).get();
  memset(pItem + size, 0, kScratchPadding);
  return pItem;
}

DECL_BRIDGE_FUNC(void, keepItem, void** obj, DataT size) {
  uint8_t* const pItem = allocScratch(size);
  memcpy(pItem, *obj, size);
  *obj = pItem;
}

DECL_BRIDGE_FUNC(bridge_util::Result, ensureQueueEmpty) {
  if (getReaderChannel().commands->isEmpty()) {
    return bridge_util::Result::Success;
  }

  const uint32_t maxAttempts = GlobalOptions::getCommandRetries();
  uint32_t attemptNum = 0;
  do {
    Result result;
    getReaderChannel().commands->peek(result, 1);

    if (result != Result::Timeout) {
      // Give the server some time to process the commands
      Sleep(8);
    } else {
      // Timeout from peek() means the queue is empty
      return bridge_util::Result::Success;
    }
  } while (attemptNum++ <= maxAttempts && gbBridgeRunning);

  return bridge_util::Result::Timeout;
}

DECL_BRIDGE_FUNC(bridge_util::Result, waitForCommand, const Commands::D3D9Command& command,
                                                       DWORD overrideTimeoutMS,
                                                       std::atomic<bool>* const pbEarlyOutSignal, bool verifyUID, UID uidToVerify) {
  ZoneScoped;
  if (!gbBridgeRunning) {
    return Result::Failure;
  }
#ifdef REMIX_BRIDGE_CLIENT
  const bool timeResponseWait = kIsDeviceBridge<BridgeId> && command == Commands::Bridge_Response && bridge_util::perf::enabled();
#else
  const bool timeResponseWait = false;
#endif
  const ResponseWaitTimer responseWaitTimer { timeResponseWait ? bridge_util::perf::now() : 0 };
  DWORD peekTimeoutMS = overrideTimeoutMS > 0 ? overrideTimeoutMS : GlobalOptions::getCommandTimeout();
  uint32_t maxAttempts = GlobalOptions::getCommandRetries();
#ifdef ENABLE_WAIT_FOR_COMMAND_TRACE
  if (command != Commands::Any) {
    Logger::trace(format_string("Waiting for command %s for %d ms up to %d times...", Commands::toString(command).c_str(), peekTimeoutMS, maxAttempts));
  }
#endif
#if defined(_DEBUG) || defined(DEBUGOPT)
  if (GlobalOptions::getLogAllCommands()) {
    Logger::info("waitForCommand Command:" + toString(command) + (verifyUID ? " UID: " + std::to_string(uidToVerify) : ""));
  }
#endif
  bool infiniteRetries = false;
  bool bEarlyOut = false;
  uint32_t attemptNum = 0;
  do {
    Result result;
    Header header = getReaderChannel().commands->peek(result, peekTimeoutMS, pbEarlyOutSignal);

    switch (result) {

    case Result::Success:
    {
      bool uidVerified = true;
      if (verifyUID) {
        if (header.pHandle != uidToVerify) {
          uidVerified = false;
        }
      }
      if ((command == Commands::Bridge_Any) || (header.command == command) && uidVerified) {
#ifdef REMIX_BRIDGE_CLIENT
        // The client reads a response's data before it pops the header.
        beginReadCommand(header, true);
#endif
#ifdef ENABLE_WAIT_FOR_COMMAND_TRACE
        if (command != Commands::Bridge_Any) {
          Logger::trace(format_string("...success, command %s received!", Commands::toString(command).c_str()));
        }
#endif
        return Result::Success;
      } else {
        if (verifyUID && !uidVerified) {
          Logger::err(format_string("Unexpected bridge response UID %u while waiting for UID %u; disabling bridge to preserve response queue state.", header.pHandle, uidToVerify));
          gbBridgeRunning = false;
          return Result::Failure;
        }
#if defined(_DEBUG) || defined(DEBUGOPT)
        if (GlobalOptions::getLogAllCommands()) {
          Logger::info(format_string("Different instance of a command detected: %s with UID: %s , Expected: %s with UID: %s. ", Commands::toString(header.command).c_str(), std::to_string(header.pHandle).c_str(),
                                     Commands::toString(command).c_str(), std::to_string(uidToVerify).c_str()));
        }
#endif
        // If we see the incorrect command, we want to give the other side of
        // the bridge ample time to make an attempt to process it first
        Sleep(peekTimeoutMS);
      }
      break;
    }

    case Result::Timeout:
    {
      if (GlobalOptions::getInfiniteRetries()) {
        // Infinite retries requested, the application might be alt-tabbed and sleeping, and so we need to wait too.

        // Set timeout for consecutive peeks to 1ms to relieve spin-waits.
        peekTimeoutMS = 1;
        // Decrease the attempt counter so it won't overrun maxAttempts in case it did not capture infinite retries.
        if (attemptNum > 0) {
          --attemptNum;
        }
        // Set the flag so that the consecutive peek 1ms-timeout would not generate a failure in case if
        // infinite retries are revoked in the process.
        infiniteRetries = true;

        // Sleep for default OS period
        Sleep(1);
      } else if (infiniteRetries) {
        // A timeout in infinite retries loop but infinite retries have been revoked (app restored from alt-tab).

        // Restore peek timeout interval and continue.
        peekTimeoutMS = overrideTimeoutMS > 0 ? overrideTimeoutMS : GlobalOptions::getCommandTimeout();
        // Drop the flag - we're in the normal loop now.
        infiniteRetries = false;
      }
      Logger::trace(format_string("Peek timeout while waiting for command: %s.", Commands::toString(command).c_str()));
      break;
    }

    case Result::Failure:
    {
      Logger::trace(format_string("Peek failed while waiting for command: %s.", Commands::toString(command).c_str()));
      return Result::Failure;
    }

    }
    if (pbEarlyOutSignal) {
      bEarlyOut = pbEarlyOutSignal->load();
    }
  } while (!bEarlyOut &&
            attemptNum++ <= maxAttempts &&
            gbBridgeRunning);
  if (verifyUID && command == Commands::Bridge_Response && gbBridgeRunning) {
    Logger::err(format_string("Timed out waiting for bridge response UID %u; disabling bridge to prevent a late response from blocking the response queue.", uidToVerify));
    gbBridgeRunning = false;
  }
  return Result::Timeout;
}

DECL_BRIDGE_FUNC(void, pushHeaderLocked, Commands::D3D9Command command, Commands::Flags flags, uint32_t handle) {
  uint32_t numRetries = 0;
  Result result;
  // We check if the bridge is enabled for each loop iteration in case it
  // was disabled externally by the server process exit callback.
  do {
    result = s_pWriterChannel->commands->push({ command, flags, (uint32_t) s_pWriterChannel->data->get_pos(), handle });
#if defined(_DEBUG) || defined(DEBUGOPT)
    if (GlobalOptions::getLogAllCommands()) {
      Logger::info("Pushed: " + toString(command));
    }
#endif
  } while (
        RESULT_FAILURE(result)
    && numRetries++ < GlobalOptions::getCommandRetries()
    && gbBridgeRunning
#ifdef REMIX_BRIDGE_CLIENT
    && BridgeState::getServerState_NoLock() == BridgeState::ProcessState::Running
#endif
  );
#ifdef REMIX_BRIDGE_CLIENT
  if (BridgeState::getServerState_NoLock() >= BridgeState::ProcessState::DoneProcessing) {
    Logger::warn(format_string("The command %s will not be sent; Server is in the process of or has already shut down. Turning bridge off.", Commands::toString(command).c_str()));
    gbBridgeRunning = false;;
  } else
#endif
    if (RESULT_FAILURE(result) && gbBridgeRunning) {
      Logger::err(format_string("The command %s could not be successfully sent, turning bridge off and falling back to client rendering!", Commands::toString(command).c_str()));
      gbBridgeRunning = false;
    } else if (RESULT_SUCCESS(result) && numRetries > 1) {
      std::string commandStr = Commands::toString(command);
      Logger::debug(format_string("The command %s took %d retries (%d ms)!", commandStr.c_str(), numRetries, numRetries * GlobalOptions::getCommandTimeout()));
    }
}

DECL_BRIDGE_FUNC(void, publishLocked) {
  s_pWriterChannel->data->end_batch();
  if (!s_streamHeaderPushed) {
    pushHeaderLocked(s_streamCommand, s_streamFlags | Commands::FlagBits::DataStreamed, s_streamHandle);
    s_streamHeaderPushed = true;
  } else {
    pushHeaderLocked(Commands::Bridge_DataChunk, 0, s_streamHandle);
  }
  s_pWriterChannel->data->begin_batch();
  s_curBatchStartPos = (int32_t) s_pWriterChannel->data->get_pos();
}

DECL_BRIDGE_FUNC(void, openStatePacketLocked, uint32_t handle) {
  s_pWriterChannel->data->begin_batch();
  s_curBatchStartPos = (int32_t) s_pWriterChannel->data->get_pos();
  s_packetOpen = true;
  s_packetHandle = handle;
  s_packetRecords = 0;
  s_cmdCounter++;
  // The packet's UID, as for every command
  syncDataQueue(1, false);
  const auto result = s_pWriterChannel->data->push((UINT) s_cmdUID);
  if (RESULT_FAILURE(result)) {
    Logger::err("DataQueue send_data: Failed to send data!");
  }
}

DECL_BRIDGE_FUNC(void, closeStatePacketLocked) {
  if (!s_packetOpen) {
    return;
  }
  s_packetOpen = false;
  if (gbBridgeRunning) {
    const uint32_t perfDataWords = bridge_util::perf::enabled() ? (uint32_t) dataWordsSince(s_curBatchStartPos) : 0;
    s_pWriterChannel->data->end_batch();
    s_curBatchStartPos = -1;
    pushHeaderLocked(Commands::Bridge_StatePacket, 0, s_packetHandle);
    if (bridge_util::perf::enabled()) {
      // A packet stays open while the game works between its setters, so its open time is not send time.
      bridge_util::perf::noteSent((uint16_t) Commands::Bridge_StatePacket);
      bridge_util::perf::noteCommand((uint16_t) Commands::Bridge_StatePacket, perfDataWords, 0);
    }
  } else {
    s_pWriterChannel->data->end_batch();
    s_curBatchStartPos = -1;
  }
  ++s_cmdUID;
}

#define DECL_COMMAND_FUNC(RETURN_T, NAME, ...) \
  template<typename BridgeId> \
  RETURN_T Bridge<BridgeId>::Command::NAME(__VA_ARGS__)

DECL_COMMAND_FUNC(,Command,const Commands::D3D9Command command,
                           uintptr_t pHandle,
                           const Commands::Flags commandFlags)
  : m_command(command)
  , m_handle((uint32_t) (size_t) pHandle)
  , m_commandFlags(commandFlags) {
  // If the assert or exception gets triggered it means that there is more than one Command
  // instance in a function or command block with overlapping object lifecycles. Only one instance
  // can be alive at a time to ensure data integrity on the command and data buffers. To resolve
  // this issue I recommend enclosing the Command object in its own scope block, and make
  // sure there is no command nesting happening either.

#if defined(_DEBUG) || defined(DEBUGOPT)
  if (GlobalOptions::getLogAllCommands()) {
#ifdef REMIX_BRIDGE_CLIENT
    Logger::info("Requesting: " +toString(command) + " UID: " + std::to_string(s_cmdUID));
#else
    Logger::info("Responding: " + toString(command) + " UID: " + std::to_string(pHandle));
#endif
  }
#endif

#ifdef REMIX_BRIDGE_CLIENT
  s_pWriterChannel->m_mutex.lock();
  // Every state setter recorded so far reaches the server before this command
  closeStatePacketLocked();
#endif

  assert(!s_pWriterChannel->pbCmdInProgress->load());
  if (s_pWriterChannel->pbCmdInProgress->load()) {
    Logger::errLogMessageBoxAndExit(logger_strings::MultipleActiveCommands);
  }
  // Only start a data batch if the bridge is actually enabled, otherwise this becomes a no-op
  if (gbBridgeRunning) {
    s_pWriterChannel->data->begin_batch();
  }
  s_pWriterChannel->pbCmdInProgress->store(true);
  s_curBatchStartPos = (int32_t) s_pWriterChannel->data->get_pos();
  s_cmdCounter++;
  if constexpr (kIsDeviceBridge<BridgeId>) {
    s_streamable = true;
    s_streamHeaderPushed = false;
    s_streamCommand = command;
    s_streamFlags = commandFlags;
    s_streamHandle = m_handle;
  }
#ifdef REMIX_BRIDGE_CLIENT
  if constexpr (kIsDeviceBridge<BridgeId>) {
    if (bridge_util::perf::enabled()) {
      m_perfStart = bridge_util::perf::now();
    }
  }
#endif
  if (gbBridgeRunning) {
    // Send command id as part of data queue for everycommand from client to server
#ifdef REMIX_BRIDGE_CLIENT
      syncDataQueue(1, false);
      const auto result = s_pWriterChannel->data->push((UINT)s_cmdUID);
#if defined(_DEBUG) || defined(DEBUGOPT)
      if (GlobalOptions::getLogAllCommands()) {
        Logger::info("Pushed UID: " + std::to_string(s_cmdUID));
      }
#endif
      if (RESULT_FAILURE(result)) {
        // For now just log when things go wrong, but could use some robustness improvements
        Logger::err("DataQueue send_data: Failed to send data!");
      }
#endif
  }
}

DECL_COMMAND_FUNC(,~Command) {
  if (s_blobStaged) {
    // A staged blob without end_data_blob() is dropped. The server reads nothing for it.
    s_blobStaged = false;
    std::vector<uint8_t>().swap(s_blobStaging);
  }
  // Only actually send the command if the bridge is enabled, otherwise this becomes a no-op
  if (gbBridgeRunning) {
    const uint32_t perfDataWords = (m_perfStart != 0) ? (uint32_t) dataWordsSince(s_curBatchStartPos) : 0;
    s_pWriterChannel->data->end_batch();
    s_curBatchStartPos = -1;
    if (s_streamHeaderPushed) {
      pushHeaderLocked(Commands::Bridge_DataChunk, Commands::FlagBits::DataStreamEnd, m_handle);
    } else {
      pushHeaderLocked(m_command, m_commandFlags, m_handle);
    }
    if (m_perfStart != 0) {
      bridge_util::perf::noteSent((uint16_t) m_command);
      bridge_util::perf::noteCommand((uint16_t) m_command, perfDataWords, bridge_util::perf::now() - m_perfStart);
    }
  }
  s_streamable = false;
  s_streamHeaderPushed = false;
  s_pWriterChannel->pbCmdInProgress->store(false);
#ifdef REMIX_BRIDGE_CLIENT
  ++s_cmdUID;
  s_pWriterChannel->m_mutex.unlock();
#endif
}

#define DECL_STATE_MESSAGE_FUNC(RETURN_T, NAME, ...) \
  template<typename BridgeId> \
  RETURN_T Bridge<BridgeId>::StateMessage::NAME(__VA_ARGS__)

DECL_STATE_MESSAGE_FUNC(,StateMessage,const Commands::D3D9Command command, uintptr_t pHandle)
  : m_command(command) {
#ifdef REMIX_BRIDGE_CLIENT
  assert(Commands::IsBatchableState(command));
  const uint32_t handle = (uint32_t) (size_t) pHandle;
  s_pWriterChannel->m_mutex.lock();
  if (!gbBridgeRunning) {
    return;
  }
  // A record for another device, or a full packet, starts a new packet.
  if (s_packetOpen &&
      (s_packetHandle != handle || s_packetRecords >= kMaxPacketRecords ||
       (uint32_t) dataWordsSince(s_curBatchStartPos) >= std::min(kMaxPacketWords, s_maxItemWords))) {
    closeStatePacketLocked();
  }
  if (!s_packetOpen) {
    openStatePacketLocked(handle);
  }
  // Record header: the setter's command id. Its arguments follow through the DataWriter methods.
  syncDataQueue(1, false);
  const auto result = s_pWriterChannel->data->push((uint32_t) command);
  if (RESULT_FAILURE(result)) {
    Logger::err("DataQueue send_data: Failed to send data!");
  }
#else
  assert(false && "StateMessage is client-only");
#endif
}

DECL_STATE_MESSAGE_FUNC(,~StateMessage) {
#ifdef REMIX_BRIDGE_CLIENT
  if (s_packetOpen) {
    ++s_packetRecords;
    if (bridge_util::perf::enabled()) {
      bridge_util::perf::noteRecord((uint16_t) m_command);
    }
    // Batching off: one record per packet, sent at once (the per-command cost of before, same wire format).
    if (!s_stateBatching) {
      closeStatePacketLocked();
    }
  }
  s_pWriterChannel->m_mutex.unlock();
#endif
}

template class Bridge<BridgeId::Module>;
template class Bridge<BridgeId::Device>;
