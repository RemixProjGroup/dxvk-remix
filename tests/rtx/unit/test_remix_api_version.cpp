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

// Checks the Remix API version rules in src/util/util_remix_api_version.h:
// - which client versions the runtime and the bridge client accept;
// - how much of remixapi_Interface InitializeLibrary writes for each accepted version.

#include <cstdio>
#include <cstring>
#include <vector>

#define REMIX_WINAPI_NO_INCLUDE
#define REMIX_WINAPI_NO_LIBRARY_LOADER

#include "../../../src/util/util_remix_api_version.h"

namespace {
  using namespace remixapi_version;

  int g_failures = 0;
  constexpr uint64_t kRuntimeVersion = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  constexpr uint8_t kCanary = 0xCD;
  constexpr size_t kCanaryBytes = 64;

  // The 1.0 interface has 41 function pointers, and no later version may change that prefix.
  static_assert(kInterfaceSize1_0 == 41 * sizeof(void*), "The 1.0 interface prefix changed");

  void check(bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL: %s\n", what);
      ++g_failures;
    }
  }

  void testAccepted(uint64_t version, size_t clientBytes, const char* what) {
    check(isClientVersionCompatible(version, kRuntimeVersion), what);
    check(clientInterfaceSize(version) == clientBytes, what);

    // The copy InitializeLibrary makes must leave the bytes behind the client's interface untouched.
    const remixapi_Interface interf {};
    std::vector<uint8_t> buffer(sizeof(remixapi_Interface) + kCanaryBytes, kCanary);
    std::memcpy(buffer.data(), &interf, clientInterfaceSize(version));
    for (size_t i = clientBytes; i < buffer.size(); ++i) {
      if (buffer[i] != kCanary) {
        check(false, what);
        break;
      }
    }
  }

  void testRejected(uint64_t version, const char* what) {
    check(!isClientVersionCompatible(version, kRuntimeVersion), what);
  }

  void testVersions() {
    testAccepted(kRuntimeVersion, sizeof(remixapi_Interface), "a client at the runtime's version");
    testAccepted(REMIXAPI_VERSION_MAKE(1, 0, 0), kInterfaceSize1_0, "a 1.0.0 client");
    testAccepted(REMIXAPI_VERSION_MAKE(1, 0, 7), kInterfaceSize1_0, "a 1.0 client with a newer patch");
    testAccepted(REMIXAPI_VERSION_MAKE(0, 1000, 0), kInterfaceSize1_0, "a 0.1000.0 client");
    testAccepted(REMIXAPI_VERSION_MAKE(0, 1000, 1), kInterfaceSize1_0, "a 0.1000.1 client");

    testRejected(REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR + 1, 0), "a client with a newer minor");
    testRejected(REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR + 1, 0, 0), "a client with a newer major");
    testRejected(REMIXAPI_VERSION_MAKE(0, 6, 4), "an NVIDIA 0.6.4 client");
    testRejected(REMIXAPI_VERSION_MAKE(0, 6, 12), "an NVIDIA 0.6.12 client");
    testRejected(REMIXAPI_VERSION_MAKE(0, 1003, 0), "a 0.1003.0 client from another fork");
  }
}

int main() {
  testVersions();
  if (g_failures != 0) {
    std::printf("%d failures\n", g_failures);
    return -1;
  }
  std::printf("test_remix_api_version: all passed\n");
  return 0;
}
