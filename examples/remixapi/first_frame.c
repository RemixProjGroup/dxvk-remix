// The first frame of the Remix API plugin guide (documentation/RemixApi-Plugin-Guide.md, section 3).
// It opens a window, starts Remix on it, creates one material, one mesh and one light,
// and then draws one instance of the mesh with a camera every frame until the window closes.
//
// Build it from an x64 Native Tools command prompt, in this folder:
//   cl /std:c11 /W4 /I..\..\public\include first_frame.c user32.lib
// It also compiles as C++ (cl /TP).
// Run first_frame.exe from a folder that holds the Remix runtime: d3d9.dll and the files beside it.

#include <remix/remix_c.h>

#include <math.h>
#include <stdio.h>

static remixapi_Float3D float3(float x, float y, float z) {
  remixapi_Float3D v = { x, y, z };
  return v;
}

// A stable 64-bit key from a name (FNV-1a). Remix uses the key as the handle, so it is never 0.
static uint64_t hashName(const char* name) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const char* c = name; *c != '\0'; ++c) {
    hash ^= (uint8_t) *c;
    hash *= 0x100000001b3ull;
  }
  return hash != 0 ? hash : 1;
}

// A rotation about the Y axis followed by a translation, as the 3x4 row-major matrix Remix takes.
// The last column holds the translation.
static remixapi_Transform makeTransform(float angle, float x, float y, float z) {
  const float c = cosf(angle);
  const float s = sinf(angle);
  remixapi_Transform t = { {
    {    c, 0.0f,    s,    x },
    { 0.0f, 1.0f, 0.0f,    y },
    {   -s, 0.0f,    c,    z },
  } };
  return t;
}

// The axes of the six faces of a cube: the face normal, then the two axes across the face.
typedef struct Face {
  float n[3];
  float u[3];
  float v[3];
} Face;

static const Face kFaces[6] = {
  { {  1,  0,  0 }, {  0,  0,  1 }, { 0, 1,  0 } },
  { { -1,  0,  0 }, {  0,  0, -1 }, { 0, 1,  0 } },
  { {  0,  1,  0 }, {  1,  0,  0 }, { 0, 0,  1 } },
  { {  0, -1,  0 }, {  1,  0,  0 }, { 0, 0, -1 } },
  { {  0,  0,  1 }, { -1,  0,  0 }, { 0, 1,  0 } },
  { {  0,  0, -1 }, {  1,  0,  0 }, { 0, 1,  0 } },
};

// Fills 24 vertices and 36 indices of a cube with half size h, centered on the origin.
static void buildCube(float h, remixapi_HardcodedVertex vertices[24], uint32_t indices[36]) {
  static const float kCorner[4][2] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
  for (uint32_t f = 0; f < 6; ++f) {
    const Face* face = &kFaces[f];
    for (uint32_t k = 0; k < 4; ++k) {
      // A zero-filled vertex leaves the padding words zero, as Remix expects.
      remixapi_HardcodedVertex vertex = { 0 };
      for (uint32_t i = 0; i < 3; ++i) {
        vertex.position[i] = (face->n[i] + face->u[i] * kCorner[k][0] + face->v[i] * kCorner[k][1]) * h;
        vertex.normal[i] = face->n[i];
      }
      vertex.texcoord[0] = 0.5f + 0.5f * kCorner[k][0];
      vertex.texcoord[1] = 0.5f - 0.5f * kCorner[k][1];
      vertex.color = 0xFFFFFFFFu;
      vertices[f * 4 + k] = vertex;
    }
    const uint32_t first = f * 4;
    const uint32_t quad[6] = { first, first + 1, first + 2, first, first + 2, first + 3 };
    for (uint32_t i = 0; i < 6; ++i) {
      indices[f * 6 + i] = quad[i];
    }
  }
}

static remixapi_Interface g_remix = { 0 };
static remixapi_HMODULE g_remixDll = NULL;
static remixapi_MaterialHandle g_material = NULL;
static remixapi_MeshHandle g_mesh = NULL;
static remixapi_LightHandle g_light = NULL;

static int check(remixapi_ErrorCode status, const char* call) {
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    printf("%s failed: 0x%X\n", call, (unsigned) status);
    return 0;
  }
  return 1;
}

static int startRemix(HWND hwnd) {
  // Loads d3d9.dll, checks the API version and fills the function table.
  if (!check(remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll", &g_remix, &g_remixDll),
             "remixapi_lib_loadRemixDllAndInitialize")) {
    return 0;
  }

  // Creates the renderer's device and swapchain for the window.
  remixapi_StartupInfo startup = { 0 };
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.hwnd = hwnd;
  startup.combineGuiInFinalColor = 1;
  if (!check(g_remix.Startup(&startup), "Startup")) {
    return 0;
  }

  // An opaque, rough orange material with no textures.
  remixapi_MaterialInfoOpaqueEXT opaque = { 0 };
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = float3(0.8f, 0.3f, 0.1f);
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = 0.4f;
  opaque.metallicConstant = 0.0f;
  opaque.thinFilmThickness_value = 200.0f;
  opaque.useDrawCallAlphaState = 0;
  opaque.alphaTestType = 7;  // Always passes. 0 never passes, and the surface disappears.
  opaque.enableDlssControlMask = 1;
  opaque.dlssControlMaskIntensity = 1.0f;
  opaque.dlssControlMaskToneStrength = 0.3f;
  opaque.dlssControlMaskStructuralStrength = 0.7f;

  remixapi_MaterialInfo material = { 0 };
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = hashName("example/material/orange");
  material.spriteSheetRow = 1;
  material.spriteSheetCol = 1;
  material.filterMode = 1;  // Linear.
  material.wrapModeU = 1;   // Repeat.
  material.wrapModeV = 1;
  if (!check(g_remix.CreateMaterial(&material, &g_material), "CreateMaterial")) {
    return 0;
  }

  // A cube one meter wide. With the default rtx.sceneScale, one unit is one centimeter.
  // Remix copies the vertices and indices during the call, so they can live on the stack.
  remixapi_HardcodedVertex vertices[24];
  uint32_t indices[36];
  buildCube(50.0f, vertices, indices);

  remixapi_MeshInfoSurfaceTriangles surface = { 0 };
  surface.vertices_values = vertices;
  surface.vertices_count = 24;
  surface.indices_values = indices;
  surface.indices_count = 36;
  surface.material = g_material;

  remixapi_MeshInfo mesh = { 0 };
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = hashName("example/mesh/cube");
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  if (!check(g_remix.CreateMesh(&mesh, &g_mesh), "CreateMesh")) {
    return 0;
  }

  // A small white sphere light above and in front of the cube.
  // A light stays in the scene every frame until DestroyLight.
  remixapi_LightInfoSphereEXT sphere = { 0 };
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = float3(100.0f, 150.0f, -150.0f);
  sphere.radius = 10.0f;
  sphere.volumetricRadianceScale = 1.0f;

  remixapi_LightInfo light = { 0 };
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  light.hash = hashName("example/light/key");
  light.radiance = float3(200.0f, 190.0f, 180.0f);
  if (!check(g_remix.CreateLight(&light, &g_light), "CreateLight")) {
    return 0;
  }
  return 1;
}

static void drawFrame(uint32_t width, uint32_t height, float seconds) {
  // The camera looks down +Z from 3 m in front of the cube, with +Y up.
  remixapi_CameraInfoParameterizedEXT params = { 0 };
  params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  params.position = float3(0.0f, 0.0f, -300.0f);
  params.forward = float3(0.0f, 0.0f, 1.0f);
  params.up = float3(0.0f, 1.0f, 0.0f);
  params.right = float3(1.0f, 0.0f, 0.0f);
  params.fovYInDegrees = 60.0f;
  params.aspect = height > 0 ? (float) width / (float) height : 1.0f;
  params.nearPlane = 1.0f;
  params.farPlane = 10000.0f;

  remixapi_CameraInfo camera = { 0 };
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &params;
  camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
  g_remix.SetupCamera(&camera);

  // One instance of the cube, turning slowly. The same mesh handle every frame keeps its history.
  remixapi_InstanceInfo instance = { 0 };
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.categoryFlags = 0;
  instance.mesh = g_mesh;
  instance.transform = makeTransform(seconds * 0.5f, 0.0f, 0.0f, 0.0f);
  instance.doubleSided = 1;
  g_remix.DrawInstance(&instance);

  // Renders the frame and presents it into the window.
  g_remix.Present(NULL);
}

static void stopRemix(void) {
  if (g_remix.Shutdown == NULL) {
    return;
  }
  g_remix.DestroyLight(g_light);
  g_remix.DestroyMesh(g_mesh);
  g_remix.DestroyMaterial(g_material);
  remixapi_lib_shutdownAndUnloadRemixDll(&g_remix, g_remixDll);
}

// Closing the window ends the loop. The window itself is destroyed after Remix shuts down.
static LRESULT WINAPI windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main(void) {
  WNDCLASSEXW windowClass = { 0 };
  windowClass.cbSize = sizeof(windowClass);
  windowClass.style = CS_CLASSDC;
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = GetModuleHandleW(NULL);
  windowClass.lpszClassName = L"RemixApiFirstFrame";
  RegisterClassExW(&windowClass);

  RECT rect = { 0, 0, 1280, 720 };
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowW(windowClass.lpszClassName, L"Remix API first frame", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                            NULL, NULL, windowClass.hInstance, NULL);
  if (hwnd == NULL) {
    return 1;
  }

  int status = 1;
  if (startRemix(hwnd)) {
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    const ULONGLONG start = GetTickCount64();
    MSG msg = { 0 };
    while (msg.message != WM_QUIT) {
      if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        continue;
      }
      RECT client = { 0 };
      GetClientRect(hwnd, &client);
      const float seconds = (float) (GetTickCount64() - start) / 1000.0f;
      drawFrame((uint32_t) (client.right - client.left), (uint32_t) (client.bottom - client.top), seconds);
    }
    status = 0;
  }

  stopRemix();
  DestroyWindow(hwnd);
  UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
  return status;
}
