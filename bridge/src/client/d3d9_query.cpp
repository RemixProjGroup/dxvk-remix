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
#include "pch.h"
#include "d3d9_query.h"
#include "client_options.h"
#include "util_devicecommand.h"
#include "util_queryresults.h"

#include <cstring>

Direct3DQuery9_LSS::Direct3DQuery9_LSS(BaseDirect3DDevice9Ex_LSS* const pDevice, D3DQUERYTYPE Type)
  : D3DBase<IDirect3DQuery9>((IDirect3DQuery9*) nullptr, pDevice)
  , m_type(Type)
  , m_pDevice(pDevice) {
  if (!ClientOptions::getAsyncQueries() || bridge_util::QueryResults::block() == nullptr) {
    return;
  }
  // Events, occlusion, timestamps and the small statistics types get their results in a shared slot. Larger types
  // (D3DQUERYTYPE_RESOURCEMANAGER) and slot exhaustion keep the blocking round trip.
  m_asyncResult.init(GetDataSize());
}

HRESULT Direct3DQuery9_LSS::QueryInterface(REFIID riid, LPVOID* ppvObj) {
  LogFunctionCall();
  if (ppvObj == nullptr) {
    return E_POINTER;
  }

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown)
    || riid == __uuidof(IDirect3DQuery9)) {
    *ppvObj = bridge_cast<IDirect3DQuery9*>(this);
    AddRef();
    return S_OK;
  }

  return E_NOINTERFACE;
}

ULONG Direct3DQuery9_LSS::AddRef() {
  LogMissingFunctionCall();
  return D3DBase::AddRef();
}

ULONG Direct3DQuery9_LSS::Release() {
  LogMissingFunctionCall();
  return D3DBase::Release();
}

void Direct3DQuery9_LSS::onDestroy() {
  LogFunctionCall();
  ClientMessage { Commands::IDirect3DQuery9_Destroy, getId() };
  // A request still queued for this slot is processed before any request of a later owner (queue order), and
  // later owners use new sequence numbers, so the slot can be reused at once.
  m_asyncResult.release();
}

HRESULT Direct3DQuery9_LSS::GetDevice(IDirect3DDevice9** ppDevice) {
  LogFunctionCall();
  m_pDevice->reportDevice(ppDevice);
  return S_OK;
}

D3DQUERYTYPE Direct3DQuery9_LSS::GetType() {
  LogFunctionCall();
  return m_type;
}

DWORD Direct3DQuery9_LSS::GetDataSize() {
  LogFunctionCall();
  switch (m_type) {
  case D3DQUERYTYPE_VCACHE:               return sizeof(D3DDEVINFO_VCACHE);
  case D3DQUERYTYPE_RESOURCEMANAGER:      return sizeof(D3DDEVINFO_RESOURCEMANAGER);
  case D3DQUERYTYPE_VERTEXSTATS:          return sizeof(D3DDEVINFO_D3DVERTEXSTATS);
  case D3DQUERYTYPE_EVENT:                return sizeof(BOOL);
  case D3DQUERYTYPE_OCCLUSION:            return sizeof(DWORD);
  case D3DQUERYTYPE_TIMESTAMP:            return sizeof(UINT64);
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:    return sizeof(BOOL);
  case D3DQUERYTYPE_TIMESTAMPFREQ:        return sizeof(UINT64);
  case D3DQUERYTYPE_PIPELINETIMINGS:      return sizeof(D3DDEVINFO_D3D9PIPELINETIMINGS);
  case D3DQUERYTYPE_INTERFACETIMINGS:     return sizeof(D3DDEVINFO_D3D9INTERFACETIMINGS);
  case D3DQUERYTYPE_VERTEXTIMINGS:        return sizeof(D3DDEVINFO_D3D9STAGETIMINGS);
  case D3DQUERYTYPE_PIXELTIMINGS:         return sizeof(D3DDEVINFO_D3D9PIPELINETIMINGS);
  case D3DQUERYTYPE_BANDWIDTHTIMINGS:     return sizeof(D3DDEVINFO_D3D9BANDWIDTHTIMINGS);
  case D3DQUERYTYPE_CACHEUTILIZATION:     return sizeof(D3DDEVINFO_D3D9CACHEUTILIZATION);
  default:                                return 0;
  }
}

HRESULT Direct3DQuery9_LSS::Issue(DWORD dwIssueFlags) {
  LogFunctionCall();
  
  DeviceBridge::ResponseTransaction responseTransaction;

  UID currentUID = 0;

  {
    // The device lock keeps a concurrent GetData from mixing this Issue with an earlier request or result.
    BRIDGE_PARENT_DEVICE_LOCKGUARD();
    ClientMessage c(Commands::IDirect3DQuery9_Issue, getId());
    currentUID = c.get_uid();
    c.send_data(dwIssueFlags);
    m_asyncResult.reset();
  }

  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("Direct3DQuery9_LSS::Issue()", D3DERR_INVALIDCALL, currentUID);

  return S_OK;
}

HRESULT Direct3DQuery9_LSS::GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) {
  LogFunctionCall();
  if (m_asyncResult.enabled()) {
    return getDataAsync(pData, dwSize, dwGetDataFlags);
  }
  return getDataBlocking(pData, dwSize, dwGetDataFlags);
}

HRESULT Direct3DQuery9_LSS::getDataAsync(void* pData, DWORD dwSize, DWORD dwGetDataFlags) {
  BRIDGE_PARENT_DEVICE_LOCKGUARD();
  using bridge_util::perf::QueryPath;
  // The server answers the request in the slot (sv/main.cpp, IDirect3DQuery9_GetData with FlagBits::AsyncResult).
  bool cached = false;
  const HRESULT hresult = m_asyncResult.getData(pData, dwSize, dwGetDataFlags, gbBridgeRunning, cached,
    [this](uint32_t size, DWORD flags, uint32_t slot, uint32_t sequence) {
      ClientMessage c(Commands::IDirect3DQuery9_GetData, getId(), Commands::FlagBits::AsyncResult);
      c.send_data(size);
      c.send_data(flags);
      c.send_data(slot);
      c.send_data(sequence);
    });
  if (bridge_util::perf::enabled()) {
    bridge_util::perf::noteQuery(cached ? QueryPath::AsyncCached : QueryPath::AsyncRequest, hresult == S_OK);
  }
  return hresult;
}

HRESULT Direct3DQuery9_LSS::getDataBlocking(void* pData, DWORD dwSize, DWORD dwGetDataFlags) {
  DeviceBridge::ResponseTransaction responseTransaction;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DQuery9_GetData, getId());
    currentUID = c.get_uid();
    c.send_data(dwSize);
    c.send_data(dwGetDataFlags);
  }

  WAIT_FOR_SERVER_RESPONSE("Direct3DQuery9_LSS::GetData()", D3DERR_INVALIDCALL, currentUID);
  HRESULT hresult = DeviceBridge::get_data();
  // The server sends the data blob whenever the call succeeded and dwSize > 0: consume it even when pData is null,
  // or the response queue's data position falls out of step.
  if (SUCCEEDED(hresult) && dwSize > 0) {
    void* pDataReturned = NULL;
    DeviceBridge::get_data(&pDataReturned);
    if (pData != NULL && pDataReturned != NULL) {
      memcpy(pData, pDataReturned, dwSize);
    }
  }

  DeviceBridge::pop_front();

  if (bridge_util::perf::enabled()) {
    bridge_util::perf::noteQuery(bridge_util::perf::QueryPath::Blocking, hresult == S_OK);
  }
  return hresult;
}
