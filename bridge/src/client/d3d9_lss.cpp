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
#include "pch.h"

#include "version.h"

#include "d3d9_lss.h"
#include "d3d9_surface.h"
#include "d3d9_texture.h"
#include "d3d9_cubetexture.h"
#include "d3d9_swapchain.h"
#include "swapchain_map.h"
#include "client_options.h"
#include "config/config.h"
#include "config/global_options.h"
#include "di_hook.h"
#include "log/log.h"
#include "remix_state.h"
#include "window.h"
#include "message_channels.h"

#include "util_bridge_assert.h"
#include "util_bridge_state.h"
#include "util_common.h"
#include "util_devicecommand.h"
#include "util_modulecommand.h"
#include "util_filesys.h"
#include "util_hack_d3d_debug.h"
#include "util_messagechannel.h"
#include "util_seh.h"
#include "util_semaphore.h"

#include "../../../src/util/util_crash_report.h"

#include <algorithm>
#include <assert.h>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdio.h>
#include <string>
#include <mutex>
#include <vector>

using namespace bridge_util;

std::atomic<uintptr_t> D3dBaseIdFactory::id_counter = 1;
uintptr_t D3dBaseIdFactory::getNextId() {
  return id_counter++;
}

#if defined(_DEBUG) || defined(DEBUGOPT)

std::map<std::thread::id, std::atomic<size_t>> FunctionEntryExitLogger::s_counters;

FunctionEntryExitLogger::FunctionEntryExitLogger(const std::string functionName, void* thiz) {
  if (GlobalOptions::getLogApiCalls() || GlobalOptions::getLogAllCalls()) {
    
    std::string tabs = "";
    if (s_counters.count(std::this_thread::get_id()) == 0) {
      s_counters[std::this_thread::get_id()] = 0;
    }
    std::atomic<size_t>& counter = s_counters[std::this_thread::get_id()];
    for (int i = 0; i < counter; i++) {
      tabs.append("\t");
    }
    if (!GlobalOptions::getLogAllCalls()) {
      if (counter == 0) {
        _LogFunctionCall(functionName, thiz);
      }
    }
    else {
      _LogFunctionCall(tabs + functionName + " ENTRY", thiz);
    }
    counter++;
    m_functionName = functionName;
    m_thiz = thiz;
  }

}

FunctionEntryExitLogger::~FunctionEntryExitLogger() {
  if (GlobalOptions::getLogApiCalls() || GlobalOptions::getLogAllCalls()) {
    std::atomic<size_t>& counter = s_counters[std::this_thread::get_id()];
    counter--;
    if (!GlobalOptions::getLogAllCalls()) {
      return;
    }
    std::string tabs = "";
    for (int i = 0; i < counter; i++) {
      tabs.append("\t");
    }
    _LogFunctionCall(tabs + m_functionName + " EXIT", m_thiz);
  }
}

#endif

static bool gIsAttached = false;
Guid gUniqueIdentifier;
Process* gpServer = nullptr;
NamedSemaphore* gpPresent = nullptr;
ShadowMap gShadowMap;
std::mutex gShadowMapMutex;
std::mutex serverStartMutex;
SceneState gSceneState = WaitBeginScene;
std::chrono::steady_clock::time_point gTimeStart;
bool gbBridgeRunning = true;
std::string gRemixFolder = "";
std::mutex gSwapChainMapMutex;
std::mutex gPresentMutex;
SwapChainMap gSwapChainMap;
const char* gServerExecutable = "NvRemixBridge.exe";

static fspath GetServerExecutablePath() {
  ONCE(Logger::info(std::string("Native arch: ") + getNativeArchString()));

  auto arch = bridge_util::Config::getOption<bool>("forceX64Server", false) ? nullptr : getNativeArchString();
  const fspath remixFolder(gRemixFolder);
  return remixFolder / dxvk::util::RtxFileSys::runtimeDirNameIn(remixFolder, arch) / gServerExecutable;
}

void PrintRecentCommandHistory() {
  // Log history of recent client side commands sent and received by the server
  Logger::info("Most recent Device Queue commands sent from Client");
  DeviceBridge::Command::print_writer_data_sent();
  Logger::info("Most recent Device Queue commands received by Server");
  DeviceBridge::Command::print_writer_data_received();
  Logger::info("Most recent Module Queue commands sent from Client");
  ModuleBridge::Command::print_writer_data_sent();
  Logger::info("Most recent Module Queue commands received by Server");
  ModuleBridge::Command::print_writer_data_received();
}

// Setup bridge exception handler if requested
void SetupExceptionHandler() {
  if (ClientOptions::getSetExceptionHandler()) {
    ExceptionHandler::get().init();
  }
}

namespace {
  // Shows the crash dialog, then exits. The dialog only informs: crash reports stay on disk.
  [[noreturn]] void showCrashDialogAndExit(bool isGpuCrash) {
    const auto logsDir = dxvk::util::RtxFileSys::path(dxvk::util::RtxFileSys::Logs);
    const dxvk::CrashPromptKind promptKind = isGpuCrash ? dxvk::CrashPromptKind::GpuImmediate : dxvk::CrashPromptKind::CpuImmediate;
    const std::wstring message = dxvk::buildCrashReportMessage(promptKind, logsDir);

    // Prepare the game window so the dialog is visible on top (demote from topmost + minimize, plus
    // ghost suppression / WndProc fullscreen-restore suppression). This runs on the process-monitor
    // threadpool thread — NOT the window-owning main thread — so use the async path to avoid blocking
    // on the main thread, which may be stuck in the Present wait and not pumping messages. For GPU
    // crashes the early handler has usually already done this; this call is the backstop and also
    // covers CPU crashes (which have no early handler).
    WndProc::prepareForCrashDialog(/*async=*/true);

    Logger::err(isGpuCrash
        ? std::string("Crash dialog: GPU crash, see the logs in ") + logsDir.string()
        : std::string("Crash dialog: crash report saved to ") + dxvk::crashReportsDir(logsDir).string());
    MessageBoxW(nullptr, message.c_str(), dxvk::kCrashReportDialogTitle, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);

    // Not std::exit(): its static destructors race with the game's own threads tearing down D3D9
    // objects (e.g. D3DBase<T>::~D3DBase touches gShadowMap).
    TerminateProcess(GetCurrentProcess(), static_cast<DWORD>(-1));
    // TerminateProcess is asynchronous, so park here rather than returning into those same
    // destructors. ExitProcess covers termination never landing.
    for (int i = 0; i < 5; i++) {
      Sleep(1000);
    }
    ExitProcess(static_cast<DWORD>(-1));
  }

  // Without a server nothing can render, and the handshake would wait for it forever with no window on screen.
  [[noreturn]] void showServerLaunchErrorAndExit(const std::string& serverPath, DWORD error) {
    const std::string message = format_string(
      "The RTX Remix bridge server could not be started (Windows error %lu).\n\n%s\n\n"
      "Check that this file exists. If it is missing, your antivirus may have quarantined or blocked it: "
      "restore it, add the game folder to the antivirus exclusions, and reinstall the mod files.",
      error, serverPath.c_str());
    Logger::err(format_string("Bridge server launch failed with error %lu, exiting: %s", error, serverPath.c_str()));
    MessageBoxA(nullptr, message.c_str(), "RTX Remix", MB_OK | MB_ICONERROR | MB_TOPMOST);

    // Terminates rather than exits, because the DLL detach path would try to talk to the missing server.
    TerminateProcess(GetCurrentProcess(), static_cast<DWORD>(-1));
    for (;;) {
      Sleep(1000);
    }
  }
}

void OnServerExited(Process const*, DWORD exitCode) {
  BridgeState::setServerState(BridgeState::ProcessState::Exited);

  // Stop the WndProc from restoring the fullscreen window during crash handling.
  // When the server exits while the game's render loop is running (server signalled Present
  // before crashing), the game keeps calling D3D9 functions and its own window management fights
  // any forced minimize — the WM_SIZE → WM_ACTIVATEAPP loop keeps re-expanding the window to
  // fullscreen. Suppressing it here prevents that before we disable the bridge or show the dialog.
  WndProc::setSuppressWindowManagement(true);
  Logger::warn("Crash handler: window management suppressed, ghosting disabled");

  // Suppress the Windows "not responding" ghost dialog for the remainder of this process lifetime.
  // The game's main thread is blocked in the Present semaphore wait and cannot pump messages, so
  // Windows will otherwise offer to kill the process before our crash dialog can appear.
  DisableProcessWindowsGhosting();

  // Disable the bridge to terminate any ongoing processing
  gbBridgeRunning = false;
  // Notify the user that we have to shut down the bridge entirely because we don't have a renderer anymore
  if (BridgeState::getClientState() != BridgeState::ProcessState::DoneProcessing) {
    // The runtime signals GPU crashes (device loss) via a dedicated exit code so we can show a
    // GPU-specific dialog instead of the generic crash prompt.
    const bool isGpuCrash = (exitCode == dxvk::kRemixGpuCrashExitCode);
    Logger::warn(format_string("Crash handler: server exited (code=0x%08x, type=%s)",
        exitCode, isGpuCrash ? "GPU" : "CPU"));
    PrintRecentCommandHistory();
    showCrashDialogAndExit(isGpuCrash);
  }

  const auto timeServerEnd = std::chrono::high_resolution_clock::now();
  std::stringstream uptimeSS;
  uptimeSS << "[Uptime] Server (estimated): ";
  uptimeSS <<
    std::chrono::duration_cast<std::chrono::seconds>(timeServerEnd - gTimeStart).count();
  uptimeSS << "s";
  Logger::info(uptimeSS.str());
}

void InitServer() {
  std::lock_guard<std::mutex> guard(serverStartMutex);
  if (gpServer != nullptr) {
    return;
  }
  Logger::info("Launching server with GUID " + gUniqueIdentifier.toString());
  const std::string serverPath = GetServerExecutablePath().string();
  std::stringstream cmdSS;
  cmdSS << serverPath;
  cmdSS << " " << gUniqueIdentifier.toString();
  cmdSS << " " << BRIDGE_VERSION;
  cmdSS << " " << std::string(GetCommandLineA());
  const std::string command = cmdSS.str();
  gpServer = new Process(command.c_str(), OnServerExited);
  if (!gpServer->isLaunched()) {
    showServerLaunchErrorAndExit(serverPath, gpServer->getLaunchError());
  }

  if (ClientOptions::getEnableDpiAwareness()) {
    Logger::info("Process set as DPI aware");
    static HINSTANCE shcore_dll = ::LoadLibraryA("shcore.dll");
    if (shcore_dll != NULL) {
      typedef HRESULT(WINAPI* PFN_SetProcessDpiAwareness)(int);
      if (PFN_SetProcessDpiAwareness SetProcessDpiAwarenessFn = (PFN_SetProcessDpiAwareness)::GetProcAddress(shcore_dll, "SetProcessDpiAwareness")) {
        constexpr int PROCESS_PER_MONITOR_DPI_AWARE = 2;
        SetProcessDpiAwarenessFn(PROCESS_PER_MONITOR_DPI_AWARE);
      } else {
        SetProcessDPIAware();
      }
    } else {
      DWORD errorCode = GetLastError();
      Logger::err("Fail shcore.dll, error code: " + std::to_string(errorCode));
    }
  }

  BridgeState::setServerState(BridgeState::ProcessState::Init);

  // Initialize our shared queue as a Reader.
  Logger::info("Sending SYN command, waiting for ACK from server...");
  ClientMessage { Commands::Bridge_Syn, (uintptr_t) gpServer->GetCurrentProcessHandle() };

  BridgeState::setClientState(BridgeState::ProcessState::Handshaking);
  const auto waitForAckResult = DeviceBridge::waitForCommand(Commands::Bridge_Ack, GlobalOptions::getStartupTimeout());
  switch (waitForAckResult) {
  case Result::Timeout:
  {
    Logger::err("Timeout. Connection not established to server.");
    Logger::err("Are you sure the server was invoked by this application and is running?");
    BridgeState::setServerState(BridgeState::ProcessState::DoneProcessing);
    gbBridgeRunning = false;
    return;
  }
  case Result::Failure:
  {
    Logger::err("Failed to connect to server.");
    BridgeState::setServerState(BridgeState::ProcessState::DoneProcessing);
    gbBridgeRunning = false;
    return;
  }
  }
  // Remove Ack from queue and get thread id for thread proc message handler from server
  const auto ackResponse = DeviceBridge::pop_front();
  initServerMessageChannel(ackResponse.pHandle);

  BridgeState::setServerState(BridgeState::ProcessState::Handshaking);
  Logger::info("Ack received! Handshake completed! Telling server to continue waiting for commands...");
  {
    // The server adopts the client's response policy, so the two sides always agree on which calls get answered
    // (GlobalOptions::applyResponsePolicyFlags).
    ClientMessage c(Commands::Bridge_Continue);
    c.send_data(GlobalOptions::getResponsePolicyFlags());
  }

#ifdef DEBUG
  {
    const char* text = "Hello";
    ClientMessage command(Commands::Bridge_DebugMessage);
    command.send_data(42);
    command.send_data(strlen(text), (void*) text);
  }
  {
    const char* text = "World!";
    ClientMessage command(Commands::Bridge_DebugMessage);
    command.send_data(1313);
    command.send_data(strlen(text), (void*) text);
  }
  {
    const char* text = "Test!";
    ClientMessage command(Commands::Bridge_DebugMessage);
    command.send_data(4090);
    command.send_data(strlen(text), (void*) text);
  }
#endif
  BridgeState::setClientState(BridgeState::ProcessState::Running);
  BridgeState::setServerState(BridgeState::ProcessState::Running);
  
  if (GlobalOptions::getUseSharedHeap()) {
    SharedHeap::init();
  }
}

bool InitRemixFolder(HMODULE hinst) {
  if (!gRemixFolder.empty()) {
    return true;
  }

  DWORD len;
  std::string tmp(MAX_PATH, '\0');
  do {
    len = GetModuleFileNameA(hinst, tmp.data(), tmp.length());

    if (len > 0 && len < tmp.length()) {
      break;
    }

    tmp.resize(tmp.length() * 2);
  } while (true);

  // Trim filename
  while (len) {
    if (tmp[len] == '\\' || tmp[len] == '/') {
      tmp.resize(len + 1);
      break;
    }
    --len;
  }

  gRemixFolder = tmp;

  return true;
}

bool initFileSys(const HMODULE hModule) {
  const auto hModulePath = getModuleFilePath(hModule);
  bool bNeedToFindExecutable = hModulePath.extension().compare(".exe") != 0;
  fspath executablePath = "";
  if (bNeedToFindExecutable) {
    auto executablePathVec = createPathVec();
    if(GetModuleFileName(NULL,executablePathVec.data(), executablePathVec.size()) == 0) {
      Logger::err("Failed to find executable path!");
      return false;
    }
    executablePath = fspath(executablePathVec.data());
  }
  const auto exeDir = executablePath.parent_path();
  dxvk::util::RtxFileSys::init(exeDir.string());
  return true;
}

/*
 * Public exports needed for D3D
 */
extern "C" {

  HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppDeviceEx) {
    return LssDirect3DCreate9Ex(SDKVersion, ppDeviceEx);
  }

  IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    return LssDirect3DCreate9(SDKVersion);
  }

  DLLEXPORT int __stdcall D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) {
    return 0;
  }

  DLLEXPORT int __stdcall D3DPERF_EndEvent(void) {
    return 0;
  }

  DLLEXPORT void __stdcall D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) {
  }

  DLLEXPORT void __stdcall D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) {
  }

  DLLEXPORT BOOL __stdcall D3DPERF_QueryRepeatFrame(void) {
    return FALSE;
  }

  DLLEXPORT void __stdcall D3DPERF_SetOptions(DWORD dwOptions) {
  }

  DLLEXPORT DWORD __stdcall D3DPERF_GetStatus(void) {
    return 0;
  }

  DLLEXPORT void __stdcall DebugSetMute(void) {
  }

  DLLEXPORT int __stdcall DebugSetLevel(void) {
    return 0;
  }

  DLLEXPORT int __stdcall Direct3D9EnableMaximizedWindowedModeShim(UINT a) {
    return 0;
  }

  // A proxy d3d9.dll that wraps a device of this client registers its own device and IDirect3D9 here, so that
  // GetDevice on every object of that device and IDirect3DDevice9::GetDirect3D return the proxy's objects.
  // The client keeps no references to them, so the proxy clears the registration (null pointers) before its
  // final release. pOuterDirect3D may be null on its own.
  // Returns E_INVALIDARG when pInner is not a live device of this client.
  DLLEXPORT HRESULT __stdcall BridgeClient_SetOuterDevice(IDirect3DDevice9* pInner, IDirect3DDevice9* pOuterDevice,
                                                          IDirect3D9* pOuterDirect3D) {
    return BaseDirect3DDevice9Ex_LSS::setOuterObjects(pInner, pOuterDevice, pOuterDirect3D) ? S_OK : E_INVALIDARG;
  }
}

bool RemixAttach(HMODULE hModule) {
  if (!gIsAttached) {
    // Sort out module/library handles
    if(!hModule) {
      const DWORD getHandleFlags = GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
      if (!GetModuleHandleEx(getHandleFlags, NULL, &hModule)) {
        Logger::err("Unable to find module handle...");
        return false;
      }
    }
    
    // Initialize options
    Config::init(Config::App::Client, hModule);
    GlobalOptions::init();

    // Find path of original executable to properly set up paths
    if (!initFileSys(hModule)) {
      Logger::err("Failed to initialize rtx filesystem!");
    }

    // Initialize logger
    Logger::init();

    // Setup Remix folder first hand
    if (!InitRemixFolder(hModule)) {
      Logger::err("Fatal: Unable to initialize Remix folder...");
      return false;
    }

    // Initialize WndProc logic
    if(!WndProc::init()) {
      Logger::warn("Failed to detour WndProc setter/getter. Remix UI will likely not work.");
    }

    SetupExceptionHandler();

    // Identify yourself
    Logger::info("==================\nNVIDIA RTX Remix Bridge Client\n==================");
    Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
    auto clientPath = getModuleFilePath();
    Logger::info(format_string("Loaded d3d9.dll from %s", clientPath.c_str()));

    DInputHookAttach();

    initRemixMessageChannel();
    RemixState::init(*gpRemixMessageChannel);

    initModuleBridge();
    initDeviceBridge();

    // The client creates this semaphore before it launches the server, so these counts are the ones that apply (the
    // server's are ignored when it opens the existing semaphore). Initial count = presentSemaphoreMaxFrames: the
    // client may run that many frames ahead of the server. 0 = lock-step (each Present waits for the server's).
    gpPresent = NamedSemaphore::createPresent(GlobalOptions::getPresentSemaphoreMaxFrames());

    BridgeState::setClientState(BridgeState::ProcessState::Init);

    // Deprecated config options, will be removed in future versions!!!
    if (bridge_util::Config::isOptionDefined("client.shaderVersion")) {
      Logger::warn("[deprecated-config] 'client.shaderVersion' has been deprecated, please use d3d9.shaderModel in the dxvk.conf instead");
    }

    if (bridge_util::Config::isOptionDefined("client.maxActiveLights")) {
      Logger::warn("[deprecated-config] 'client.maxActiveLights' has been deprecated, please use d3d9.maxActiveLights in the dxvk.conf instead");
    }

#ifdef WITH_INJECTION
    void RemixDetach();
    atexit(RemixDetach);
#endif

    gIsAttached = true;
  }

  return true;
}

void RemixDetach() {
  if (gIsAttached) {
    WndProc::terminate();
    BridgeState::setClientState(BridgeState::ProcessState::DoneProcessing);
    Logger::info("About to unload bridge client.");

    if (gpServer) {
      // Instruct the server to wrap things up and bail
      // Note that while we can queue up the command the semaphore doesn't work anymore at this point
      Logger::info("Sending Terminate command to server...");
      // Unregister exit callback handler first so we don't trigger it when exiting the server normally
      gpServer->UnregisterExitCallback();
      // Send Terminate command immediately before we clean up resources
      {
        ClientMessage { Commands::Bridge_Terminate };
      }

      const auto result = DeviceBridge::waitForCommandAndDiscard(Commands::Bridge_Ack,
                                                                  GlobalOptions::getCommandTimeout());
      if (RESULT_SUCCESS(result)) {
        Logger::info("Server notified that it has cleanly terminated. Cleaning up.");
      } else {
        Logger::err("Timeout waiting for clean server termination. Moving ahead anyway.");
      }

      delete gpServer;
    }

    PrintRecentCommandHistory();

    // Clean up resources
    delete gpPresent;

    Logger::info("Shutdown cleanup successful, exiting now!");
    BridgeState::setClientState(BridgeState::ProcessState::Exited);

    DInputHookDetach();

    gIsAttached = false;
  }
}

static bool shouldAttachToCurrentProcess() {
  std::string targetProcess = ClientOptions::getTargetProcess();
  if (targetProcess.empty()) {
    return true;
  }

  char exePath[MAX_PATH];
  DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
  if (len == 0) return true;

  std::string currentExe(exePath);
  auto pos = currentExe.find_last_of("\\/");
  if (pos != std::string::npos) {
    currentExe = currentExe.substr(pos + 1);
  }

  std::transform(currentExe.begin(), currentExe.end(), currentExe.begin(),
    [](unsigned char c) { return std::tolower(c); });
  std::transform(targetProcess.begin(), targetProcess.end(), targetProcess.begin(),
    [](unsigned char c) { return std::tolower(c); });

  return currentExe == targetProcess;
}

/*
 * Direct3D9 Interface Implementation
 */

HRESULT LssDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppDeviceEx) {
  if (!shouldAttachToCurrentProcess()) {
    return orig_Direct3DCreate9Ex(SDKVersion, ppDeviceEx);
  }
  if (!RemixAttach(NULL)) {
    return D3DERR_NOTAVAILABLE;
  }

  // A game may override client's exception handler when it was setup early.
  // Attempt to restore the exeption handler.
  SetupExceptionHandler();

  (*ppDeviceEx) = new Direct3D9Ex_LSS((IDirect3D9Ex*) nullptr);
  InitServer();
  return S_OK;
}

IDirect3D9* LssDirect3DCreate9(UINT SDKVersion) {
  if (!shouldAttachToCurrentProcess()) {
    return orig_Direct3DCreate9(SDKVersion);
  }
  if (!RemixAttach(NULL)) {
    return nullptr;
  }

  // A game may override client's exception handler when it was setup early.
  // Attempt to restore the exeption handler.
  SetupExceptionHandler();

  auto retval = new Direct3D9Ex_LSS((IDirect3D9*) nullptr);
  InitServer();
  return retval;
}
