#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "dxvk_device.h"
#include "dxvk_limits.h"
#include "rtx_auto_exposure.h"
#include "rtx_context.h"
#include "rtx_external_effects.h"
#include "rtx_imgui.h"
#include "rtx_postFx.h"

#include "../imgui/imgui.h"
#include "../../util/util_env.h"
#include "../../util/util_global_time.h"

namespace dxvk {

  namespace {
    constexpr uint32_t kInputBinding = 0;
    constexpr uint32_t kOutputBinding = 1;
    constexpr uint32_t kLinearDepthBinding = 2;
    constexpr uint32_t kMotionBinding = 3;
    constexpr uint32_t kWorldNormalBinding = 4;
    constexpr uint32_t kAlbedoBinding = 5;
    constexpr uint32_t kRoughnessBinding = 6;
    constexpr uint32_t kSurfaceFlagsBinding = 7;
    constexpr uint32_t kObjectPickingBinding = 8;
    constexpr uint32_t kConeRadiusBinding = 9;
    constexpr uint32_t kBlueNoiseBinding = 10;
    constexpr uint32_t kExposureBinding = 11;
    constexpr uint32_t kSceneSamplerBinding = 12;
    constexpr uint32_t kFrameDataBinding = 13;
    constexpr uint32_t kSceneLinearSamplerBinding = 14;
    constexpr uint32_t kFocusStateBinding = 15;
    constexpr uint32_t kProjectedDepthBinding = 16;
    // 17 is deliberately left free: scene bindings own 0..16 and adding one
    // later must not move the parameter buffer out from under shipped effects.
    constexpr uint32_t kReservedBinding = 17;
    constexpr uint32_t kParameterDataBinding = 18;
    // User textures. One index per declared texture, reachable as a sampled
    // image at kUserTextureReadBinding + index and as a storage image at
    // kUserTextureWriteBinding + index. Keeping one index across both ranges
    // is what lets a '-D' define carry a texture's identity to the shader
    // without the shader having to know which passes read or write it.
    constexpr uint32_t kUserTextureReadBinding = 32;
    constexpr uint32_t kUserTextureWriteBinding = 48;
    constexpr uint32_t kCompilerTimeoutMilliseconds = 30000;
    // slangc diagnostics can run long for a template-heavy file; keep enough to
    // be useful in the panel without turning one broken effect into a wall.
    constexpr size_t kMaxCompilerOutputCharacters = 4096;
    // Quiet period after the last change under the search path before a
    // rebuild starts. Editors write a file more than once per save - a
    // temporary, a rename, sometimes a separate truncate - and each of those
    // is its own notification; a rebuild started on the first would read a
    // half-written file.
    constexpr int64_t kHotReloadDebounceMilliseconds = 250;

    int64_t steadyMilliseconds() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    enum ExternalEffectInput : uint32_t {
      ExternalEffectInputLinearDepth = 1 << 0,
      ExternalEffectInputMotion = 1 << 1,
      ExternalEffectInputWorldNormal = 1 << 2,
      ExternalEffectInputAlbedo = 1 << 3,
      ExternalEffectInputRoughness = 1 << 4,
      ExternalEffectInputSurfaceFlags = 1 << 5,
      ExternalEffectInputObjectPicking = 1 << 6,
      ExternalEffectInputConeRadius = 1 << 7,
      ExternalEffectInputBlueNoise = 1 << 8,
      ExternalEffectInputExposure = 1 << 9,
      ExternalEffectInputFocusState = 1 << 10,
      ExternalEffectInputProjectedDepth = 1 << 11,
    };

    // Only per-dispatch state lives here now. Parameters moved to their own
    // uniform buffer, which is what frees the bank for the pass-local fields a
    // multi-pass effect needs: passIndex and the extent this pass runs over,
    // which stops matching imageSize as soon as passes can run at a divisor.
    struct ExternalEffectArgs {
      uint32_t imageSize[2];
      float invImageSize[2];
      uint32_t dispatchSize[2];
      float timeSeconds;
      uint32_t frameIndex;
      uint32_t passIndex;
      uint32_t padding;
    };

    static_assert(sizeof(ExternalEffectArgs) == 40);
    // DxvkContext::pushConstants only asserts this bound, so a release build
    // would silently overrun the push-constant bank if the struct ever grew.
    static_assert(sizeof(ExternalEffectArgs) <= DxvkLimits::MaxPushConstantSize);

    struct ExternalEffectParameterData {
      float values[RtxExternalEffects::kMaxParameterValues];
      uint32_t valueCount;
      // Tail padding only: the shader-side block rounds up to 16 bytes under
      // std140 and the bound range has to cover that rounded size.
      uint32_t padding[3];
    };

    static_assert(sizeof(ExternalEffectParameterData) == 4112);
    // maxUniformBufferRange is only guaranteed to be 16 KiB, and a cap that
    // could not actually be bound would be a cap in name only.
    static_assert(sizeof(ExternalEffectParameterData) <= 16384);

    struct ExternalEffectFrameData {
      mat4 worldToView;
      mat4 viewToWorld;
      mat4 viewToProjection;
      mat4 projectionToView;
      mat4 viewToProjectionJittered;
      mat4 projectionToViewJittered;
      mat4 worldToProjectionJittered;
      mat4 projectionToWorldJittered;
      mat4 translatedWorldToView;
      mat4 translatedWorldToProjectionJittered;
      mat4 projectionToTranslatedWorld;
      mat4 previousWorldToView;
      mat4 previousViewToWorld;
      mat4 previousWorldToProjection;
      mat4 previousWorldToProjectionJittered;
      mat4 previousProjectionToView;
      mat4 previousProjectionToViewJittered;
      mat4 previousTranslatedWorldToView;
      mat4 previousTranslatedWorldToProjection;
      mat4 projectionToPreviousProjectionJittered;
      uint32_t outputSize[2];
      uint32_t renderSize[2];
      float invRenderSize[2];
      float nearPlane;
      float meterToWorldScale;
      uint32_t cameraFlags;
      uint32_t availableInputs;
      float linearDepthMissValue;
      float deltaTimeSeconds;
      float manualFocusDistance;
      float autoFocusOffset;
      uint32_t autoFocusEnabled;
      // Per frame, not per pass, which is why it lives here rather than in the
      // push bank: a multi-pass effect must see the same answer in every pass
      // or one half of it would reset while the other accumulated.
      uint32_t historyInvalid;
      uint32_t cameraCut;
      uint32_t padding[3];
    };

    static_assert(sizeof(ExternalEffectFrameData) == 1360);
    // std140 rounds the block to a multiple of 16 and scalar layout does not,
    // so the two only agree on the bound range if the struct is already a
    // multiple of 16. The padding above is what makes that true.
    static_assert(sizeof(ExternalEffectFrameData) % 16 == 0);

    bool endsWith(const std::string& value, const std::string& suffix) {
      return value.size() >= suffix.size()
          && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string fallbackIdFromPath(const std::filesystem::path& path) {
      constexpr const char* kSuffix = ".remixfx.slang";
      const std::string filename = path.filename().u8string();
      return endsWith(filename, kSuffix)
        ? filename.substr(0, filename.size() - std::char_traits<char>::length(kSuffix))
        : path.stem().u8string();
    }

    // One module per entry point, so the cache key has to carry the entry name
    // as well as the source. Putting it in the filename means the mtime
    // comparison that already guards the single-entry case keeps working
    // unchanged for every pass.
    std::filesystem::path spirvPathForPass(
      const std::filesystem::path& path, const std::string& entryPoint) {
      const std::string id = fallbackIdFromPath(path);
      return path.parent_path() / (id + ".remixfx." + entryPoint + ".spv");
    }

    // slangc's '-depfile' output, written beside the module it describes. It
    // is what makes editing a '.slangh' rebuild the effects that include it:
    // without it the only input with a tracked mtime is the '.remixfx.slang'
    // itself, so a change to the shared binding header rebuilds nothing.
    std::filesystem::path dependencyPathForPass(const std::filesystem::path& spirvPath) {
      std::filesystem::path result = spirvPath;
      result += ".d";
      return result;
    }

    // A Make-style rule: '<target>: <dep> <dep>'. Separators are unescaped
    // whitespace and a backslash escapes the character after it, which is how
    // a Windows path survives at all - slangc writes 'C\:\\dir\\file.slang'.
    // The target is dropped: it is the file we asked slangc for.
    std::vector<std::filesystem::path> parseDependencyFile(const std::filesystem::path& path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return {};
      }
      const std::string text(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

      std::vector<std::filesystem::path> dependencies;
      std::string token;
      bool pastTarget = false;
      const auto flush = [&]() {
        if (!token.empty()) {
          if (pastTarget) {
            dependencies.push_back(std::filesystem::u8path(token));
          }
          token.clear();
        }
      };

      for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
          const char next = text[i + 1];
          // A backslash before a newline is a line continuation, not an
          // escape of the newline character.
          if (next == '\n' || next == '\r') {
            flush();
            i++;
            continue;
          }
          token.push_back(next);
          i++;
          continue;
        }
        if (!pastTarget && c == ':') {
          flush();
          pastTarget = true;
          continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          flush();
          continue;
        }
        token.push_back(c);
      }
      flush();

      return dependencies;
    }

    VkFormat toVkFormat(RtxExternalEffectTextureFormat format) {
      switch (format) {
      case RtxExternalEffectTextureFormat::R8: return VK_FORMAT_R8_UNORM;
      case RtxExternalEffectTextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
      case RtxExternalEffectTextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
      case RtxExternalEffectTextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
      case RtxExternalEffectTextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
      case RtxExternalEffectTextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
      case RtxExternalEffectTextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
      case RtxExternalEffectTextureFormat::RG32F: return VK_FORMAT_R32G32_SFLOAT;
      case RtxExternalEffectTextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
      case RtxExternalEffectTextureFormat::R32U: return VK_FORMAT_R32_UINT;
      case RtxExternalEffectTextureFormat::RG32U: return VK_FORMAT_R32G32_UINT;
      case RtxExternalEffectTextureFormat::RGBA32U: return VK_FORMAT_R32G32B32A32_UINT;
      case RtxExternalEffectTextureFormat::R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
      }
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    VkExtent3D textureExtent(
      const RtxExternalEffectTexture& texture, const VkExtent3D& outputExtent) {
      if (texture.divisor == 0) {
        return VkExtent3D { texture.width, texture.height, 1 };
      }
      // Ceil, not floor: a 'div 2' texture of an odd-width target still has to
      // cover the last column, and ceil-divides nest exactly so a 'div 32'
      // tile grid lands on whole 'div 2' workgroups.
      return VkExtent3D {
        std::max(util::ceilDivide(outputExtent.width, texture.divisor), 1u),
        std::max(util::ceilDivide(outputExtent.height, texture.divisor), 1u),
        1u
      };
    }

    // What the module says a binding is, as opposed to what the manifest says
    // it should be. Only the categories the runtime actually hands out are
    // distinguished; anything else reflects as Unknown and is left alone,
    // because a check that cannot tell what it is looking at should not be the
    // thing that refuses to load an effect.
    enum class ReflectedDescriptorType {
      Unknown,
      Sampler,
      SampledImage,
      CombinedImageSampler,
      StorageImage,
      UniformBuffer,
      StorageBuffer,
    };

    ReflectedDescriptorType toReflectedType(VkDescriptorType type) {
      switch (type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER: return ReflectedDescriptorType::Sampler;
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return ReflectedDescriptorType::SampledImage;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return ReflectedDescriptorType::CombinedImageSampler;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return ReflectedDescriptorType::StorageImage;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return ReflectedDescriptorType::UniformBuffer;
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return ReflectedDescriptorType::StorageBuffer;
      default: return ReflectedDescriptorType::Unknown;
      }
    }

    // The Slang spelling, for the ranges where there is exactly one. A user
    // texture is always a 2D image of one kind or another, so naming the type
    // an author would have typed is more use than naming the Vulkan concept
    // they never see.
    const char* shaderTypeName(ReflectedDescriptorType type) {
      switch (type) {
      case ReflectedDescriptorType::Sampler: return "SamplerState";
      case ReflectedDescriptorType::SampledImage: return "Texture2D";
      case ReflectedDescriptorType::CombinedImageSampler: return "Sampler2D";
      case ReflectedDescriptorType::StorageImage: return "RWTexture2D";
      case ReflectedDescriptorType::UniformBuffer: return "ConstantBuffer";
      case ReflectedDescriptorType::StorageBuffer: return "StructuredBuffer";
      default: return "an unrecognised type";
      }
    }

    // The Vulkan concept, for the scene bindings, where the dimensionality
    // varies and a Slang spelling would be a guess.
    const char* descriptorTypeName(ReflectedDescriptorType type) {
      switch (type) {
      case ReflectedDescriptorType::Sampler: return "a sampler";
      case ReflectedDescriptorType::SampledImage: return "a sampled image";
      case ReflectedDescriptorType::CombinedImageSampler: return "a combined image sampler";
      case ReflectedDescriptorType::StorageImage: return "a storage image";
      case ReflectedDescriptorType::UniformBuffer: return "a uniform buffer";
      case ReflectedDescriptorType::StorageBuffer: return "a storage buffer";
      default: return "an unrecognised type";
      }
    }

    struct ReflectedModule {
      VkExtent3D localSize = { 0, 0, 0 };
      std::vector<uint32_t> bindings;
      // Keyed by binding number rather than parallel to 'bindings'; a binding
      // declared with a type this file does not model is simply absent.
      std::unordered_map<uint32_t, ReflectedDescriptorType> bindingTypes;
    };

    // Reads back the three things the manifest deliberately does not state:
    // the workgroup size, which descriptors the module actually ended up
    // declaring after slangc dropped the ones it did not use, and what type
    // each of those is.
    //
    // The type half is what turns "declared a file texture as Texture2D" from
    // a wrong picture into a load error. SPIR-V puts decorations ahead of the
    // types they decorate, so nothing can be resolved during the walk: the
    // instructions are indexed on the way past and the variables are followed
    // through to their base types afterwards.
    bool reflectComputeModule(
      SpirvCodeBuffer& code, ReflectedModule& result, std::string& error) {
      uint32_t entryPointCount = 0;
      std::unordered_map<uint32_t, uint32_t> descriptorSets;
      std::unordered_map<uint32_t, uint32_t> bindingOfVariable;
      std::unordered_map<uint32_t, uint32_t> variablePointerType;
      std::unordered_map<uint32_t, uint32_t> pointerTarget;
      std::unordered_map<uint32_t, uint32_t> pointerStorage;
      std::unordered_map<uint32_t, uint32_t> arrayElement;
      // OpTypeImage's 'Sampled' operand: 1 is read through a sampler, 2 is a
      // storage image, and those are the two the binding space hands out.
      std::unordered_map<uint32_t, uint32_t> imageSampled;
      std::unordered_set<uint32_t> sampledImageTypes;
      std::unordered_set<uint32_t> samplerTypes;
      std::unordered_set<uint32_t> structTypes;

      for (auto instruction : code) {
        switch (instruction.opCode()) {
        case spv::OpEntryPoint:
          // slangc renames the selected entry to 'main' in the module it
          // emits, so the name here says nothing about which '-entry' produced
          // it. One invocation per entry point is what makes that safe: a
          // module with two would be ambiguous and is rejected below.
          entryPointCount++;
          break;

        case spv::OpExecutionMode:
          if (instruction.arg(2) == spv::ExecutionModeLocalSize) {
            result.localSize = VkExtent3D {
              instruction.arg(3), instruction.arg(4), instruction.arg(5) };
          }
          break;

        case spv::OpDecorate:
          if (instruction.arg(2) == spv::DecorationBinding) {
            result.bindings.push_back(instruction.arg(3));
            bindingOfVariable[instruction.arg(1)] = instruction.arg(3);
          } else if (instruction.arg(2) == spv::DecorationDescriptorSet) {
            descriptorSets[instruction.arg(1)] = instruction.arg(3);
          }
          break;

        case spv::OpVariable:
          variablePointerType[instruction.arg(2)] = instruction.arg(1);
          break;

        case spv::OpTypePointer:
          pointerStorage[instruction.arg(1)] = instruction.arg(2);
          pointerTarget[instruction.arg(1)] = instruction.arg(3);
          break;

        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
          arrayElement[instruction.arg(1)] = instruction.arg(2);
          break;

        case spv::OpTypeImage:
          imageSampled[instruction.arg(1)] = instruction.arg(7);
          break;

        case spv::OpTypeSampledImage:
          sampledImageTypes.insert(instruction.arg(1));
          break;

        case spv::OpTypeSampler:
          samplerTypes.insert(instruction.arg(1));
          break;

        case spv::OpTypeStruct:
          structTypes.insert(instruction.arg(1));
          break;

        default:
          break;
        }
      }

      for (const auto& variableBinding : bindingOfVariable) {
        const auto pointer = variablePointerType.find(variableBinding.first);
        if (pointer == variablePointerType.end()) {
          continue;
        }
        const auto target = pointerTarget.find(pointer->second);
        const auto storage = pointerStorage.find(pointer->second);
        if (target == pointerTarget.end()) {
          continue;
        }

        // A descriptor array declares the array, not the element, so the walk
        // has to reach the element type before anything is recognisable. The
        // depth is bounded because a malformed type table must fail the load
        // rather than hang it.
        uint32_t type = target->second;
        for (uint32_t depth = 0; depth < 8; depth++) {
          const auto element = arrayElement.find(type);
          if (element == arrayElement.end()) {
            break;
          }
          type = element->second;
        }

        ReflectedDescriptorType reflected = ReflectedDescriptorType::Unknown;
        const auto sampled = imageSampled.find(type);
        if (sampledImageTypes.find(type) != sampledImageTypes.end()) {
          reflected = ReflectedDescriptorType::CombinedImageSampler;
        } else if (sampled != imageSampled.end()) {
          reflected = sampled->second == 2
            ? ReflectedDescriptorType::StorageImage
            : ReflectedDescriptorType::SampledImage;
        } else if (samplerTypes.find(type) != samplerTypes.end()) {
          reflected = ReflectedDescriptorType::Sampler;
        } else if (structTypes.find(type) != structTypes.end()
                && storage != pointerStorage.end()) {
          reflected = storage->second == spv::StorageClassUniform
            ? ReflectedDescriptorType::UniformBuffer
            : ReflectedDescriptorType::StorageBuffer;
        }

        if (reflected != ReflectedDescriptorType::Unknown) {
          result.bindingTypes[variableBinding.second] = reflected;
        }
      }

      if (entryPointCount != 1) {
        error = "expected exactly one entry point in the compiled module, found "
          + std::to_string(entryPointCount);
        return false;
      }
      if (result.localSize.width == 0 || result.localSize.height == 0 || result.localSize.depth == 0) {
        error = "the entry point declares no [numthreads] workgroup size";
        return false;
      }

      for (const auto& descriptorSet : descriptorSets) {
        // Everything Remix binds lives in set 0; a shader that asks for
        // another set would compile and then never receive a descriptor.
        if (descriptorSet.second != 0) {
          error = "descriptor set " + std::to_string(descriptorSet.second)
            + " is not bound by Remix; declare every resource in set 0";
          return false;
        }
      }

      std::sort(result.bindings.begin(), result.bindings.end());
      result.bindings.erase(
        std::unique(result.bindings.begin(), result.bindings.end()), result.bindings.end());
      return true;
    }

    // Scene bindings every pass gets whether it uses them or not. Binding 1,
    // the output image, is deliberately absent: it is handed out per pass from
    // the manifest's write list, because a pass that stores to the output
    // without saying so is exactly the mistake this file is trying to catch.
    // Binding 0 stays unconditional because it is the colour a pass reads, and
    // 'read output' redirects it rather than adding a slot.
    const DxvkResourceSlot kSceneResourceSlots[] = {
      { kInputBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D },
      { kLinearDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kMotionBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kWorldNormalBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kAlbedoBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kRoughnessBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kSurfaceFlagsBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kObjectPickingBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kConeRadiusBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kBlueNoiseBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D_ARRAY },
      { kExposureBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_1D, VK_ACCESS_SHADER_READ_BIT },
      { kSceneSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER },
      { kFrameDataBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
      { kSceneLinearSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER },
      { kFocusStateBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_1D, VK_ACCESS_SHADER_READ_BIT },
      { kProjectedDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kParameterDataBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
    };

    bool listed(const std::vector<uint32_t>& list, uint32_t value) {
      return std::find(list.begin(), list.end(), value) != list.end();
    }

    // Builds the slot array for one pass out of the manifest, and checks it
    // against what the module actually declared.
    //
    // Both halves matter. DxvkPipelineLayout numbers its bindings 0..n-1 and
    // DxvkShader::createShaderModule rewrites the module's binding decorations
    // through getBindingId(slot), which answers InvalidBinding for a slot the
    // shader was not built with. A binding the manifest does not mention
    // therefore does not fail to bind loudly: it produces a module that loads,
    // dispatches, and stores nowhere. Deriving the array from the manifest is
    // what keeps the layout small; comparing it against the reflection is what
    // turns the remaining disagreement into a load error rather than a picture
    // that is quietly missing a pass.
    bool buildPassResourceSlots(
      const RtxExternalEffectManifest& manifest,
      const RtxExternalEffectPass& pass,
      const ReflectedModule& reflected,
      std::vector<DxvkResourceSlot>& slots,
      std::string& error) {
      slots.assign(std::begin(kSceneResourceSlots), std::end(kSceneResourceSlots));

      const bool writesOutput = listed(pass.writes, kRtxExternalEffectOutputTexture);
      if (writesOutput) {
        slots.push_back({ kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT });
      }

      for (const uint32_t index : pass.reads) {
        if (index == kRtxExternalEffectOutputTexture) {
          continue;
        }
        // A file texture's address mode is declared per texture rather than
        // chosen at the call site, so the runtime has to supply the sampler
        // along with the image: its slot is a combined image sampler, the way
        // binding 0 already is, instead of the separate sampled image a
        // pass-written texture gets.
        const bool fromFile = index < manifest.textures.size()
                           && !manifest.textures[index].file.empty();
        // The access flag is load bearing, not documentation:
        // DxvkContext::commitPostBarriers records exactly what the slot
        // declares, so a read slot that declares nothing leaves the next
        // pass's write-after-read completely untracked. A file texture is
        // never written after its upload, but a read slot that lies about
        // being a read is not a discipline worth having exceptions to.
        slots.push_back({ kUserTextureReadBinding + index,
                          fromFile ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                   : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_READ_BIT });
      }

      for (const uint32_t index : pass.writes) {
        if (index == kRtxExternalEffectOutputTexture) {
          continue;
        }
        slots.push_back({ kUserTextureWriteBinding + index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                          VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT });
      }

      const auto textureName = [&](uint32_t index) {
        return index < manifest.textures.size()
          ? manifest.textures[index].id
          : std::to_string(index);
      };

      for (const uint32_t binding : reflected.bindings) {
        if (binding == kOutputBinding) {
          if (!writesOutput) {
            error = "the shader stores to OutputColor but the pass does not declare 'write output'";
            return false;
          }
          continue;
        }
        if (binding < kUserTextureReadBinding) {
          // A scene binding. They are all present unconditionally, and 17 is
          // the only hole; a shader claiming it gets no descriptor at all.
          if (binding == kReservedBinding) {
            error = "binding " + std::to_string(kReservedBinding) + " is reserved";
            return false;
          }
          continue;
        }
        if (binding < kUserTextureWriteBinding) {
          const uint32_t index = binding - kUserTextureReadBinding;
          if (!listed(pass.reads, index)) {
            error = "the shader reads user texture binding " + std::to_string(binding)
              + " (" + textureName(index) + ") but the pass does not declare 'read "
              + textureName(index) + "'";
            return false;
          }
          continue;
        }
        if (binding < kUserTextureWriteBinding + kMaxRtxExternalEffectTextures) {
          const uint32_t index = binding - kUserTextureWriteBinding;
          if (!listed(pass.writes, index)) {
            error = "the shader writes user texture binding " + std::to_string(binding)
              + " (" + textureName(index) + ") but the pass does not declare 'write "
              + textureName(index) + "'";
            return false;
          }
          continue;
        }
        error = "binding " + std::to_string(binding) + " is outside the RemixFX binding space";
        return false;
      }

      // The other direction. A declared read the shader never performs is only
      // a wasted descriptor, but a declared write it never performs means the
      // texture stays at whatever it held last frame while the manifest says
      // it was refreshed, and that reads as a stale image rather than a bug.
      const auto declaresBinding = [&](uint32_t binding) {
        return listed(reflected.bindings, binding);
      };
      if (writesOutput && !declaresBinding(kOutputBinding)) {
        error = "the pass declares 'write output' but the shader never stores to OutputColor";
        return false;
      }
      for (const uint32_t index : pass.reads) {
        if (index != kRtxExternalEffectOutputTexture
         && !declaresBinding(kUserTextureReadBinding + index)) {
          error = "the pass declares 'read " + textureName(index)
            + "' but the shader declares no REMIXFX_READ(REMIXFX_TEX_" + textureName(index) + ")";
          return false;
        }
      }
      for (const uint32_t index : pass.writes) {
        if (index != kRtxExternalEffectOutputTexture
         && !declaresBinding(kUserTextureWriteBinding + index)) {
          error = "the pass declares 'write " + textureName(index)
            + "' but the shader declares no REMIXFX_WRITE(REMIXFX_TEX_" + textureName(index) + ")";
          return false;
        }
      }

      // Third direction, and the one that used to be missing: the binding
      // exists on both sides and names a descriptor of the wrong kind. Vulkan
      // does not reject it - the module is well formed and the descriptor is
      // written - so the failure mode was a sample returning nothing, or a
      // store landing nowhere, with no error anywhere to explain it. The
      // common case is declaring a file texture as Texture2D: file textures
      // carry their own address mode and so bind as combined image samplers.
      for (const DxvkResourceSlot& slot : slots) {
        const auto declared = reflected.bindingTypes.find(slot.slot);
        if (declared == reflected.bindingTypes.end()) {
          // Either the shader does not use the slot, or it is a type this
          // file does not model. Neither is grounds for refusing the effect.
          continue;
        }
        const ReflectedDescriptorType expected = toReflectedType(slot.type);
        if (expected == ReflectedDescriptorType::Unknown || expected == declared->second) {
          continue;
        }

        if (slot.slot >= kUserTextureReadBinding
         && slot.slot < kUserTextureWriteBinding + kMaxRtxExternalEffectTextures) {
          const bool isWrite = slot.slot >= kUserTextureWriteBinding;
          const uint32_t index = slot.slot - (isWrite ? kUserTextureWriteBinding : kUserTextureReadBinding);
          const char* macro = isWrite ? "REMIXFX_WRITE" : "REMIXFX_READ";
          const char* because = isWrite
            ? "a pass target is a storage image"
            : (index < manifest.textures.size() && !manifest.textures[index].file.empty()
                 ? "a file texture carries its own address mode and binds as a combined image sampler"
                 : "a pass-written texture is sampled through the shared scene sampler");
          error = std::string(macro) + "(REMIXFX_TEX_" + textureName(index) + ") declares "
            + shaderTypeName(declared->second) + " but " + because + ", so it must be "
            + shaderTypeName(expected);
          return false;
        }

        error = "binding " + std::to_string(slot.slot) + " is bound as "
          + descriptorTypeName(expected) + " but the shader declares "
          + descriptorTypeName(declared->second);
        return false;
      }

      return true;
    }

    std::unordered_map<std::string, std::string> parseAssignments(
      const std::string& serialized) {
      std::unordered_map<std::string, std::string> result;
      std::stringstream stream(serialized);
      std::string entry;

      while (std::getline(stream, entry, ';')) {
        const size_t equals = entry.find('=');
        if (equals != std::string::npos && equals != 0) {
          result[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
      }

      return result;
    }

    bool parsePersistedValues(
      const std::string& serialized,
      uint32_t count,
      std::vector<float>& values) {
      std::stringstream stream(serialized);
      std::vector<float> parsed;
      float value = 0.0f;
      while (stream >> value) {
        parsed.push_back(value);
      }

      if (parsed.size() != count) {
        return false;
      }

      values = std::move(parsed);
      return true;
    }

    std::wstring quoteCommandArgument(const std::wstring& value) {
      std::wstring result = L"\"";
      result += value;
      result += L"\"";
      return result;
    }

    std::wstring quoteCommandArgument(const std::filesystem::path& value) {
      return quoteCommandArgument(value.wstring());
    }

    // Texture ids and entry point names are C identifiers, so widening is a
    // per-character copy rather than a codepage conversion.
    std::wstring widenIdentifier(const std::string& value) {
      return std::wstring(value.begin(), value.end());
    }

    // Reads whatever slangc has written so far without blocking. The child has
    // to be drained while it is still running: once the pipe buffer fills the
    // compiler stalls on its next write, and the timeout below would then fire
    // on a compile that was only ever waiting for us.
    void drainPipe(HANDLE pipe, std::string& output) {
      for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
          return;
        }

        char buffer[1024];
        DWORD read = 0;
        const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(pipe, buffer, request, &read, nullptr) || read == 0) {
          return;
        }

        // Keep draining past the cap even though nothing more is stored: the
        // point of the loop is to keep the compiler unblocked, not to collect.
        if (output.size() < kMaxCompilerOutputCharacters) {
          output.append(buffer, std::min<size_t>(read, kMaxCompilerOutputCharacters - output.size()));
        }
      }
    }

    // slangc writes CRLF, which ImGui renders as a stray glyph rather than a
    // line break, and a trailing newline pushes an empty row into the panel.
    std::string normalizeCompilerOutput(std::string output) {
      output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
      const size_t last = output.find_last_not_of(" \t\n");
      return last == std::string::npos ? std::string() : output.substr(0, last + 1);
    }

    // Binary units, which is what a VRAM budget is measured in and what the
    // rest of this UI reports. One decimal from KiB up: a texture set that
    // moves between 24.8 and 25.1 MiB as the render target changes is saying
    // something, and rounding that to 25 throws it away.
    std::string formatByteCount(uint64_t bytes) {
      static const char* const kUnits[] = { "B", "KiB", "MiB", "GiB" };
      double value = static_cast<double>(bytes);
      size_t unit = 0;
      while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        unit++;
      }

      char buffer[64];
      std::snprintf(
        buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
      return buffer;
    }
  }

  RtxExternalEffects& RtxExternalEffects::instance() {
    static RtxExternalEffects instance;
    return instance;
  }

  RtxExternalEffects::Effect* RtxExternalEffects::findEffect(const std::string& id) {
    const auto it = std::find_if(m_effects.begin(), m_effects.end(), [&](const Effect& effect) {
      return effect.manifest.id == id;
    });
    return it == m_effects.end() ? nullptr : &*it;
  }

  const RtxExternalEffects::Effect* RtxExternalEffects::findEffect(const std::string& id) const {
    const auto it = std::find_if(m_effects.begin(), m_effects.end(), [&](const Effect& effect) {
      return effect.manifest.id == id;
    });
    return it == m_effects.end() ? nullptr : &*it;
  }

  std::filesystem::path RtxExternalEffects::resolveSearchPath() const {
    std::filesystem::path path(effectSearchPath());
    if (path.is_relative()) {
      path = std::filesystem::path(env::getExePath()).parent_path() / path;
    }
    return path.lexically_normal();
  }

  std::filesystem::path RtxExternalEffects::findCompiler() const {
    std::vector<std::filesystem::path> candidates;
    if (!slangCompilerPath().empty()) {
      std::filesystem::path configured(slangCompilerPath());
      if (configured.is_relative()) {
        configured = std::filesystem::path(env::getExePath()).parent_path() / configured;
      }
      candidates.push_back(configured);
    } else {
      const std::filesystem::path dllDirectory(env::getDllDirectory());
      const std::filesystem::path exeDirectory = std::filesystem::path(env::getExePath()).parent_path();
      candidates.push_back(dllDirectory / "slangc.exe");
      candidates.push_back(dllDirectory / "slang" / "slangc.exe");
      candidates.push_back(exeDirectory / "slangc.exe");
      candidates.push_back(exeDirectory / "slang" / "slangc.exe");
      candidates.push_back(std::filesystem::current_path() / "external" / "slang" / "slangc.exe");

      std::vector<wchar_t> searchResult(32768);
      const DWORD length = SearchPathW(
        nullptr, L"slangc.exe", nullptr,
        static_cast<DWORD>(searchResult.size()), searchResult.data(), nullptr);
      if (length > 0 && length < searchResult.size()) {
        candidates.emplace_back(searchResult.data());
      }
    }

    std::error_code error;
    for (const std::filesystem::path& candidate : candidates) {
      if (std::filesystem::is_regular_file(candidate, error)) {
        return std::filesystem::absolute(candidate).lexically_normal();
      }
      error.clear();
    }

    return {};
  }

  // Runs on the reload worker, never on the render thread. Static rather than
  // a member so that is checkable by inspection: it reads nothing the class
  // owns and writes nothing but its out-parameters and the two files it is
  // pointed at.
  static bool compileEffect(
    const std::filesystem::path& compiler,
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::string& entryPoint,
    const std::vector<std::wstring>& defines,
    const std::atomic<bool>& cancelled,
    std::string& error,
    std::string& compilerOutput) {
    std::filesystem::path temporaryOutput = output;
    temporaryOutput += ".tmp";
    const std::filesystem::path dependencyOutput = dependencyPathForPass(output);
    std::filesystem::path temporaryDependencyOutput = dependencyOutput;
    temporaryDependencyOutput += ".tmp";

    // One invocation per entry point is forced rather than chosen: slangc
    // rejects two '-o' for a single target, so there is no way to ask it for
    // several modules at once.
    std::wstring commandLine = quoteCommandArgument(compiler)
      + L" -entry " + quoteCommandArgument(widenIdentifier(entryPoint))
      + L" -target spirv -zero-initialize -emit-spirv-directly"
      + L" -matrix-layout-column-major -fvk-use-scalar-layout -D__SLANG__";
    for (const std::wstring& define : defines) {
      commandLine += L" " + quoteCommandArgument(define);
    }
    commandLine += L" -I " + quoteCommandArgument(source.parent_path())
      + L" -depfile " + quoteCommandArgument(temporaryDependencyOutput)
      + L" -o " + quoteCommandArgument(temporaryOutput)
      + L" " + quoteCommandArgument(source);

    // stdout and stderr share one pipe: slangc interleaves them and an author
    // reading a diagnostic wants it in the order the compiler emitted it.
    SECURITY_ATTRIBUTES pipeSecurity = {};
    pipeSecurity.nLength = sizeof(pipeSecurity);
    pipeSecurity.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &pipeSecurity, 0)) {
      error = "could not capture slangc.exe output (Windows error "
        + std::to_string(GetLastError()) + ")";
      return false;
    }
    // Only the write end may be inherited, or the read end never reaches EOF
    // because this process still holds a writer open.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // Inheriting handles is all-or-nothing unless an explicit list is given,
    // and this DLL lives inside an arbitrary game process: handing slangc every
    // inheritable file and socket the game happens to hold open is not on.
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<uint8_t> attributeStorage(attributeSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    const bool attributesReady = attributeSize != 0
      && InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize)
      && UpdateProcThreadAttribute(
           attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
           &writePipe, sizeof(writePipe), nullptr, nullptr);

    STARTUPINFOEXW startupInfo = {};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdOutput = writePipe;
    startupInfo.StartupInfo.hStdError = writePipe;
    startupInfo.lpAttributeList = attributesReady ? attributeList : nullptr;
    PROCESS_INFORMATION processInfo = {};

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    const std::wstring workingDirectory = source.parent_path().wstring();

    const BOOL created = CreateProcessW(
      compiler.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, workingDirectory.c_str(),
      &startupInfo.StartupInfo, &processInfo);

    if (attributesReady) {
      DeleteProcThreadAttributeList(attributeList);
    }
    // The parent's copy of the write end has to go before the drain loop, or
    // the pipe never signals EOF once slangc exits.
    CloseHandle(writePipe);

    if (!created) {
      CloseHandle(readPipe);
      error = "could not start slangc.exe (Windows error " + std::to_string(GetLastError()) + ")";
      return false;
    }

    DWORD waitResult = WAIT_TIMEOUT;
    bool abandoned = false;
    for (uint32_t elapsed = 0; elapsed < kCompilerTimeoutMilliseconds; elapsed += 20) {
      drainPipe(readPipe, compilerOutput);
      // Polled between waits rather than only at the top: the device is being
      // destroyed and the render thread is inside onDestroyDevice waiting for
      // this thread to finish. Thirty seconds of slangc is not an acceptable
      // answer to that, so the child is killed and the job abandoned.
      if (cancelled.load(std::memory_order_acquire)) {
        abandoned = true;
        break;
      }
      waitResult = WaitForSingleObject(processInfo.hProcess, 20);
      if (waitResult != WAIT_TIMEOUT) {
        break;
      }
    }
    // Anything buffered between the last drain and exit is usually the most
    // interesting part, since a failing compiler prints and then quits.
    drainPipe(readPipe, compilerOutput);
    compilerOutput = normalizeCompilerOutput(std::move(compilerOutput));

    if (abandoned) {
      TerminateProcess(processInfo.hProcess, 1);
      WaitForSingleObject(processInfo.hProcess, INFINITE);
      error = "the compile was cancelled";
    } else if (waitResult == WAIT_TIMEOUT) {
      TerminateProcess(processInfo.hProcess, 1);
      WaitForSingleObject(processInfo.hProcess, INFINITE);
      error = "slangc.exe exceeded the 30 second compile timeout";
    } else if (waitResult != WAIT_OBJECT_0) {
      error = "waiting for slangc.exe failed (Windows error " + std::to_string(GetLastError()) + ")";
    } else {
      DWORD exitCode = 1;
      if (!GetExitCodeProcess(processInfo.hProcess, &exitCode) || exitCode != 0) {
        error = "slangc.exe exited with code " + std::to_string(exitCode);
      }
    }

    CloseHandle(readPipe);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (error.empty() && !MoveFileExW(
          temporaryOutput.c_str(), output.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      error = "could not replace cached SPIR-V (Windows error " + std::to_string(GetLastError()) + ")";
    }

    if (error.empty()) {
      // Published after the module, and only with it: a dependency list that
      // is newer than the SPIR-V it describes would make every later mtime
      // comparison answer 'up to date' for a module that was never written.
      // A failed move leaves no list, which reads as 'unknown' and falls the
      // staleness check back to the source file alone.
      MoveFileExW(
        temporaryDependencyOutput.c_str(), dependencyOutput.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    if (!error.empty()) {
      std::error_code removeError;
      std::filesystem::remove(temporaryOutput, removeError);
      removeError.clear();
      std::filesystem::remove(temporaryDependencyOutput, removeError);
    }
    return error.empty();
  }

  bool RtxExternalEffects::reflectPasses(
    const RtxExternalEffectManifest& manifest,
    const std::filesystem::path& sourcePath,
    std::vector<PendingPass>& passes,
    std::string& error) {
    std::vector<PendingPass> reflectedPasses;
    reflectedPasses.reserve(manifest.passes.size());

    for (const RtxExternalEffectPass& pass : manifest.passes) {
      const std::filesystem::path path = spirvPathForPass(sourcePath, pass.entryPoint);
      const std::string where = "pass '" + pass.id + "': ";

      std::error_code fileError;
      const uintmax_t fileSize = std::filesystem::file_size(path, fileError);
      if (fileError || fileSize < 5 * sizeof(uint32_t) || fileSize % sizeof(uint32_t) != 0) {
        error = where + "SPIR-V file is missing or has an invalid size";
        return false;
      }

      std::ifstream file(path, std::ios::binary);
      if (!file) {
        error = where + "could not open SPIR-V file";
        return false;
      }

      SpirvCodeBuffer code(file);
      if (code.dwords() < 5 || code.data()[0] != spv::MagicNumber) {
        error = where + "compiled file does not contain a SPIR-V module";
        return false;
      }

      ReflectedModule reflected;
      std::string moduleError;
      if (!reflectComputeModule(code, reflected, moduleError)) {
        error = where + moduleError;
        return false;
      }

      PendingPass result;
      if (!buildPassResourceSlots(manifest, pass, reflected, result.slots, moduleError)) {
        error = where + moduleError;
        return false;
      }

      result.localSize = reflected.localSize;
      result.code.assign(code.data(), code.data() + code.dwords());
      result.debugName = "remixfx_" + manifest.id + "_" + pass.id;
      // The slots go into the hash as well as the module: two of the three
      // things createShader is handed come from the manifest, and a pass whose
      // read list changed needs a new shader even if the SPIR-V did not move.
      result.codeHash = XXH3_64bits(code.data(), code.size());
      result.codeHash = XXH3_64bits_withSeed(
        result.slots.data(), result.slots.size() * sizeof(DxvkResourceSlot), result.codeHash);
      reflectedPasses.push_back(std::move(result));
    }

    passes = std::move(reflectedPasses);
    return true;
  }

  bool RtxExternalEffects::createPasses(
    DxvkDevice* device,
    Effect& effect,
    const Effect* previous,
    std::vector<PendingPass>& passes,
    std::string& error) {
    std::vector<CompiledPass> compiled;
    compiled.reserve(passes.size());

    for (size_t i = 0; i < passes.size(); i++) {
      PendingPass& pending = passes[i];

      // Matched by position rather than by name: a pass list that was
      // reordered or renumbered has already changed every later pass's
      // identity, and the hash is what actually decides.
      if (previous != nullptr && i < previous->passes.size()
       && previous->passes[i].codeHash == pending.codeHash
       && previous->passes[i].shader != nullptr) {
        compiled.push_back(previous->passes[i]);
        continue;
      }

      CompiledPass result;
      result.localSize = pending.localSize;
      result.codeHash = pending.codeHash;
      result.shader = device->createShader(
        VK_SHADER_STAGE_COMPUTE_BIT,
        static_cast<uint32_t>(pending.slots.size()), pending.slots.data(),
        { 0, 0, 0, sizeof(ExternalEffectArgs) },
        SpirvCodeBuffer(static_cast<uint32_t>(pending.code.size()), pending.code.data()));
      result.shader->setDebugName(pending.debugName.c_str());
      result.shader->generateShaderKey();
      device->registerShader(result.shader, true);
      compiled.push_back(std::move(result));
    }

    effect.passes = std::move(compiled);
    return true;
  }

  bool RtxExternalEffects::decodeFileTexture(
    const RtxExternalEffectManifest& manifest,
    const std::filesystem::path& sourcePath,
    size_t index,
    RtxExternalEffectImage& image,
    std::string& error) {
    const RtxExternalEffectTexture& declaration = manifest.textures[index];

    // Relative to the effect file rather than to the search root, so an effect
    // and the images it ships with move as one directory. u8path because the
    // manifest value is bytes out of the source file, and the rest of this
    // runtime treats those as UTF-8.
    const std::filesystem::path path =
      (sourcePath.parent_path() / std::filesystem::u8path(declaration.file))
        .lexically_normal();

    std::string failure;
    if (!loadRtxExternalEffectImage(path, declaration.srgb, image, failure)) {
      // Naming the resolved path rather than the declared one: half of these
      // failures are a path that resolved somewhere the author did not mean,
      // and the declared spelling looks correct either way.
      error = "texture '" + declaration.id + "' (" + path.u8string() + "): " + failure;
      return false;
    }
    return true;
  }

  bool RtxExternalEffects::decodeFileTextures(
    const RtxExternalEffectManifest& manifest,
    const std::filesystem::path& sourcePath,
    bool keepBytes,
    std::vector<RtxExternalEffectImage>& images,
    std::string& error) {
    images.assign(manifest.textures.size(), RtxExternalEffectImage());

    for (size_t i = 0; i < manifest.textures.size(); i++) {
      if (manifest.textures[i].file.empty()) {
        continue;
      }
      if (!decodeFileTexture(manifest, sourcePath, i, images[i], error)) {
        images.clear();
        return false;
      }

      // Every effect on disk is decoded, switched on or not, because a missing
      // image belongs next to the compile errors rather than appearing the
      // first time somebody ticks the box. Only an effect that is about to
      // upload them has any use for the bytes though, and most of a sample
      // directory is switched off: holding a grain plate per effect for the
      // rest of the session to save a decode nobody asked for is the wrong
      // trade. The ones dropped here are decoded again on demand.
      if (!keepBytes) {
        images[i] = {};
      }
    }

    return true;
  }

  bool RtxExternalEffects::ensureFileTexture(
    Rc<DxvkContext> ctx, Effect& effect, size_t index, std::string& error) {
    if (effect.textures[index].resource.isValid()) {
      // Uploaded on the first frame the effect ran and never touched again:
      // nothing about a file texture follows the render target, so no resize
      // reaches it and no release below Everything drops it.
      return true;
    }

    const RtxExternalEffectTexture& declaration = effect.manifest.textures[index];
    if (effect.fileImages.size() != effect.manifest.textures.size()) {
      effect.fileImages.assign(effect.manifest.textures.size(), RtxExternalEffectImage());
    }
    // Empty either because the effect was switched off at load and the bytes
    // were dropped, or because this texture has already been uploaded once and
    // the GPU side was then released. Both want the same thing, and decoding
    // here rather than treating it as impossible is what keeps a released
    // resource from turning into an effect that reports ready forever and
    // renders nothing.
    if (effect.fileImages[index].format == VK_FORMAT_UNDEFINED
     && !decodeFileTexture(
          effect.manifest, effect.sourcePath, index, effect.fileImages[index], error)) {
      return false;
    }

    const RtxExternalEffectImage& image = effect.fileImages[index];
    const std::string name = "remixfx " + effect.manifest.id + " " + declaration.id;

    DxvkImageCreateInfo desc;
    desc.type = VK_IMAGE_TYPE_2D;
    desc.format = image.format;
    desc.flags = 0;
    desc.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    desc.extent = image.extent;
    desc.numLayers = 1;
    desc.mipLevels = 1;
    // Sampled, never storage. That is not only what a read-only input needs:
    // the sRGB and block compressed formats a file may legitimately carry
    // cannot be storage images at all, so asking would fail allocation on
    // exactly the files this feature exists to load.
    desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    desc.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    desc.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    desc.tiling = VK_IMAGE_TILING_OPTIMAL;
    // GENERAL, which is what createImageResource leaves every other texture in
    // this binding space in. One layout across the read slots is one fewer
    // thing for a transition to disagree about.
    desc.layout = VK_IMAGE_LAYOUT_GENERAL;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = desc.format;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer = 0;
    viewInfo.numLayers = 1;

    // Both creates throw rather than returning null, and unlike every other
    // texture here the format is not one of the manifest's validated few: it
    // is whatever the file's header said. A 24 bit DDS, or a BC7 one on
    // hardware without it, is a perfectly ordinary authoring mistake and has
    // to come back as the effect's status row rather than as a DxvkError
    // unwinding out of the middle of a frame.
    Resources::Resource resource;
    try {
      resource.image = ctx->getDevice()->createImage(
        desc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXMaterialTexture, name.c_str());

      // updateImage stages the copy and does the two layout transitions around
      // it, so this is safe to record mid-frame on the render thread's context.
      ctx->updateImage(
        resource.image,
        VkImageSubresourceLayers { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, desc.numLayers },
        VkOffset3D { 0, 0, 0 }, desc.extent,
        image.data.data(), image.rowPitch, image.layerPitch);
      // The first pass that samples this is a compute dispatch a few commands
      // later in the same list, and nothing between the two orders the transfer
      // against it.
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

      resource.view = ctx->getDevice()->createImageView(resource.image, viewInfo);
    } catch (const DxvkError& failure) {
      error = "file texture '" + declaration.id + "' could not be created as "
        + std::to_string(desc.extent.width) + "x" + std::to_string(desc.extent.height)
        + " format " + std::to_string(static_cast<uint32_t>(desc.format))
        + ": " + failure.message();
      return false;
    }

    effect.textures[index].resource = std::move(resource);
    effect.textures[index].extent = desc.extent;
    // The decoded copy only had to get as far as here, and the image it was
    // copied into outlives it. Dropping it keeps a set of grain plates from
    // sitting in RAM for the rest of the session; anything that needs them
    // again decodes from disk at the top of this function.
    effect.fileImages[index] = {};
    return true;
  }

  void RtxExternalEffects::releaseTextures(Effect& effect, TextureReleaseScope scope) {
    if (effect.textures.empty()) {
      // The disabled path calls this every frame, so the common case of
      // nothing to free has to cost nothing and, in particular, must not keep
      // re-arming historyInvalid for an effect that owns no history.
      return;
    }

    bool released = false;
    // The two are kept in lockstep everywhere that grows either of them, but
    // this runs on the teardown path as well, where being wrong is an
    // out-of-bounds read rather than a missed release.
    const size_t count = std::min(effect.textures.size(), effect.manifest.textures.size());
    for (size_t i = 0; i < count; i++) {
      const RtxExternalEffectTexture& declaration = effect.manifest.textures[i];
      if (scope != TextureReleaseScope::Everything) {
        // A file texture is not sized against the render target and its
        // contents cannot be regenerated on the GPU, so the only release that
        // should touch it is the one that has no choice.
        if (!declaration.file.empty()) {
          continue;
        }
        if (scope == TextureReleaseScope::Scratch && declaration.persist) {
          continue;
        }
      }
      if (!effect.textures[i].resource.isValid()) {
        continue;
      }
      // Dropping the last reference the effect holds is all this needs to do.
      // Anything still in flight is kept alive by the command list that used
      // it, and is destroyed when that frame retires.
      effect.textures[i].resource.reset();
      effect.textures[i].extent = {};
      released = true;
    }

    if (released) {
      // Whatever survives is about to sit beside freshly cleared neighbours,
      // so the next frame is a history boundary either way.
      effect.historyInvalid = true;
    }
    if (scope == TextureReleaseScope::Everything) {
      effect.textures.clear();
      effect.textureExtent = {};
    } else if (scope == TextureReleaseScope::Sized) {
      // The vector itself stays, because the file textures still in it are
      // indexed in lockstep with the manifest.
      effect.textureExtent = {};
    }
  }

  bool RtxExternalEffects::ensureTextures(
    Rc<RtxContext> ctx,
    Effect& effect,
    const VkExtent3D& outputExtent,
    std::string& error) {
    if (effect.manifest.textures.empty()) {
      return true;
    }

    // A 'size' texture does not move when the render target does, but a 'div'
    // one does, and an effect mixing the two has to be checked as a whole
    // rather than per texture: a half resolution buffer that resized while the
    // tile grid it feeds did not is worse than reallocating both.
    const bool extentChanged = effect.textureExtent.width != outputExtent.width
                            || effect.textureExtent.height != outputExtent.height;
    if (extentChanged) {
      releaseTextures(effect, TextureReleaseScope::Sized);
    }
    if (effect.textures.size() != effect.manifest.textures.size()) {
      effect.textures.assign(effect.manifest.textures.size(), EffectTexture());
    }

    Rc<DxvkContext> baseCtx = ctx;
    for (size_t i = 0; i < effect.manifest.textures.size(); i++) {
      const RtxExternalEffectTexture& declaration = effect.manifest.textures[i];
      // A file texture is uploaded once and then sits there. It is checked
      // here rather than at load because this is the first point at which a
      // context exists to record the copy on.
      if (!declaration.file.empty()) {
        if (!ensureFileTexture(baseCtx, effect, i, error)) {
          releaseTextures(effect, TextureReleaseScope::Sized);
          return false;
        }
        continue;
      }

      const VkExtent3D extent = textureExtent(declaration, outputExtent);
      if (effect.textures[i].resource.isValid() && effect.textures[i].extent.width == extent.width
       && effect.textures[i].extent.height == extent.height) {
        continue;
      }

      const std::string name = "remixfx " + effect.manifest.id + " " + declaration.id;
      effect.textures[i].resource = Resources::createImageResource(
        baseCtx, name.c_str(), extent, toVkFormat(declaration.format), 1,
        VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 0, VK_IMAGE_USAGE_STORAGE_BIT);
      if (!effect.textures[i].resource.isValid()) {
        error = "could not allocate texture '" + declaration.id + "'";
        releaseTextures(effect, TextureReleaseScope::Sized);
        return false;
      }

      effect.textures[i].extent = extent;
      // createImageResource clears at allocation, so the contents are defined
      // but meaningless. Anything accumulating has to be told that.
      effect.historyInvalid = true;
    }

    effect.textureExtent = outputExtent;
    return true;
  }

  void RtxExternalEffects::joinReloadThread() {
    if (m_reloadThread.joinable()) {
      m_reloadThread.join();
    }
    m_reloadRunning = false;
    m_reloadDone.store(false, std::memory_order_release);
  }

  RtxExternalEffects::~RtxExternalEffects() {
    // A backstop for teardown paths that never reached onDestroyDevice. The
    // worker holds no device and no GPU resource, so all this has to do is
    // make sure it is not still running when its stack frame's statics start
    // being destroyed underneath it.
    m_reloadCancelled.store(true, std::memory_order_release);
    joinReloadThread();
  }

  void RtxExternalEffects::onDestroyDevice(DxvkDevice* device) {
    // Before anything is released. The worker never touches a device, but it
    // does own the PendingReload it is filling in, and that has to be finished
    // with before the singleton's state is reset out from under it. Cancelling
    // first bounds the wait to one 20 ms poll plus a TerminateProcess rather
    // than to slangc's 30 second timeout.
    m_reloadCancelled.store(true, std::memory_order_release);
    joinReloadThread();
    m_reloadCancelled.store(false, std::memory_order_release);
    m_reloadResult.reset();
    m_reloadPending = false;
    m_reloadPendingForce = false;

    // The callback fires on the FileWatch thread and only stores to an atomic,
    // so it is harmless at any point; removing it here keeps a torn-down
    // session from queueing work for a device that no longer exists.
    if (m_watchCallbackId != 0) {
      FileWatch::get().removeFileChangedCallback(m_watchCallbackId);
      m_watchCallbackId = 0;
    }
    {
      std::lock_guard<std::mutex> lock(m_watchRootMutex);
      m_watchRoot.clear();
    }
    m_watchTouchedAtMs.store(0, std::memory_order_release);

    // Only the device that owns them may be torn down out from under them, and
    // the singleton can outlive several: releasing another device's resources
    // here would drop live images belonging to a device still running.
    if (m_uniformBufferDevice == device) {
      m_frameDataBuffer = nullptr;
      m_parameterBuffer = nullptr;
      m_uniformBufferDevice = nullptr;
    }

    for (Effect& effect : m_effects) {
      releaseTextures(effect, TextureReleaseScope::Everything);
      effect.passes.clear();
    }
    // The shaders were registered with this device, so the whole discovery
    // result goes with it; the next ensureLoaded rebuilds it against whatever
    // device comes next.
    m_effects.clear();
    m_loaded = false;
  }

  void RtxExternalEffects::armFileWatch(const std::filesystem::path& searchPath) {
    {
      // Canonical, because FileWatch composes every notification path from the
      // canonical form of the directory it opened: a search path that reached
      // the same directory through a junction or a short name would otherwise
      // never match its own notifications. Lower-cased for the same reason one
      // step down - canonical preserves the on-disk case and the configured
      // path does not have to agree with it.
      std::error_code error;
      std::filesystem::path canonical = std::filesystem::canonical(searchPath, error);
      std::wstring root = (error ? searchPath : canonical).wstring();
      std::transform(root.begin(), root.end(), root.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(::towlower(c));
      });
      std::lock_guard<std::mutex> lock(m_watchRootMutex);
      m_watchRoot = std::move(root);
    }

    // Re-armed on every reload rather than once: AssetDataManager drops every
    // watched directory when it is destroyed, and installDir is a no-op for a
    // directory that is already watched. ReadDirectoryChangesW is issued with
    // bWatchSubtree, so one install covers the whole recursive search.
    FileWatch::get().installDir(searchPath.u8string().c_str());

    if (m_watchCallbackId != 0) {
      return;
    }
    // Capturing 'this' is safe here in a way it would not be for an ordinary
    // object: the singleton is a function-local static with process lifetime.
    // The callback still touches nothing but two atomics and one small mutex,
    // because it runs on the FileWatch thread and everything else this class
    // owns belongs to the render thread.
    m_watchCallbackId = FileWatch::get().addFileChangedCallback(
      [this](const std::filesystem::path& path) {
        if (!hotReload()) {
          return;
        }
        // The subscription is global: the mod asset directories are watched
        // through the same list, and a game streaming textures would otherwise
        // rebuild every shader it owns.
        std::wstring candidate = path.wstring();
        std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](wchar_t c) {
          return static_cast<wchar_t>(::towlower(c));
        });
        {
          std::lock_guard<std::mutex> lock(m_watchRootMutex);
          if (m_watchRoot.empty() || candidate.compare(0, m_watchRoot.size(), m_watchRoot) != 0) {
            return;
          }
        }
        // The cache this loader writes lands in the directory it watches, so
        // without this a successful compile notifies us of its own output and
        // the rebuild never stops.
        if (endsWith(path.u8string(), ".spv")
         || endsWith(path.u8string(), ".spv.d")
         || endsWith(path.u8string(), ".tmp")) {
          return;
        }
        // Stamped rather than counted: the render thread waits for the stamp
        // to stop moving, so a burst of writes collapses into one rebuild.
        m_watchTouchedAtMs.store(steadyMilliseconds(), std::memory_order_release);
      });
  }

  void RtxExternalEffects::pumpHotReload(DxvkDevice* device) {
    // Result first: a job that finished is the only reason m_effects changes
    // outside an explicit reload, and taking it before starting another one
    // keeps at most a single worker alive.
    if (m_reloadDone.load(std::memory_order_acquire)) {
      // The join is the handoff. Nothing reads m_reloadResult until the worker
      // that wrote it has been joined, so there is no lock and no window in
      // which a half-written result could be seen.
      joinReloadThread();
      std::unique_ptr<PendingReload> result = std::move(m_reloadResult);
      if (result != nullptr && !m_reloadCancelled.load(std::memory_order_acquire)) {
        applyReload(device, std::move(*result));
      }
    }

    if (m_reloadRunning) {
      return;
    }
    if (m_reloadPending) {
      // Both cleared before the job starts: a forced request that stayed set
      // would make every later coalesced reload a full rebuild.
      const bool force = m_reloadPendingForce;
      m_reloadPending = false;
      m_reloadPendingForce = false;
      startReload(force);
      return;
    }

    const int64_t touched = m_watchTouchedAtMs.load(std::memory_order_acquire);
    if (touched == 0 || !hotReload()) {
      return;
    }
    if (steadyMilliseconds() - touched < kHotReloadDebounceMilliseconds) {
      return;
    }
    // Cleared before the job starts, so a save that lands while it is running
    // re-stamps and is picked up on the next quiet period rather than lost.
    m_watchTouchedAtMs.store(0, std::memory_order_release);
    // Not forced: the per-pass dependency lists are what decide which modules
    // are actually stale, so saving one effect does not recompile the other
    // nine, and saving the shared binding header recompiles all of them.
    startReload(false);
  }

  void RtxExternalEffects::loadPersistedState(Effect& effect, const Effect* previous) const {
    for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
      std::copy(
        parameter.defaultValues.begin(), parameter.defaultValues.end(),
        effect.values.begin() + parameter.valueOffset);
    }

    effect.enabled = effect.manifest.enabledByDefault;
    if (previous != nullptr) {
      effect.enabled = previous->enabled;
      for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
        const auto oldParameter = std::find_if(
          previous->manifest.parameters.begin(), previous->manifest.parameters.end(),
          [&](const RtxExternalEffectParameter& candidate) {
            return candidate.id == parameter.id && candidate.valueCount == parameter.valueCount;
          });
        if (oldParameter != previous->manifest.parameters.end()) {
          std::copy_n(
            previous->values.begin() + oldParameter->valueOffset,
            parameter.valueCount,
            effect.values.begin() + parameter.valueOffset);
        }
      }
    }

    const auto enabledStates = parseAssignments(effectEnabledStates());
    const auto enabledState = enabledStates.find(effect.manifest.id);
    if (enabledState != enabledStates.end()) {
      effect.enabled = enabledState->second == "1";
    }

    const auto parameterValues = parseAssignments(effectParameterValues());
    for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
      const auto value = parameterValues.find(effect.manifest.id + "/" + parameter.id);
      std::vector<float> parsed;
      if (value != parameterValues.end()
       && parsePersistedValues(value->second, parameter.valueCount, parsed)) {
        for (uint32_t i = 0; i < parameter.valueCount; i++) {
          effect.values[parameter.valueOffset + i] = std::clamp(
            parsed[i], parameter.minValues[i], parameter.maxValues[i]);
        }
      }

      if (parameter.type == RtxExternalEffectParameterType::Bool) {
        effect.values[parameter.valueOffset] = effect.values[parameter.valueOffset] >= 0.5f ? 1.0f : 0.0f;
      } else if (parameter.type == RtxExternalEffectParameterType::Int) {
        effect.values[parameter.valueOffset] = std::round(effect.values[parameter.valueOffset]);
      }
    }
  }

  void RtxExternalEffects::ensureLoaded(DxvkDevice* device) {
    if (!m_loaded) {
      // Marked loaded before the job it starts has finished: discovery is now
      // asynchronous, and repeating it once a frame until the first result
      // lands would spawn a compiler per frame.
      m_loaded = true;
      reload(device, false);
    }
  }

  void RtxExternalEffects::reload(DxvkDevice* device, bool forceCompile) {
    if (m_reloadRunning) {
      // Coalesced rather than queued. Two saves during one compile want one
      // rebuild of the final text, not two rebuilds of two intermediate ones,
      // and a forced request must not be downgraded by an unforced one
      // arriving behind it.
      m_reloadPending = true;
      m_reloadPendingForce = m_reloadPendingForce || forceCompile;
      return;
    }
    startReload(forceCompile);
  }

  void RtxExternalEffects::startReload(bool forceCompile) {
    ReloadRequest request;
    request.searchPath = resolveSearchPath();
    request.compiler = findCompiler();
    request.forceCompile = forceCompile;

    // Both of these read RtxOptions, so both happen here rather than on the
    // worker: option storage is render-thread state and a second reader is one
    // more thing that has to be right for no gain.
    for (const Effect& effect : m_effects) {
      request.enabled[effect.manifest.id] = effect.enabled;
    }
    for (const auto& state : parseAssignments(effectEnabledStates())) {
      if (request.enabled.find(state.first) == request.enabled.end()) {
        request.enabled[state.first] = state.second == "1";
      }
    }

    std::error_code error;
    if (!std::filesystem::exists(request.searchPath, error)) {
      std::filesystem::create_directories(request.searchPath, error);
    }
    error.clear();
    if (std::filesystem::is_directory(request.searchPath, error)) {
      armFileWatch(request.searchPath);
    }

    m_reloadResult = std::make_unique<PendingReload>();
    m_reloadDone.store(false, std::memory_order_release);
    m_reloadRunning = true;
    m_reloadStatus = "Compiling external effects...";

    PendingReload* target = m_reloadResult.get();
    m_reloadThread = dxvk::thread([this, request = std::move(request), target]() {
      env::setThreadName("rtx-remixfx-compile");
      runReloadJob(request, *target, m_reloadCancelled);
      // Last thing the worker does. The render thread joins before it reads
      // the result, so the join is what actually orders the two; this is only
      // how it learns that joining will not block.
      m_reloadDone.store(true, std::memory_order_release);
    });
    // Behind everything the frame needs. A shader rebuild is never the most
    // urgent thing in a game process, and slangc is mostly waiting on itself.
    m_reloadThread.set_priority(ThreadPriority::Lowest);
  }

  void RtxExternalEffects::runReloadJob(
    const ReloadRequest& request,
    PendingReload& result,
    const std::atomic<bool>& cancelled) {
    std::error_code error;
    result.compilerFound = !request.compiler.empty();

    if (!std::filesystem::is_directory(request.searchPath, error)) {
      result.reloadStatus =
        "External effect directory is unavailable: " + request.searchPath.u8string();
      Logger::warn(result.reloadStatus);
      return;
    }
    error.clear();
    result.searchPathAvailable = true;

    std::vector<std::filesystem::path> sources;
    for (std::filesystem::recursive_directory_iterator it(
           request.searchPath, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
      if (it->is_regular_file(error)
       && endsWith(it->path().filename().u8string(), ".remixfx.slang")) {
        sources.push_back(it->path());
      }
    }
    std::sort(sources.begin(), sources.end());

    std::unordered_set<std::string> ids;

    for (const std::filesystem::path& source : sources) {
      // Between effects rather than mid-effect: an abandoned job's result is
      // thrown away whole, so stopping early only saves the compiles that had
      // not started yet.
      if (cancelled.load(std::memory_order_acquire)) {
        return;
      }

      std::ifstream sourceStream(source);
      RtxExternalEffectManifest manifest;
      std::string manifestError;
      if (!sourceStream || !parseRtxExternalEffectManifest(
            sourceStream, fallbackIdFromPath(source), manifest, manifestError)) {
        Logger::err(str::format("External effect metadata failed for '", source.u8string(), "': ", manifestError));
        result.failedEffectCount++;
        continue;
      }
      if (!ids.insert(manifest.id).second) {
        Logger::err(str::format("Duplicate external effect id '", manifest.id, "' in ", source.u8string()));
        result.failedEffectCount++;
        continue;
      }

      PendingEffect pending;
      pending.manifest = std::move(manifest);
      pending.sourcePath = source;

      // Unknown directives load the effect rather than rejecting it, so the
      // only way an author learns a key was dropped is if we say so here.
      std::string manifestWarnings;
      for (const std::string& warning : pending.manifest.warnings) {
        Logger::warn(str::format("External effect '", pending.manifest.id, "': ", warning));
        manifestWarnings += manifestWarnings.empty() ? warning : "\n" + warning;
      }

      // Texture identity reaches the shader as a define rather than through a
      // generated header: nothing is written into the author's directory, so
      // there is no stale header to race against and nothing to clean up.
      // The same path is what a UI-editable 'define' directive will ride on.
      std::vector<std::wstring> defines;
      for (size_t i = 0; i < pending.manifest.textures.size(); i++) {
        defines.push_back(
          L"-DREMIXFX_TEX_" + widenIdentifier(pending.manifest.textures[i].id)
          + L"=" + std::to_wstring(i));
      }

      // The cache is per entry point, so a new pass compiles on its own while
      // the others are reused.
      bool spirvExists = true;
      bool sourceOutOfDate = false;
      for (const RtxExternalEffectPass& pass : pending.manifest.passes) {
        const std::filesystem::path spirvPath = spirvPathForPass(source, pass.entryPoint);
        if (!std::filesystem::is_regular_file(spirvPath, error)) {
          spirvExists = false;
          error.clear();
          continue;
        }
        const std::filesystem::file_time_type spirvTime =
          std::filesystem::last_write_time(spirvPath, error);
        if (error) {
          spirvExists = false;
          error.clear();
          continue;
        }

        if (std::filesystem::last_write_time(source, error) > spirvTime) {
          sourceOutOfDate = true;
        }
        error.clear();

        // Every file slangc actually opened, recorded by '-depfile' beside the
        // module. This is the only reason editing remixfx_bindings.slangh
        // rebuilds anything: the source's own mtime says nothing about a file
        // it includes. An absent list is not an error - a hand-shipped .spv
        // has none - and falls the check back to the source alone.
        for (const std::filesystem::path& dependency :
               parseDependencyFile(dependencyPathForPass(spirvPath))) {
          const std::filesystem::file_time_type dependencyTime =
            std::filesystem::last_write_time(dependency, error);
          if (!error && dependencyTime > spirvTime) {
            sourceOutOfDate = true;
          }
          error.clear();
        }
      }
      const bool needsCompile = request.forceCompile || !spirvExists || sourceOutOfDate;

      std::string effectError;
      std::string compilerOutput;
      if (needsCompile && !request.compiler.empty()) {
        for (const RtxExternalEffectPass& pass : pending.manifest.passes) {
          std::string passError;
          std::string passOutput;
          compileEffect(
            request.compiler, source, spirvPathForPass(source, pass.entryPoint),
            pass.entryPoint, defines, cancelled, passError, passOutput);
          if (!passOutput.empty()) {
            compilerOutput += compilerOutput.empty() ? passOutput : "\n" + passOutput;
          }
          // Stop at the first failure. Later passes almost always fail for the
          // same reason, and three copies of one diagnostic is a worse panel
          // than one copy.
          if (!passError.empty()) {
            effectError = pending.manifest.passes.size() > 1
              ? "pass '" + pass.id + "': " + passError
              : passError;
            break;
          }
        }
      } else if (needsCompile && !spirvExists) {
        // Name the file the runtime actually looks for. spirvPathForPass builds
        // '<name>.remixfx.<entry>.spv', one per entry point, so an author who
        // shipped the single '<name>.remixfx.spv' the old message named would
        // have watched it be ignored with nothing to explain why.
        effectError =
          "slangc.exe was not found and no precompiled '<name>.remixfx.<entry>.spv' exists "
          "(one per pass entry point)";
      } else if (needsCompile) {
        pending.status = "Source is newer than cached SPIR-V; configure slangc.exe to rebuild it.";
      }

      // The exit code says a compile failed; only slangc's own diagnostics say
      // which line, and they are the difference between a fixable error and a
      // shrug. They ride along with the message everywhere it is shown.
      if (!effectError.empty() && !compilerOutput.empty()) {
        effectError += "\n" + compilerOutput;
      }

      if (!effectError.empty() && spirvExists) {
        std::string cachedShaderError;
        if (reflectPasses(pending.manifest, source, pending.passes, cachedShaderError)) {
          pending.status = effectError + "; using cached SPIR-V.";
          effectError.clear();
          result.failedEffectCount++;
        } else {
          effectError += "; cached SPIR-V also failed: " + cachedShaderError;
        }
      } else if (effectError.empty()) {
        reflectPasses(pending.manifest, source, pending.passes, effectError);
      }

      if (!effectError.empty()) {
        // Whether to fall back to the modules already on screen is decided on
        // the render thread, which is the only side that can see them.
        pending.passes.clear();
        pending.error = effectError;
        Logger::err(str::format("External effect '", pending.manifest.id, "': ", effectError));
        result.failedEffectCount++;
      } else if (pending.status.empty() && !compilerOutput.empty()) {
        // A compile that succeeded and still printed something is a warning,
        // and warnings that nobody sees are the ones that become bugs.
        pending.status = compilerOutput;
      }

      // Images are decoded last and only for an effect that would otherwise
      // run. The outcome is absolute where a failed compile's is not: there is
      // no previous image to fall back to the way there is a previous shader,
      // because the descriptor that would be bound is the missing one.
      if (!pending.passes.empty()) {
        const auto enabled = request.enabled.find(pending.manifest.id);
        const bool keepBytes = enabled != request.enabled.end()
          ? enabled->second
          : pending.manifest.enabledByDefault;

        std::string imageError;
        if (!decodeFileTextures(
              pending.manifest, source, keepBytes, pending.fileImages, imageError)) {
          pending.passes.clear();
          pending.fileImagesFailed = true;
          pending.status = pending.status.empty()
            ? imageError
            : pending.status + "\n" + imageError;
          Logger::err(str::format("External effect '", pending.manifest.id, "': ", imageError));
          result.failedEffectCount++;
        }
      }

      if (!manifestWarnings.empty()) {
        pending.status = pending.status.empty()
          ? manifestWarnings
          : manifestWarnings + "\n" + pending.status;
      }

      result.effects.push_back(std::move(pending));
    }

    result.reloadStatus = str::format(
      "Found ", result.effects.size(), " external effect(s) in ", request.searchPath.u8string(),
      result.failedEffectCount == 0 ? "." : str::format("; ", result.failedEffectCount, " failed."));
  }

  void RtxExternalEffects::applyReload(DxvkDevice* device, PendingReload&& reload) {
    m_compilerFound = reload.compilerFound;
    m_reloadStatus = reload.reloadStatus;
    if (!reload.searchPathAvailable) {
      // Nothing was discovered, which is not the same as discovering nothing.
      // Replacing the effect set here would drop every effect on screen
      // because a directory was busy for one reload.
      return;
    }

    std::vector<Effect> reloadedEffects;
    reloadedEffects.reserve(reload.effects.size());

    for (PendingEffect& pending : reload.effects) {
      // Still the old set at this point, which is what makes carrying values
      // and history across possible at all.
      Effect* previous = findEffect(pending.manifest.id);

      Effect effect;
      effect.manifest = std::move(pending.manifest);
      effect.sourcePath = std::move(pending.sourcePath);
      effect.fileImages = std::move(pending.fileImages);
      effect.status = std::move(pending.status);
      loadPersistedState(effect, previous);

      std::string effectError = std::move(pending.error);
      if (effectError.empty()) {
        createPasses(device, effect, previous, pending.passes, effectError);
      }

      if (!effectError.empty()) {
        // Falling back to the previous modules keeps a working effect on
        // screen while its source is mid-edit, but only while the manifest
        // still describes the layout they were built with. Each module carries
        // the slot array its pass's read and write lists produced, so a
        // changed list means the old shader would be bound against descriptors
        // it does not have.
        bool previousMatches = previous != nullptr
          && previous->passes.size() == effect.manifest.passes.size()
          && previous->manifest.textures.size() == effect.manifest.textures.size();
        for (size_t i = 0; previousMatches && i < effect.manifest.passes.size(); i++) {
          const RtxExternalEffectPass& before = previous->manifest.passes[i];
          const RtxExternalEffectPass& after = effect.manifest.passes[i];
          previousMatches = before.entryPoint == after.entryPoint
            && before.reads == after.reads
            && before.writes == after.writes;
        }
        // Same read list, different descriptor type: a texture that changed
        // between a file and a pass-written one moves its read slot between a
        // combined image sampler and a plain sampled image, and the old module
        // was built against whichever it used to be.
        for (size_t i = 0; previousMatches && i < effect.manifest.textures.size(); i++) {
          previousMatches = previous->manifest.textures[i].file.empty()
                         == effect.manifest.textures[i].file.empty();
        }
        // A decode failure is not a shader failure: the image the missing
        // descriptor would carry is missing either way, so there is nothing
        // for the old module to be bound against.
        if (previousMatches && !pending.fileImagesFailed) {
          effect.passes = previous->passes;
          effect.status = effect.status.empty()
            ? effectError + "; using the previous shader."
            : effect.status + "\n" + effectError + "; using the previous shader.";
        } else {
          effect.passes.clear();
          effect.status = effect.status.empty()
            ? effectError
            : effect.status + "\n" + effectError;
        }
      }

      carryTexturesAcrossReload(effect, previous);
      reloadedEffects.push_back(std::move(effect));
    }

    m_effects = std::move(reloadedEffects);
    if (!m_reloadStatus.empty()) {
      Logger::info(m_reloadStatus);
    }
  }

  void RtxExternalEffects::carryTexturesAcrossReload(Effect& effect, Effect* previous) const {
    if (previous == nullptr || previous->textures.empty()) {
      return;
    }

    // Sized against the new manifest up front, so a texture that moved index
    // or stopped existing simply has nothing moved into it.
    effect.textures.assign(effect.manifest.textures.size(), EffectTexture());

    bool carried = false;
    for (size_t i = 0; i < effect.manifest.textures.size(); i++) {
      const RtxExternalEffectTexture& declaration = effect.manifest.textures[i];
      // History only. Scratch is refilled by the next frame's passes, and a
      // file texture is re-uploaded on purpose: editing the image is one of
      // the things a reload exists to pick up.
      if (!declaration.persist || !declaration.file.empty()) {
        continue;
      }

      const auto before = std::find_if(
        previous->manifest.textures.begin(), previous->manifest.textures.end(),
        [&](const RtxExternalEffectTexture& candidate) {
          // Matched by id, but only carried when the declaration that sized
          // and formatted it is unchanged: a texture whose format was edited
          // holds bits that no longer mean what the shader will read.
          return candidate.id == declaration.id
              && candidate.persist
              && candidate.file.empty()
              && candidate.format == declaration.format
              && candidate.divisor == declaration.divisor
              && candidate.width == declaration.width
              && candidate.height == declaration.height;
        });
      if (before == previous->manifest.textures.end()) {
        continue;
      }

      const size_t index = std::distance(previous->manifest.textures.begin(), before);
      if (index >= previous->textures.size() || !previous->textures[index].resource.isValid()) {
        continue;
      }

      effect.textures[i] = std::move(previous->textures[index]);
      previous->textures[index] = EffectTexture();
      carried = true;
    }

    if (!carried) {
      // Back to empty rather than a vector of holes: the disabled path calls
      // releaseTextures every frame and its early-out is what keeps that free.
      effect.textures.clear();
      return;
    }

    // What was carried is only meaningful against the extent it was sized
    // for. Handing that extent across is what stops ensureTextures reading the
    // set as a resize and releasing the history it just kept.
    effect.textureExtent = previous->textureExtent;
    // The whole point of carrying it. An auto-exposure ramp that survived the
    // reload has not lost its history, and telling the shader it has would
    // reset the image to black on every save.
    effect.historyInvalid = previous->historyInvalid;
  }

  uint64_t RtxExternalEffects::textureMemoryBytes(const Effect& effect) {
    uint64_t bytes = 0;
    for (const EffectTexture& texture : effect.textures) {
      if (texture.resource.isValid()) {
        // The allocation rather than extent times texel size: a compressed
        // file texture has no texel size, and alignment and tiling padding are
        // VRAM the effect is holding whether or not it asked for them.
        bytes += texture.resource.image->memSize();
      }
    }
    return bytes;
  }

  std::vector<RtxExternalEffectInfo> RtxExternalEffects::effectInfos() const {
    std::vector<RtxExternalEffectInfo> result;
    result.reserve(m_effects.size());
    for (const Effect& effect : m_effects) {
      result.push_back({
        effect.manifest.id,
        effect.manifest.name,
        effect.manifest.domain,
        effect.enabled,
        !effect.passes.empty(),
        effect.status,
      });
    }
    return result;
  }

  bool RtxExternalEffects::hasEffect(const std::string& id) const {
    return findEffect(id) != nullptr;
  }

  bool RtxExternalEffects::setEffectEnabled(const std::string& id, bool enabled) {
    Effect* effect = findEffect(id);
    if (effect == nullptr || effect->enabled == enabled) {
      return false;
    }
    effect->enabled = enabled;
    // Whatever a persistent texture held describes a scene the effect was not
    // running in. Arming the reset on enable rather than on disable means an
    // effect toggled off and on inside one frame still sees it.
    effect->historyInvalid = true;
    persistEnabledStates();
    return true;
  }

  bool RtxExternalEffects::getParameter(
    const std::string& effectId,
    const std::string& parameterId,
    std::vector<float>& values) const {
    const Effect* effect = findEffect(effectId);
    if (effect == nullptr) {
      return false;
    }

    const auto parameter = std::find_if(
      effect->manifest.parameters.begin(), effect->manifest.parameters.end(),
      [&](const RtxExternalEffectParameter& candidate) { return candidate.id == parameterId; });
    if (parameter == effect->manifest.parameters.end()) {
      return false;
    }

    values.assign(
      effect->values.begin() + parameter->valueOffset,
      effect->values.begin() + parameter->valueOffset + parameter->valueCount);
    return true;
  }

  bool RtxExternalEffects::setParameter(
    const std::string& effectId,
    const std::string& parameterId,
    const std::vector<float>& values) {
    Effect* effect = findEffect(effectId);
    if (effect == nullptr) {
      return false;
    }

    const auto parameter = std::find_if(
      effect->manifest.parameters.begin(), effect->manifest.parameters.end(),
      [&](const RtxExternalEffectParameter& candidate) { return candidate.id == parameterId; });
    if (parameter == effect->manifest.parameters.end() || values.size() != parameter->valueCount) {
      return false;
    }

    if (!std::all_of(values.begin(), values.end(), [](float value) {
          return std::isfinite(value);
        })) {
      return false;
    }

    for (uint32_t i = 0; i < parameter->valueCount; i++) {
      effect->values[parameter->valueOffset + i] = std::clamp(
        values[i], parameter->minValues[i], parameter->maxValues[i]);
    }
    if (parameter->type == RtxExternalEffectParameterType::Bool) {
      effect->values[parameter->valueOffset] = effect->values[parameter->valueOffset] >= 0.5f ? 1.0f : 0.0f;
    } else if (parameter->type == RtxExternalEffectParameterType::Int) {
      effect->values[parameter->valueOffset] = std::round(effect->values[parameter->valueOffset]);
    }
    persistParameterValues();
    return true;
  }

  // Rebuilding a persisted string from m_effects alone drops every effect the
  // runtime does not currently hold - a shader moved aside for the afternoon,
  // one whose manifest has a typo this session, one that failed to compile -
  // and drops it the moment the user touches some *other* effect's control.
  // There is no way back, because the value leaves the option before the effect
  // returns to claim it. So an entry keyed to an id we do not know about is
  // carried through untouched rather than rewritten away.
  //
  // Both strings are ';'-separated 'key=value' and both keys lead with the
  // effect id - bare for enabled states, 'id/parameter' for values - so one
  // merge serves both.
  static std::string mergeUnknownPersistedEntries(
    const std::string& fresh,
    const std::string& stored,
    const std::vector<std::string>& knownEffectIds) {
    std::string merged = fresh;

    size_t begin = 0;
    while (begin < stored.size()) {
      const size_t end = stored.find(';', begin);
      const size_t length = end == std::string::npos ? std::string::npos : end - begin;
      const std::string entry = stored.substr(begin, length);
      begin = end == std::string::npos ? stored.size() : end + 1;

      const size_t equals = entry.find('=');
      if (entry.empty() || equals == std::string::npos) {
        continue;
      }

      const std::string key = entry.substr(0, equals);
      const size_t slash = key.find('/');
      const std::string effectId = slash == std::string::npos ? key : key.substr(0, slash);

      if (std::find(knownEffectIds.begin(), knownEffectIds.end(), effectId)
            != knownEffectIds.end()) {
        continue;
      }

      if (!merged.empty()) {
        merged += ";";
      }
      merged += entry;
    }

    return merged;
  }

  void RtxExternalEffects::persistEnabledStates() {
    std::string serialized;
    std::vector<std::string> knownEffectIds;
    knownEffectIds.reserve(m_effects.size());

    for (const Effect& effect : m_effects) {
      if (!serialized.empty()) {
        serialized += ";";
      }
      serialized += effect.manifest.id + "=" + (effect.enabled ? "1" : "0");
      knownEffectIds.push_back(effect.manifest.id);
    }

    effectEnabledStatesObject().setDeferred(
      mergeUnknownPersistedEntries(serialized, effectEnabledStates(), knownEffectIds));
  }

  void RtxExternalEffects::persistParameterValues() {
    std::ostringstream stream;
    stream << std::setprecision(9);
    bool first = true;
    std::vector<std::string> knownEffectIds;
    knownEffectIds.reserve(m_effects.size());

    for (const Effect& effect : m_effects) {
      knownEffectIds.push_back(effect.manifest.id);
      for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
        if (!first) {
          stream << ";";
        }
        first = false;
        stream << effect.manifest.id << "/" << parameter.id << "=";
        for (uint32_t i = 0; i < parameter.valueCount; i++) {
          if (i != 0) {
            stream << " ";
          }
          stream << effect.values[parameter.valueOffset + i];
        }
      }
    }

    // Keyed by the effect id, so an effect present but with a parameter renamed
    // still loses that one parameter's value - which is correct, the old key
    // names nothing any more. It is only a whole missing effect that is kept.
    effectParameterValuesObject().setDeferred(
      mergeUnknownPersistedEntries(stream.str(), effectParameterValues(), knownEffectIds));
  }

  void RtxExternalEffects::dispatch(
    Rc<RtxContext> ctx,
    Resources::RaytracingOutput& rtOutput,
    const std::string& id) {
    ensureLoaded(ctx->getDevice().ptr());
    Effect* effect = findEffect(id);
    if (effect == nullptr) {
      return;
    }
    if (!enabled() || !effect->enabled || effect->passes.empty()) {
      // An effect that is not running has no reason to hold its working set;
      // the half resolution buffers a gather-style effect declares are tens of
      // megabytes at 1600p. The history it was told to keep stays, because
      // that is the whole difference 'persist' makes, and so do its file
      // inputs, which would cost a decode to get back.
      releaseTextures(*effect, TextureReleaseScope::Scratch);
      return;
    }

    ScopedGpuProfileZone(ctx, effect->manifest.name.c_str());
    ctx->setFramePassStage(RtxFramePassStage::PostFX);
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const Resources::Resource& input = rtOutput.m_finalOutput.resource(Resources::AccessType::Read);
    const VkExtent3D extent = input.image->info().extent;

    std::string textureError;
    if (!ensureTextures(ctx, *effect, extent, textureError)) {
      // Dispatching anyway would bind null descriptors to the writes and lose
      // the frame's image; stopping keeps whatever the previous stage produced.
      effect->status = textureError;
      Logger::err(str::format("External effect '", effect->manifest.id, "': ", textureError));
      return;
    }

    const Camera& camera = rtOutput.m_raytraceArgs.camera;
    const Resources::Resource& exposure = ctx->getCommonObjects()->metaAutoExposure().getExposureTexture();
    DxvkPostFx& postFx = ctx->getCommonObjects()->metaPostFx();
    const Resources::Resource& focusState = postFx.getDofFocusState();
    const Rc<DxvkImageView> blueNoise = ctx->getResourceManager().getBlueNoiseTexture(ctx);
    const Rc<DxvkSampler> nearestSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const Rc<DxvkSampler> linearSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    // The other half of 'texture.<name>.repeat'. Linear in both cases: at a
    // one-to-one mapping linear and nearest agree, so the filter only matters
    // where a file is being stretched, and a lookup table being read between
    // its entries is the case that wants interpolation rather than a step.
    const Rc<DxvkSampler> repeatSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
      VK_SAMPLER_ADDRESS_MODE_REPEAT);

    uint32_t availableInputs = 0;
    availableInputs |= rtOutput.m_primaryLinearViewZ.isValid() ? ExternalEffectInputLinearDepth : 0;
    availableInputs |= rtOutput.m_primaryScreenSpaceMotionVector.isValid() ? ExternalEffectInputMotion : 0;
    availableInputs |= rtOutput.m_primaryWorldShadingNormal.isValid() ? ExternalEffectInputWorldNormal : 0;
    availableInputs |= rtOutput.m_primaryAlbedo.isValid() ? ExternalEffectInputAlbedo : 0;
    availableInputs |= rtOutput.m_primaryPerceptualRoughness.isValid() ? ExternalEffectInputRoughness : 0;
    availableInputs |= rtOutput.m_primarySurfaceFlags.isValid() ? ExternalEffectInputSurfaceFlags : 0;
    availableInputs |= rtOutput.m_primaryObjectPicking.isValid() ? ExternalEffectInputObjectPicking : 0;
    availableInputs |= rtOutput.m_primaryConeRadius.isValid() ? ExternalEffectInputConeRadius : 0;
    availableInputs |= blueNoise != nullptr ? ExternalEffectInputBlueNoise : 0;
    availableInputs |= exposure.isValid() ? ExternalEffectInputExposure : 0;
    availableInputs |= postFx.isDofAutoFocusEnabled() && focusState.isValid()
      ? ExternalEffectInputFocusState : 0;
    availableInputs |= rtOutput.m_primaryDepth.isValid() ? ExternalEffectInputProjectedDepth : 0;

    ExternalEffectFrameData frameData = {};
    frameData.worldToView = camera.worldToView;
    frameData.viewToWorld = camera.viewToWorld;
    frameData.viewToProjection = camera.viewToProjection;
    frameData.projectionToView = camera.projectionToView;
    frameData.viewToProjectionJittered = camera.viewToProjectionJittered;
    frameData.projectionToViewJittered = camera.projectionToViewJittered;
    frameData.worldToProjectionJittered = camera.worldToProjectionJittered;
    frameData.projectionToWorldJittered = camera.projectionToWorldJittered;
    frameData.translatedWorldToView = camera.translatedWorldToView;
    frameData.translatedWorldToProjectionJittered = camera.translatedWorldToProjectionJittered;
    frameData.projectionToTranslatedWorld = camera.projectionToTranslatedWorld;
    frameData.previousWorldToView = camera.prevWorldToView;
    frameData.previousViewToWorld = camera.prevViewToWorld;
    frameData.previousWorldToProjection = camera.prevWorldToProjection;
    frameData.previousWorldToProjectionJittered = camera.prevWorldToProjectionJittered;
    frameData.previousProjectionToView = camera.prevProjectionToView;
    frameData.previousProjectionToViewJittered = camera.prevProjectionToViewJittered;
    frameData.previousTranslatedWorldToView = camera.prevTranslatedWorldToView;
    frameData.previousTranslatedWorldToProjection = camera.prevTranslatedWorldToProjection;
    frameData.projectionToPreviousProjectionJittered = camera.projectionToPrevProjectionJittered;
    frameData.outputSize[0] = extent.width;
    frameData.outputSize[1] = extent.height;
    frameData.renderSize[0] = camera.resolution.x;
    frameData.renderSize[1] = camera.resolution.y;
    frameData.invRenderSize[0] = camera.resolution.x != 0 ? 1.0f / static_cast<float>(camera.resolution.x) : 0.0f;
    frameData.invRenderSize[1] = camera.resolution.y != 0 ? 1.0f / static_cast<float>(camera.resolution.y) : 0.0f;
    frameData.nearPlane = camera.nearPlane;
    frameData.meterToWorldScale = RtxOptions::getMeterToWorldUnitScale();
    frameData.cameraFlags = camera.flags;
    frameData.availableInputs = availableInputs;
    frameData.linearDepthMissValue = rtOutput.m_raytraceArgs.primaryDirectMissLinearViewZ;
    frameData.deltaTimeSeconds = GlobalTime::get().deltaTime();
    frameData.manualFocusDistance = DxvkPostFx::focusDistance();
    frameData.autoFocusOffset = DxvkPostFx::autoFocusOffset();
    frameData.autoFocusEnabled = postFx.isDofAutoFocusEnabled() ? 1 : 0;
    frameData.historyInvalid = effect->historyInvalid ? 1 : 0;
    // A cut is not a history reset the runtime can decide on: a temporal blur
    // wants to drop its accumulation across one, an auto-exposure ramp may
    // want to keep it. The effect is told and chooses.
    frameData.cameraCut = ctx->getSceneManager().getCamera().isCameraCut() ? 1 : 0;

    ExternalEffectParameterData parameterData = {};
    parameterData.valueCount = effect->manifest.parameterValueCount;
    std::copy(effect->values.begin(), effect->values.end(), parameterData.values);

    DxvkDevice* device = ctx->getDevice().ptr();
    if (m_frameDataBuffer == nullptr || m_uniformBufferDevice != device) {
      DxvkBufferCreateInfo info = {};
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.size = sizeof(frameData);
      m_frameDataBuffer = device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "RemixFX Frame Data");

      info.size = sizeof(parameterData);
      m_parameterBuffer = device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "RemixFX Parameters");
      m_uniformBufferDevice = device;
    }
    ctx->writeToBuffer(m_frameDataBuffer, 0, sizeof(frameData), &frameData);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_frameDataBuffer);
    ctx->writeToBuffer(m_parameterBuffer, 0, sizeof(parameterData), &parameterData);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_parameterBuffer);

    ExternalEffectArgs args = {};
    args.imageSize[0] = extent.width;
    args.imageSize[1] = extent.height;
    args.invImageSize[0] = 1.0f / static_cast<float>(extent.width);
    args.invImageSize[1] = 1.0f / static_cast<float>(extent.height);
    args.timeSeconds = static_cast<float>(GlobalTime::get().absoluteTimeMs()) / 1000.0f;
    args.frameIndex = ctx->getDevice()->getCurrentFrameId();

    ctx->bindResourceSampler(kInputBinding, linearSampler);
    ctx->bindResourceView(kLinearDepthBinding, rtOutput.m_primaryLinearViewZ.view, nullptr);
    ctx->bindResourceView(kMotionBinding, rtOutput.m_primaryScreenSpaceMotionVector.view, nullptr);
    ctx->bindResourceView(kWorldNormalBinding, rtOutput.m_primaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(kAlbedoBinding, rtOutput.m_primaryAlbedo.view, nullptr);
    ctx->bindResourceView(kRoughnessBinding, rtOutput.m_primaryPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(kSurfaceFlagsBinding, rtOutput.m_primarySurfaceFlags.view, nullptr);
    ctx->bindResourceView(
      kObjectPickingBinding,
      rtOutput.m_primaryObjectPicking.isValid() ? rtOutput.m_primaryObjectPicking.view : nullptr,
      nullptr);
    ctx->bindResourceView(kConeRadiusBinding, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(kBlueNoiseBinding, blueNoise, nullptr);
    ctx->bindResourceView(kExposureBinding, exposure.isValid() ? exposure.view : nullptr, nullptr);
    ctx->bindResourceSampler(kSceneSamplerBinding, nearestSampler);
    ctx->bindResourceBuffer(
      kFrameDataBinding,
      DxvkBufferSlice(m_frameDataBuffer, 0, m_frameDataBuffer->info().size));
    ctx->bindResourceSampler(kSceneLinearSamplerBinding, linearSampler);
    ctx->bindResourceView(
      kFocusStateBinding,
      focusState.isValid() ? focusState.view : nullptr,
      nullptr);
    ctx->bindResourceView(
      kProjectedDepthBinding,
      rtOutput.m_primaryDepth.isValid() ? rtOutput.m_primaryDepth.view : nullptr,
      nullptr);
    ctx->bindResourceBuffer(
      kParameterDataBinding,
      DxvkBufferSlice(m_parameterBuffer, 0, m_parameterBuffer->info().size));

    for (size_t passIndex = 0; passIndex < effect->passes.size(); passIndex++) {
      const RtxExternalEffectPass& pass = effect->manifest.passes[passIndex];
      const CompiledPass& compiled = effect->passes[passIndex];
      ScopedGpuProfileZone(ctx, pass.id.c_str());

      // Binding 0 is the colour this pass reads, which is the scene input
      // unless the pass asked for what the effect has written so far. Both are
      // the same descriptor, so nothing in the shader has to change; only the
      // image behind it does.
      const bool readsOutput = std::find(
        pass.reads.begin(), pass.reads.end(),
        kRtxExternalEffectOutputTexture) != pass.reads.end();
      ctx->bindResourceView(
        kInputBinding,
        readsOutput
          ? rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Read)
          : input.view,
        nullptr);

      // AliasedResource on this branch (shared with the DLSS-NR input): the
      // effect shader must claim ownership with an explicit Write before the
      // copy below reads it. A pass that does not write the output leaves the
      // slot empty, which is also what its layout says.
      const bool writesOutput = std::find(
        pass.writes.begin(), pass.writes.end(),
        kRtxExternalEffectOutputTexture) != pass.writes.end();
      ctx->bindResourceView(
        kOutputBinding,
        writesOutput
          ? rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Write)
          : nullptr,
        nullptr);

      // Bound per pass rather than once: a slot left pointing at last pass's
      // texture would be tracked as accessed by this one, and dxvk would build
      // barriers for a dependency that is not there.
      for (uint32_t i = 0; i < effect->manifest.textures.size(); i++) {
        const bool reads = std::find(pass.reads.begin(), pass.reads.end(), i) != pass.reads.end();
        const bool writes = std::find(pass.writes.begin(), pass.writes.end(), i) != pass.writes.end();
        ctx->bindResourceView(
          kUserTextureReadBinding + i, reads ? effect->textures[i].resource.view : nullptr, nullptr);
        // A file texture's read slot is a combined image sampler, so the
        // sampler half has to be written to the same slot. Which one it is
        // comes from the manifest and not from the shader, because the address
        // mode belongs to the image: a grain plate is meaningless clamped and
        // a lookup table is meaningless wrapped.
        if (reads && !effect->manifest.textures[i].file.empty()) {
          ctx->bindResourceSampler(
            kUserTextureReadBinding + i,
            effect->manifest.textures[i].repeat ? repeatSampler : linearSampler);
        }
        ctx->bindResourceView(
          kUserTextureWriteBinding + i, writes ? effect->textures[i].resource.view : nullptr, nullptr);
      }

      const VkExtent3D passExtent = pass.over == kRtxExternalEffectOutputTexture
        ? extent
        : effect->textures[pass.over].extent;
      // A 'once' pass is one workgroup, so what it covers is its own local
      // size rather than any texture's extent.
      args.dispatchSize[0] = pass.once ? compiled.localSize.width : passExtent.width;
      args.dispatchSize[1] = pass.once ? compiled.localSize.height : passExtent.height;
      args.passIndex = static_cast<uint32_t>(passIndex);
      ctx->pushConstants(0, sizeof(args), &args);

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, compiled.shader);

      // The group grid comes from the module's own [numthreads], so a shader
      // that changes its workgroup size cannot leave the runtime dispatching
      // the old one and covering the wrong number of pixels.
      const VkExtent3D workgroups = pass.once
        ? VkExtent3D { 1, 1, 1 }
        : util::computeBlockCount(passExtent, compiled.localSize);
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    }

    // Consumed: an effect that runs every frame sees the reset for exactly one.
    effect->historyInvalid = false;

    ctx->copyImage(
      rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
      rtOutput.m_postFxIntermediateTexture.image(Resources::AccessType::Read),
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, extent);
  }

  void RtxExternalEffects::showGlobalSettings(DxvkDevice* device) {
    RemixGui::Checkbox("External Effects Enabled", &enabledObject());
    RemixGui::Checkbox("Recompile On Save", &hotReloadObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Watches the search path and rebuilds an effect when its source, or a file it includes, "
      "changes. Compilation runs on a worker thread, so a save does not stall the frame.");
    RemixGui::InputText("Effect Search Path", &effectSearchPathObject());
    RemixGui::InputText("Slang Compiler Path", &slangCompilerPathObject());

    // Disabled rather than hidden while a job is in flight: a button that
    // vanishes mid-click is worse than one that says it is busy.
    ImGui::BeginDisabled(m_reloadRunning);
    if (ImGui::Button("Reload External Effects")) {
      reload(device, true);
    }
    ImGui::EndDisabled();
    if (!m_reloadStatus.empty()) {
      ImGui::TextWrapped("%s", m_reloadStatus.c_str());
    }

    uint64_t totalBytes = 0;
    for (const Effect& effect : m_effects) {
      totalBytes += textureMemoryBytes(effect);
    }
    if (totalBytes != 0) {
      ImGui::TextDisabled("Effect textures: %s", formatByteCount(totalBytes).c_str());
    }

    // Only once a job has answered the question. Before that the flag is
    // merely unset, and saying the compiler is missing while it is running is
    // worse than saying nothing.
    if (m_loaded && !m_reloadRunning && !m_compilerFound) {
      ImGui::TextDisabled("slangc.exe was not found; precompiled .remixfx.spv files can still be loaded.");
    }
  }

  void RtxExternalEffects::showEffectSettings(const std::string& id) {
    Effect* effect = findEffect(id);
    if (effect == nullptr) {
      return;
    }

    if (!effect->status.empty()) {
      ImGui::TextWrapped("%s", effect->status.c_str());
    }
    ImGui::TextDisabled("%s", effect->sourcePath.u8string().c_str());

    // Surfaced per effect because this is where the cost is: a gather-style
    // effect's half resolution working set is tens of megabytes at 1600p, and
    // the number an author needs is the one for the effect they are editing.
    // Zero for an effect that is switched off is not a gap in the reporting -
    // it is the scratch release doing its job.
    if (!effect->manifest.textures.empty()) {
      const uint64_t bytes = textureMemoryBytes(*effect);
      ImGui::TextDisabled("Textures: %s", formatByteCount(bytes).c_str());
      if (ImGui::IsItemHovered()) {
        std::string breakdown;
        for (size_t i = 0; i < effect->manifest.textures.size(); i++) {
          const RtxExternalEffectTexture& declaration = effect->manifest.textures[i];
          const bool allocated = i < effect->textures.size()
                              && effect->textures[i].resource.isValid();
          breakdown += breakdown.empty() ? "" : "\n";
          breakdown += declaration.id;
          if (!allocated) {
            breakdown += ": not allocated";
            continue;
          }
          const VkExtent3D& extent = effect->textures[i].extent;
          breakdown += ": " + std::to_string(extent.width) + "x" + std::to_string(extent.height)
            + "  " + formatByteCount(effect->textures[i].resource.image->memSize());
          if (declaration.persist) {
            breakdown += "  (persist)";
          } else if (!declaration.file.empty()) {
            breakdown += "  (file)";
          }
        }
        RemixGui::SetTooltipUnformattedUnwrapped(breakdown.c_str());
      }
    }

    // These go through RemixGui rather than raw ImGui so external parameters
    // get the same label column, row hover and formatting as every built-in
    // option row. The manifest guarantees min <= max for each component, so the
    // declared bounds can be handed straight to the widget with AlwaysClamp:
    // the drag then stops at the limit instead of overshooting and snapping
    // back on the post-edit clamp below.
    bool changed = false;
    // 'category' is sticky in the manifest, so every parameter carrying one is
    // already contiguous with its neighbours: a group opens when the category
    // changes and closes when it changes again. Nothing needs sorting, and
    // declaration order - which is the author's order - survives intact.
    const std::string* openCategory = nullptr;
    bool categoryOpen = true;

    for (const RtxExternalEffectParameter& parameter : effect->manifest.parameters) {
      if (openCategory == nullptr || parameter.category != *openCategory) {
        if (openCategory != nullptr && !openCategory->empty()) {
          ImGui::Unindent();
        }
        openCategory = &parameter.category;
        categoryOpen = true;
        if (!parameter.category.empty()) {
          // Open by default: a settings pane that starts as a column of closed
          // headers hides the thing the author came here to move.
          categoryOpen = RemixGui::CollapsingHeader(
            parameter.category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
          ImGui::Indent();
        }
      }
      if (!categoryOpen) {
        continue;
      }

      float* values = effect->values.data() + parameter.valueOffset;
      bool parameterChanged = false;
      ImGui::PushID(parameter.id.c_str());

      switch (parameter.type) {
      case RtxExternalEffectParameterType::Bool: {
        bool value = values[0] >= 0.5f;
        if (RemixGui::Checkbox(parameter.name.c_str(), &value)) {
          values[0] = value ? 1.0f : 0.0f;
          parameterChanged = true;
        }
        break;
      }
      case RtxExternalEffectParameterType::Int: {
        int value = static_cast<int>(std::lround(values[0]));
        // 'items' turns the int into a named choice. The parser has already
        // checked that there is exactly one label per representable value and
        // that the step is 1, so the index into the list is the value minus
        // its minimum and needs no further guarding.
        if (!parameter.items.empty()) {
          const int minimum = static_cast<int>(parameter.minValues[0]);
          std::vector<const char*> labels;
          labels.reserve(parameter.items.size());
          for (const std::string& item : parameter.items) {
            labels.push_back(item.c_str());
          }
          int selected = std::clamp(
            value - minimum, 0, static_cast<int>(labels.size()) - 1);
          if (RemixGui::Combo(
                parameter.name.c_str(), &selected, labels.data(),
                static_cast<int>(labels.size()))) {
            values[0] = static_cast<float>(selected + minimum);
            parameterChanged = true;
          }
          break;
        }
        if (RemixGui::DragInt(
              parameter.name.c_str(), &value,
              std::max(1.0f, parameter.step),
              static_cast<int>(parameter.minValues[0]),
              static_cast<int>(parameter.maxValues[0]),
              "%d", ImGuiSliderFlags_AlwaysClamp)) {
          values[0] = static_cast<float>(value);
          parameterChanged = true;
        }
        break;
      }
      case RtxExternalEffectParameterType::Float:
        parameterChanged |= RemixGui::DragFloat(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Float2:
        parameterChanged |= RemixGui::DragFloat2(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Float3:
        parameterChanged |= RemixGui::DragFloat3(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Float4:
        parameterChanged |= RemixGui::DragFloat4(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Color3:
        parameterChanged |= RemixGui::ColorEdit3(
          parameter.name.c_str(), values,
          ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        break;
      case RtxExternalEffectParameterType::Color4:
        parameterChanged |= RemixGui::ColorEdit4(
          parameter.name.c_str(), values,
          ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        break;
      }

      // The same hover delay every built-in option row uses, and unformatted
      // because the text is the author's: a '%' in a tooltip about percentages
      // must not be read as a format specifier.
      if (!parameter.tooltip.empty()) {
        RemixGui::SetTooltipToLastWidgetOnHover(parameter.tooltip.c_str());
      }

      ImGui::PopID();

      if (parameterChanged) {
        // Still clamp per component: a vector parameter may declare different
        // bounds per component, and the color editors carry no bounds at all.
        for (uint32_t i = 0; i < parameter.valueCount; i++) {
          values[i] = std::clamp(values[i], parameter.minValues[i], parameter.maxValues[i]);
        }
        changed = true;
      }
    }

    if (openCategory != nullptr && !openCategory->empty()) {
      ImGui::Unindent();
    }

    if (changed) {
      persistParameterValues();
    }
  }

} // namespace dxvk
