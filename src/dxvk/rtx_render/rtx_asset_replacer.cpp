/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_asset_replacer.h"

#include "dxvk_device.h"
#include "dxvk_context.h"
#include "rtx_context.h"
#include "rtx_initializer.h"
#include "rtx_options.h"
#include "rtx_utils.h"
#include "rtx_asset_data_manager.h"

namespace dxvk {

// Each lookup asks the winner of a clash on the hash first, then walks the mods from the highest
// precedence down. See documentation/RemixModOrder.md, "Precedence".
std::shared_ptr<const ReplacementBucket> AssetReplacer::getReplacementsForMesh(XXH64_hash_t hash) {
  if (!RtxOptions::getEnableReplacementMeshes())
    return nullptr;

  auto variantInfo = m_variantInfos.find(hash);

  if (variantInfo != m_variantInfos.end()) {
    hash += variantInfo->second.selectedVariant;
  }

  if (Mod* winner = m_modManager.clashWinner(ModAssetKind::Mesh, hash)) {
    if (auto replacement = winner->replacements().get<AssetReplacement::eMesh>(hash)) {
      return replacement;
    }
  }

  const auto& mods = m_modManager.mods();
  for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {
    if (auto replacement = (*mod)->replacements().get<AssetReplacement::eMesh>(hash)) {
      return replacement;
    }
  }

  return nullptr;
}

std::shared_ptr<const ReplacementBucket> AssetReplacer::getReplacementsForLight(XXH64_hash_t hash) {
  if (!RtxOptions::getEnableReplacementLights())
    return nullptr;

  if (Mod* winner = m_modManager.clashWinner(ModAssetKind::Light, hash)) {
    if (auto replacement = winner->replacements().get<AssetReplacement::eLight>(hash)) {
      return replacement;
    }
  }

  const auto& mods = m_modManager.mods();
  for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {
    if (auto replacement = (*mod)->replacements().get<AssetReplacement::eLight>(hash)) {
      return replacement;
    }
  }

  return nullptr;
}

std::shared_ptr<MaterialData> AssetReplacer::getReplacementMaterial(XXH64_hash_t hash) {
  if (!RtxOptions::getEnableReplacementMaterials())
    return nullptr;

  std::shared_ptr<MaterialData> material;
  if (Mod* winner = m_modManager.clashWinner(ModAssetKind::Material, hash)) {
    if (winner->replacements().getMaterial(hash, material)) {
      return material;
    }
  }

  const auto& mods = m_modManager.mods();
  for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {
    if ((*mod)->replacements().getMaterial(hash, material)) {
      return material;
    }
  }

  return nullptr;
}

void AssetReplacer::initialize(const Rc<DxvkContext>& context) {
  for (auto& mod : m_modManager.mods()) {
    // Each mod cancels internally too; this stops us starting the next one. Returning
    // rather than breaking skips the secret-replacement pass, which has nothing to do
    // over a mod set we deliberately stopped populating.
    if (m_modManager.isLoadingCancelled()) {
      return;
    }
    mod->load(context);
  }
  updateSecretReplacements();
}

bool AssetReplacer::checkForChanges(const Rc<DxvkContext>& context) {
  ScopedCpuProfileZone();

  bool changed = false;
  for (auto& mod : m_modManager.mods()) {
    changed |= mod->checkForChanges(context);
  }
  // A clash with a new winner leaves replacements resolved against the previous one in the scene.
  changed |= m_modManager.consumeClashesChanged();
  if (changed) {
    ++m_externalAssetEpoch;
    ++m_externalAssetGlobalEpoch;
    updateSecretReplacements();
  }
  return changed;
}

bool AssetReplacer::applyPendingRebuilds(const Rc<DxvkContext>& context, AssetChanges& changes) {
  ScopedCpuProfileZone();

  bool applied = false;
  for (auto& mod : m_modManager.mods()) {
    applied |= mod->applyPendingRebuild(context, changes);
  }
  if (applied) {
    ++m_externalAssetEpoch;
    ++m_externalAssetGlobalEpoch;
    updateSecretReplacements();
  }
  return applied;
}

void AssetReplacer::onDestroy() {
  for (auto& mod : m_modManager.mods()) {
    mod->onDestroy();
  }
}

bool AssetReplacer::hasAnyMods() const {
  return !m_modManager.mods().empty();
}

std::vector<Mod::State> AssetReplacer::getReplacementStates() const {
  const auto& mods = m_modManager.mods();
  std::vector<Mod::State> modStates;
  modStates.reserve(mods.size());

  for (auto& mod : mods) {
    modStates.emplace_back(mod->state());
  }

  return modStates;
}

void AssetReplacer::requestReload() {
  ++m_externalAssetEpoch;
  ++m_externalAssetGlobalEpoch;
  m_modManager.forgetOwnerFiles();
  for (auto& mod : m_modManager.mods()) {
    mod->requestReload();
  }
}

bool AssetReplacer::isReloadPending() const {
  for (auto& mod : m_modManager.mods()) {
    const auto p = mod->state();
    if (mod->isReloadRequested() || (p.progressState != Mod::ProgressState::Unloaded && p.progressState != Mod::ProgressState::Loaded)) {
      return true;
    }
  }

  return false;
}

void AssetReplacer::updateSecretReplacements() {
  ++m_externalAssetEpoch;
  ++m_externalAssetGlobalEpoch;
  bool updated = false;

  m_secretReplacements.clear();

  for (auto& mod : m_modManager.mods()) {
    if (mod->state().progressState != Mod::ProgressState::Loaded) {
      continue;
    }

    // Pull secret replacement info
    for (auto& secrets : mod->replacements().secretReplacements()) {
      for (auto& secret : secrets.second) {
        m_secretReplacements[secrets.first].emplace_back(
          SecretReplacement{secret.header,
                            secret.name,
                            secret.description,
                            secret.unlockHash,
                            secret.assetHash,
                            secret.replacementPath,
                            secret.bDisplayBeforeUnlocked,
                            secret.bExclusiveReplacement,
                            secret.variantId
          }
        );

        updated = true;
      }
    }
  }

  // Selected variants are user state keyed by asset hash, not data derived from the mod
  // tables, so they outlive a reload. Drop only selections whose variant is gone.
  m_variantInfos.erase_if([this](const auto& it) {
    if (it->second.selectedVariant == VariantInfo::kDefaultVariant) {
      return false;
    }
    const auto secrets = m_secretReplacements.find(it->first);
    if (secrets == m_secretReplacements.end()) {
      return true;
    }
    for (const auto& secret : secrets->second) {
      if (secret.variantId == it->second.selectedVariant) {
        return false;
      }
    }
    return true;
  });

  m_bSecretReplacementsUpdated = updated;
}

namespace {
  std::string tostr(const remixapi_MaterialHandle& h) {
    static_assert(sizeof h == sizeof uint64_t);
    return std::to_string(reinterpret_cast<uint64_t>(h));
  }
  std::string tostr(const remixapi_MeshHandle& h) {
    static_assert(sizeof h == sizeof uint64_t);
    return std::to_string(reinterpret_cast<uint64_t>(h));
  }
}

void AssetReplacer::makeMaterialWithTexturePreload(DxvkContext& ctx, remixapi_MaterialHandle handle, MaterialData&& data) {
  // A create for a live handle replaces its material. try_emplace leaves data intact when the handle exists.
  auto [iter, isNew] = m_extMaterials.try_emplace(handle, std::move(data));
  if (!isNew) {
    iter->second.emplace(std::move(data));
  }

  // A retained entry whose material handle was not registered yet resolved it to "none".
  ++m_externalAssetEpoch;
  noteChangedExternalAsset(reinterpret_cast<uint64_t>(handle));
}

const MaterialData* AssetReplacer::accessExternalMaterial(remixapi_MaterialHandle handle) const {
  auto found = m_extMaterials.find(handle);
  if (found == m_extMaterials.end()) {
    return nullptr;
  }
  return found->second ? &found->second.value() : nullptr;
}

void AssetReplacer::destroyExternalMaterial(remixapi_MaterialHandle handle) {
  ++m_externalAssetEpoch;
  noteChangedExternalAsset(reinterpret_cast<uint64_t>(handle));
  m_extMaterials.erase(handle);
}

void AssetReplacer::noteTextureReplaced(const TextureRef& ref) {
  if (ref.isValid()) {
    m_pendingTextureRefreshes[ref.getUniqueKey()] = ref;
  }
}

uint32_t AssetReplacer::applyTextureRefreshes() {
  if (m_pendingTextureRefreshes.empty()) {
    return 0;
  }
  uint32_t refreshed = 0;
  auto refresh = [&](TextureRef& tex) {
    if (!tex.isValid()) {
      return;
    }
    auto found = m_pendingTextureRefreshes.find(tex.getUniqueKey());
    if (found == m_pendingTextureRefreshes.end()) {
      return;
    }
    // The material hash sums image hashes, so only a ref with the same image hash is swapped.
    const TextureRef& fresh = found->second;
    if (tex.getImageHash() == fresh.getImageHash() && tex.getImageView() != fresh.getImageView()) {
      tex = fresh;
      ++refreshed;
    }
  };
  for (auto& [handle, material] : m_extMaterials) {
    if (!material) {
      continue;
    }
    switch (material->getType()) {
    case MaterialDataType::Opaque: {
      OpaqueMaterialData& m = material->getOpaqueMaterialData();
      refresh(m.getAlbedoOpacityTexture());
      refresh(m.getNormalTexture());
      refresh(m.getTangentTexture());
      refresh(m.getHeightTexture());
      refresh(m.getRoughnessTexture());
      refresh(m.getMetallicTexture());
      refresh(m.getEmissiveColorTexture());
      refresh(m.getSubsurfaceTransmittanceTexture());
      refresh(m.getSubsurfaceThicknessTexture());
      refresh(m.getSubsurfaceSingleScatteringAlbedoTexture());
      refresh(m.getSubsurfaceRadiusTexture());
      refresh(m.getSecondaryTexture());
      break;
    }
    case MaterialDataType::Translucent: {
      TranslucentMaterialData& m = material->getTranslucentMaterialData();
      refresh(m.getNormalTexture());
      refresh(m.getTransmittanceTexture());
      refresh(m.getEmissiveColorTexture());
      break;
    }
    case MaterialDataType::RayPortal: {
      RayPortalMaterialData& m = material->getRayPortalMaterialData();
      refresh(m.getMaskTexture());
      refresh(m.getMaskTexture2());
      break;
    }
    default:
      break;
    }
  }
  m_pendingTextureRefreshes.clear();
  return refreshed;
}

bool AssetReplacer::registerExternalMesh(remixapi_MeshHandle handle, std::vector<RasterGeometry>&& submeshes,
                                         bool refreshGeometry) {
  auto existing = m_extMeshes.find(handle);
  if (existing != m_extMeshes.end()) {
    // In-place geometry refresh for CPU-deformed meshes that keep their topology. It is opt-in, because
    // the ordinary re-create path re-submits byte-identical content, and reallocating buffers for that
    // would churn the BLAS of geometry that never moved.
    //
    // The refresh publishes a new vector instead of assigning through the shared_ptr. Callers alias raw
    // pointers into the elements of what accessExternalMesh returns (submitExternalDraw holds one across
    // its submesh loop), so an in-flight reader finishes against the geometry it started with.
    // The old buffers stay alive through the last holder and DXVK's lifetime tracking.
    if (!refreshGeometry || existing->second->size() != submeshes.size()) {
      return false;
    }

    for (size_t i = 0; i < submeshes.size(); i++) {
      const GeometryHashes& prev = (*existing->second)[i].hashes;
      GeometryHashes& next = submeshes[i].hashes;

      // Carry over every hash except VertexPosition. DrawCallCache::get buckets on TopologicalHash
      // (Indices | GeometryDescriptor), so it returns the same BlasEntry. SceneManager::processGeometryInfo
      // then sees equal Indices and a new VertexPosition and takes kUpdateBVH, which gives motion vectors.
      next[HashComponents::Indices]            = prev[HashComponents::Indices];
      next[HashComponents::GeometryDescriptor] = prev[HashComponents::GeometryDescriptor];
      next[HashComponents::VertexTexcoord]     = prev[HashComponents::VertexTexcoord];
      next[HashComponents::VertexLayout]       = prev[HashComponents::VertexLayout];
      next.precombine();

      submeshes[i].externalMesh = handle;
    }

    existing->second = std::make_shared<std::vector<RasterGeometry>>(std::move(submeshes));
    ++m_externalAssetEpoch;
    noteChangedExternalAsset(reinterpret_cast<uint64_t>(handle));
    return true;
  }

  // Tag each submesh with the external mesh handle so capture + runtime
  // use the same identity for replacement-lookup parity.
  for (auto& submesh : submeshes) {
    submesh.externalMesh = handle;
  }

  m_extMeshes.emplace(handle, std::make_shared<std::vector<RasterGeometry>>(std::move(submeshes)));
  // Retained commands reach the scene through their own queue, so an instance can be built before the command
  // stream registers its mesh. Such an entry waits under this handle (RetainedScene::buildDirect) and is rebuilt now.
  noteChangedExternalAsset(reinterpret_cast<uint64_t>(handle));
  return true;
}

std::shared_ptr<const std::vector<RasterGeometry>> AssetReplacer::accessExternalMesh(remixapi_MeshHandle handle) const {
  auto found = m_extMeshes.find(handle);
  if (found == m_extMeshes.end()) {
    static const auto s_empty = std::make_shared<const std::vector<RasterGeometry>>();
    return s_empty;
  }
  return found->second;
}

void AssetReplacer::destroyExternalMesh(remixapi_MeshHandle handle) {
  ++m_externalAssetEpoch;
  noteChangedExternalAsset(reinterpret_cast<uint64_t>(handle));
  m_extMeshes.erase(handle);
}

} // namespace dxvk

