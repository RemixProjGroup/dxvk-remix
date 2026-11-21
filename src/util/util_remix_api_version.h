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

#include <stddef.h>
#include <stdint.h>

// The C header refuses to compile for 32-bit targets, but the bridge client only needs its types.
#if _WIN64 != 1
#define REMIX_ALLOW_X86
#endif
#include <remix/remix_c.h>
#if _WIN64 != 1
#undef REMIX_ALLOW_X86
#endif

// Version rules of the Remix API, shared by the runtime, the bridge client and the unit tests.
// See the Extensibility section of documentation/RemixSDK.md for the rules every API change follows.
namespace remixapi_version {

// The minor version of the 0.x header that came before 1.0. Its interface table and structs are the same as 1.0's.
constexpr uint64_t kPreReleaseMinor = 1000;

// Returns true when the runtime can serve a client built against clientVersion.
// A client with the runtime's major version works when its minor version is not newer than the runtime's.
// A 1.x runtime also serves 0.1000.x clients.
// Every other 0.x client, such as NVIDIA dxvk-remix 0.6.x, uses another layout and is rejected.
constexpr bool isClientVersionCompatible(uint64_t clientVersion, uint64_t runtimeVersion) {
  if (REMIXAPI_VERSION_GET_MAJOR(runtimeVersion) == 1 &&
      REMIXAPI_VERSION_GET_MAJOR(clientVersion) == 0 &&
      REMIXAPI_VERSION_GET_MINOR(clientVersion) == kPreReleaseMinor) {
    return true;
  }
  if (REMIXAPI_VERSION_GET_MAJOR(clientVersion) != REMIXAPI_VERSION_GET_MAJOR(runtimeVersion)) {
    return false;
  }
  return REMIXAPI_VERSION_GET_MINOR(clientVersion) <= REMIXAPI_VERSION_GET_MINOR(runtimeVersion);
}

// The size in bytes of the interface of 0.1000.x and 1.0.x clients, which ends with GetGameValue.
constexpr size_t kInterfaceSize1_0 = offsetof(remixapi_Interface, GetGameValue) + sizeof(PFN_remixapi_GetGameValue);

// Returns the size in bytes of the remixapi_Interface prefix that a client of the given version allocates.
// Every minor version that appends a function pointer adds a branch here.
constexpr size_t clientInterfaceSize(uint64_t clientVersion) {
  if (clientVersion < REMIXAPI_VERSION_MAKE(1, 1, 0)) {
    return kInterfaceSize1_0;
  }
  return sizeof(remixapi_Interface);
}

static_assert(sizeof(remixapi_Interface) == kInterfaceSize1_0, "Add a branch to clientInterfaceSize for the new interface");

}
