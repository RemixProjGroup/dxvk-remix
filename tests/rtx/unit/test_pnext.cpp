/*
* Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
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

#define RTX_REMIX_PNEXT_CHECK_STRUCTS

#include <cstring>
#include <vector>

#include "../../../src/dxvk/rtx_render/rtx_remix_pnext.h"
#include "../../../src/dxvk/rtx_render/rtx_fork_api_record_format.h"

// Note: including C++ wrapper to check consistency with C API
#include <remix/remix.h>

#include "../../test_utils.h"

#ifdef _MSC_VER
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define ERROR_INTRO __FUNCTION__ "(" STRINGIFY(__LINE__) "): "
#else
#define ERROR_INTRO ""
#endif

namespace dxvk {
  // Note: Logger needed by some shared code used in this Unit Test.
  Logger Logger::s_instance("test_pnext.log");
}

namespace pnext_test_app {
  using namespace pnext::detail;

  void test_find() {
    auto ext = remixapi_MaterialInfoOpaqueEXT {};
    {
      ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      ext.pNext = nullptr;
    }
    auto info = remixapi_MaterialInfo {};
    {
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &ext;
    }

    if (auto f = pnext::find< remixapi_MaterialInfo >(&info)) {
      if (f != &info) {
        throw dxvk::DxvkError { ERROR_INTRO
          "Result of pnext::find< remixapi_MaterialInfoOpaqueEXT >( remixapi_MaterialInfo{..} )"
          "must match the address of \'info\' variable" };
      }
    } else {
      throw dxvk::DxvkError { ERROR_INTRO
        "pnext::find< remixapi_MaterialInfo >( remixapi_MaterialInfo{..} ) failed" };
    }

    if (auto f = pnext::find< remixapi_MaterialInfoOpaqueEXT >(&info)) {
      if (f != &ext) {
        throw dxvk::DxvkError { ERROR_INTRO
          "Result of pnext::find< remixapi_MaterialInfoOpaqueEXT >( remixapi_MaterialInfo{..} )"
          "must match the address of \'ext\' variable" };
      }
    } else {
      throw dxvk::DxvkError { ERROR_INTRO
        "pnext::find< remixapi_MaterialInfoOpaqueEXT >( remixapi_MaterialInfo{..} ) must return non-null" };
    }

    // must be un-compilable and output a short compilation error
    {
      // pnext::find< remixapi_LightInfo >(&info);
      // pnext::find< remixapi_LightInfoSphereEXT >(&info);
    }
  }

  void test_const() {
    auto ext = remixapi_MaterialInfoOpaqueEXT {};
    {
      ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      ext.pNext = nullptr;
    }
    auto info = remixapi_MaterialInfo {};
    {
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &ext;
    }
    const auto constInfo = remixapi_MaterialInfo { info };

    {
      auto pNext = getPNext(&info);
      static_assert(!std::is_const_v< std::remove_pointer_t< decltype(pNext) > >);

      auto constPNext = getPNext(&constInfo);
      static_assert(std::is_const_v< std::remove_pointer_t< decltype(constPNext) > >);
    }
    {
      auto f = pnext::find< remixapi_MaterialInfoOpaqueEXT >(&info);
      if (f != &ext) {
        throw dxvk::DxvkError{ ERROR_INTRO
          "Result of pnext::find< remixapi_MaterialInfoOpaqueEXT >( remixapi_MaterialInfo{..} )"
          "must match the address of \'ext\' variable" };
      }

      static_assert(!std::is_const_v< std::remove_pointer_t< decltype(f) > >);
    }
    {
      auto cf = pnext::find< remixapi_MaterialInfoOpaqueEXT >(&constInfo);
      if (cf != &ext) {
        throw dxvk::DxvkError { ERROR_INTRO
          "Result of pnext::find< remixapi_MaterialInfoOpaqueEXT >( remixapi_MaterialInfo{..} )"
          "must match the address of \'ext\' variable" };
      }

      static_assert(std::is_const_v< std::remove_pointer_t< decltype(cf) > >);
    }
  }

  void test_getPNext() {
    auto ext = remixapi_LightInfoDistantEXT {};
    {
      ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
      ext.pNext = nullptr;
    }
    auto info = remixapi_LightInfo {};
    {
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &ext;
    }

    if (info.pNext != &ext) {
      throw dxvk::DxvkError { ERROR_INTRO "Unexpected pNext mismatch: info.pNext != &ext" };
    }
    if (getPNext(&info) != info.pNext || getPNext(&ext) != ext.pNext) {
      throw dxvk::DxvkError { ERROR_INTRO "getPNext test fail: result is not equal to .pNext" };
    }
  }

  void test_memberDetection() {
    struct BadType_0 {
      int   sType_none;
      void* pNext;
    };
    struct BadType_1 {
      int   sType;
      void* pNext_none;
    };
    struct BadType_2 {
      int   sType;
    };
    struct BadType_3 {
      void* pNext;
    };
    struct BadType_4 {
      std::underlying_type_t< remixapi_StructType > sType;
      void* pNext;
    };
    struct BadType_5 {
      remixapi_StructType sType;
      uint64_t pNext;
    };
    static_assert(!helper::HasSTypePNext< BadType_0 >);
    static_assert(!helper::HasSTypePNext< BadType_1 >);
    static_assert(!helper::HasSTypePNext< BadType_2 >);
    static_assert(!helper::HasSTypePNext< BadType_3 >);
    static_assert(!helper::HasSTypePNext< BadType_4 >);
    static_assert(!helper::HasSTypePNext< BadType_5 >);

    struct GoodType {
      remixapi_StructType sType;
      void* pNext;
    };
    static_assert(helper::HasSTypePNext< GoodType >);
  }

  void test_decalOrder() {
    auto order = remixapi_InstanceInfoDecalOrderEXT {};
    {
      order.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT;
      order.pNext = nullptr;
      order.sortOrder = 200;
    }
    auto blend = remixapi_InstanceInfoBlendEXT {};
    {
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = &order;
    }
    auto info = remixapi_InstanceInfo {};
    {
      info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      info.pNext = &blend;
    }

    static_assert(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT == 0x4E560007);
    if (pnext::find< remixapi_InstanceInfoDecalOrderEXT >(&info) != &order) {
      throw dxvk::DxvkError { ERROR_INTRO
        "pnext::find< remixapi_InstanceInfoDecalOrderEXT > must find the struct behind another extension" };
    }
    info.pNext = nullptr;
    if (pnext::find< remixapi_InstanceInfoDecalOrderEXT >(&info) != nullptr) {
      throw dxvk::DxvkError { ERROR_INTRO
        "pnext::find< remixapi_InstanceInfoDecalOrderEXT > must return null without the struct" };
    }
  }

  // Writes and reads the fields of plain values, as the recorder and the replayer do. Other kinds of fields are not
  // used by the structs this test sends through it.
  struct BytesIO {
    std::vector<uint8_t> bytes;
    size_t cursor = 0;
    bool isReading = false;

    template<class T> void pod(T& value) {
      if (isReading) {
        std::memcpy(&value, bytes.data() + cursor, sizeof(T));
        cursor += sizeof(T);
      } else {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
      }
    }
    template<class T> void handle(remix_record::HandleKind, T&) { }
    template<class T, class C> void podArray(T*&, C&) { }
    template<class T, class C> void blobArray(T*&, C&) { }
    void blobBytes(const void*&, uint64_t&) { }
    template<class T, class C, class F> void structArray(T*&, C&, F&&) { }
    template<class T> void optPod(const T*&) { }
    void wstr(const wchar_t*&) { }
    void chain(void*&) { }
    uint64_t apiVersion() const { return REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH); }
    bool reading() const { return isReading; }
  };

  void test_decalOrderRecord() {
    if (remix_record::extensionSize(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT) != sizeof(remixapi_InstanceInfoDecalOrderEXT)) {
      throw dxvk::DxvkError { ERROR_INTRO "The recording format must know the size of remixapi_InstanceInfoDecalOrderEXT" };
    }
    auto written = remixapi_InstanceInfoDecalOrderEXT {};
    written.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT;
    written.sortOrder = 173;
    BytesIO io;
    if (!remix_record::ioExtension(io, written.sType, &written)) {
      throw dxvk::DxvkError { ERROR_INTRO "The recording format must write remixapi_InstanceInfoDecalOrderEXT" };
    }
    io.isReading = true;
    auto read = remixapi_InstanceInfoDecalOrderEXT {};
    remix_record::ioExtension(io, written.sType, &read);
    if (read.sortOrder != written.sortOrder || io.cursor != io.bytes.size()) {
      throw dxvk::DxvkError { ERROR_INTRO "remixapi_InstanceInfoDecalOrderEXT must read back as it was written" };
    }
  }

  remixapi_ErrorCode emulated(const remixapi_Rect2D* pixelRegion,
                              PFN_remixapi_pick_RequestObjectPickingUserCallback callback,
                              void* callbackUserData) {
    callback(nullptr, 0, callbackUserData);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void test_wrapper() {
    auto d = remix::Interface {};
    {
      // define C interface to emulate
      d.m_CInterface.pick_RequestObjectPicking = emulated;
    }

    int testvalue = 0;

    // call C++ function
    d.pick_RequestObjectPicking(
      remix::Rect2D {},
      [&testvalue](remix::Span<uint32_t> objectPickingValues) {
        testvalue = 42;
      });

    if (testvalue != 42) {
      throw dxvk::DxvkError { ERROR_INTRO "C++ wrapper test fail: pick_RequestObjectPicking doesn't call lambda callback" };
    }
  }

  namespace test_apiVersionOrdering {
    static_assert(REMIXAPI_VERSION_MAKE(0, 0, 0) < REMIXAPI_VERSION_MAKE(1, 0, 0));
    static_assert(REMIXAPI_VERSION_MAKE(0, 0, 0) < REMIXAPI_VERSION_MAKE(0, 1, 0));
    static_assert(REMIXAPI_VERSION_MAKE(0, 0, 0) < REMIXAPI_VERSION_MAKE(0, 0, 1));

    static_assert(REMIXAPI_VERSION_MAKE(1, 0, 0) < REMIXAPI_VERSION_MAKE(1, 2, 3));
    static_assert(REMIXAPI_VERSION_MAKE(1, 0, 1) < REMIXAPI_VERSION_MAKE(2, 0, 0));
    static_assert(REMIXAPI_VERSION_MAKE(1, 2, 3) < REMIXAPI_VERSION_MAKE(2, 0, 0));
    static_assert(REMIXAPI_VERSION_MAKE(0, 4, 1) < REMIXAPI_VERSION_MAKE(0, 4, 2));
  }
}

int main() {
  try {
    pnext_test_app::test_find();
    pnext_test_app::test_const();
    pnext_test_app::test_getPNext();
    pnext_test_app::test_memberDetection();
    pnext_test_app::test_decalOrder();
    pnext_test_app::test_decalOrderRecord();
    pnext_test_app::test_wrapper();
  }
  catch (const dxvk::DxvkError& error) {
    std::cerr << error.message() << std::endl;
    throw;
  }

  return 0;
}