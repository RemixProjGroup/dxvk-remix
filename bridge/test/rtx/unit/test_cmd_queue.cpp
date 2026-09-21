#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include "config/config.h"
#include "config/global_options.h"
#include "util_commands.h"
#include "util_circularqueue.h"
#include "util_atomiccircularqueue.h"
#include "util_scopedlock.h"


using namespace std;
using namespace Commands;
using namespace bridge_util;

const int QUEUE_SIZE = 5;
const int MEM_SIZE = 640;
void* gMemoryData = NULL;

class CommandQueueHistoryTest {
public:
  static void run() {
    cout << "Begin CommandHistoryQueue smoke test" << endl;
    test_smoke();
    test_response_transaction_serialization();
    test_concurrent_response_uids();
    cout << "CommandHistoryQueue successfully smoke tested" << endl;
  }

private:
  static void test_smoke() {
    gMemoryData = new char[MEM_SIZE];
    AtomicCircularQueue<Header, Accessor::Writer> commandQueueObject("Client2ServerCommand",
                                gMemoryData,
                                MEM_SIZE,
                                QUEUE_SIZE);

    // Pushing list of commands into the queue
    if (commandQueueObject.push({ Bridge_Syn, 0, 0, 0 }) != Result::Success) {
      throw string("Issue sending command to the queue");
    }
    if (commandQueueObject.push({ Bridge_Ack, 0, 0, 0 }) != Result::Success) {
      throw string("Issue sending command to the queue");
    }
    if (commandQueueObject.push({ IDirect3DDevice9Ex_GetDeviceCaps, 0, 0, 0 }) != Result::Success) {
      throw string("Issue sending command to the queue");
    }

    // Pulling commands from the queue
    Result result = Result::Failure;
    Header pullResult = commandQueueObject.pull(result);
    if (result != Result::Success) {
      throw string("Issue retrieving command from the queue");
    }
    if (pullResult.command != Bridge_Syn) {
      throw string("Retrieved command from the queue is not as expected");
    }

    // Check if Command sent to the queue is consistent
    vector<D3D9Command> commandSent;
    commandSent = commandQueueObject.getWriterQueueData(3);
    if (commandSent[0] != IDirect3DDevice9Ex_GetDeviceCaps || commandSent[1] != Bridge_Ack || commandSent[2] != Bridge_Syn) {
      throw string("Commands sent do not match");
    }

    // Check if Command recieved from the queue is consistent
    vector<D3D9Command> commandReceived;
    commandReceived = commandQueueObject.getReaderQueueData(1);
    if (commandReceived[0] != Bridge_Syn) {
      throw string("Commands received do not match");
    }
  }

  static void test_concurrent_response_uids() {
    constexpr uint32_t requestsPerCaller = 500;
    constexpr uint32_t capacity = 4;
    std::vector<char> requestMemory(1024), responseMemory(1024);
    AtomicCircularQueue<Header, Accessor::Writer> requests("requests", requestMemory.data(), requestMemory.size(), capacity);
    AtomicCircularQueue<Header, Accessor::Reader> serverRequests("requests", requestMemory.data(), requestMemory.size(), capacity);
    AtomicCircularQueue<Header, Accessor::Writer> responses("responses", responseMemory.data(), responseMemory.size(), capacity);
    AtomicCircularQueue<Header, Accessor::Reader> clientResponses("responses", responseMemory.data(), responseMemory.size(), capacity);
    std::atomic<bool> failed = false;
    std::atomic<bool> start = false;
    uint32_t nextUid = 0;

    std::thread server([&] {
      for (uint32_t i = 0; i < requestsPerCaller * 2 && !failed.load(); ++i) {
        Result result;
        const Header request = serverRequests.pull(result, 1000);
        if (result != Result::Success) {
          failed.store(true);
          return;
        }
        std::this_thread::yield();
        if (responses.push({Bridge_Response, 0, 0, request.pHandle}) != Result::Success) {
          failed.store(true);
          return;
        }
      }
    });
    auto caller = [&] {
      while (!start.load()) {
        std::this_thread::yield();
      }
      for (uint32_t i = 0; i < requestsPerCaller && !failed.load(); ++i) {
        bridge_util::ResponseTransaction transaction;
        const uint32_t uid = ++nextUid;
        if (requests.push({IDirect3DQuery9_GetData, 0, 0, uid}) != Result::Success) {
          failed.store(true);
          return;
        }
        std::this_thread::yield();
        Result result;
        const Header response = clientResponses.pull(result, 1000);
        if (result != Result::Success || response.command != Bridge_Response || response.pHandle != uid) {
          failed.store(true);
          return;
        }
      }
    };
    std::thread first(caller), second(caller);
    start.store(true);
    first.join();
    second.join();
    server.join();
    if (failed.load() || nextUid != requestsPerCaller * 2 || !requests.isEmpty() || !responses.isEmpty()) {
      throw string("Concurrent request/response transactions lost, reordered or abandoned a UID");
    }
    cout << "Verified 1000 concurrent request/response UIDs using paired queues" << endl;
  }

  static void test_response_transaction_serialization() {
    using namespace std::chrono_literals;

    // ResponseTransaction and the multithreaded device guard must use one
    // recursive mutex. This checks both cross-thread serialization and the
    // re-entrant path used when a response handler calls a device method.
    std::atomic<bool> transactionEntered = false;
    std::atomic<bool> allowRelease = false;
    std::atomic<bool> deviceLockEntered = false;

    std::thread responseThread([&] {
      bridge_util::ResponseTransaction transaction;
      transactionEntered.store(true, std::memory_order_release);

      // Same-thread recursion models a response handler entering a device method.
      std::lock_guard<std::recursive_mutex> deviceLock(bridge_util::getClientBridgeMutex());
      while (!allowRelease.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!transactionEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    if (!transactionEntered.load(std::memory_order_acquire)) {
      allowRelease.store(true, std::memory_order_release);
      responseThread.join();
      throw string("Response transaction did not acquire its serialization lock");
    }

    std::thread deviceThread([&] {
      auto& deviceMutex = bridge_util::getClientBridgeMutex();
      if (deviceMutex.try_lock()) {
        deviceLockEntered.store(true, std::memory_order_release);
        deviceMutex.unlock();
      }
    });
    deviceThread.join();

    // A second thread must not enter the device domain while the response is live.
    const bool responseAndDeviceNotSerialized = deviceLockEntered.load(std::memory_order_acquire);

    std::atomic<bool> secondTransactionEntered = false;
    std::thread secondResponseThread([&] {
      bridge_util::ResponseTransaction transaction;
      secondTransactionEntered.store(true, std::memory_order_release);
    });

    const auto secondAttemptDeadline = std::chrono::steady_clock::now() + 50ms;
    while (std::chrono::steady_clock::now() < secondAttemptDeadline && !secondTransactionEntered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const bool concurrentResponseTransactionsAllowed = secondTransactionEntered.load(std::memory_order_acquire);

    allowRelease.store(true, std::memory_order_release);
    responseThread.join();

    const auto secondTransactionDeadline = std::chrono::steady_clock::now() + 1s;
    while (!secondTransactionEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < secondTransactionDeadline) {
      std::this_thread::yield();
    }
    if (!secondTransactionEntered.load(std::memory_order_acquire)) {
      secondResponseThread.join();
      throw string("Second response transaction did not complete after the first released the queue");
    }
    secondResponseThread.join();

    if (responseAndDeviceNotSerialized) {
      throw string("Response and device locks are not serialized together");
    }
    if (concurrentResponseTransactionsAllowed) {
      throw string("Concurrent response transactions can reorder FIFO responses");
    }
    auto& deviceMutex = bridge_util::getClientBridgeMutex();
    if (!deviceMutex.try_lock()) {
      throw string("Device lock did not release after response transaction completed");
    }
    deviceMutex.unlock();
  }
};

int main() {
  try {
    CommandQueueHistoryTest::run();
  }
  catch (const string& errorMessage) {
    cerr << errorMessage << endl;
	return -1;
  }
  delete[] gMemoryData;
  return 0;
}

