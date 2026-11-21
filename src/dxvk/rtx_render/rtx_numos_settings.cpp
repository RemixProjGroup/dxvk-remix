#include "rtx_numos_settings.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "numos/log.h"
#include "rtx_options.h"
#include "../../util/log/log.h"

namespace dxvk {

  namespace NumosSettings {

    namespace {

      template <typename T>
      T& field(numos::Settings& settings, const numos::SettingInfo& info) {
        return *reinterpret_cast<T*>(reinterpret_cast<char*>(&settings) + info.offset);
      }

      template <typename T>
      const T& field(const numos::Settings& settings, const numos::SettingInfo& info) {
        return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&settings) + info.offset);
      }

      Vector3 toRemix(const numos::Vector3& v) { return Vector3(v.x, v.y, v.z); }
      numos::Vector3 toNumos(const Vector3& v) { return numos::Vector3(v.x, v.y, v.z); }

      // The settings the user menu shows (showNumosUserHaze, showNumosUserClouds and showNumosUserNight in
      // rtx_numos_ui.cpp). They are player settings, so they save to user.conf.
      constexpr const char* kUserSettingKeys[] = {
        "rtx.atmosphere.aerialPerspective",
        "rtx.atmosphere.aerosolDensity",
        "rtx.atmosphere.hazeWorldScale",
        "rtx.atmosphere.fogVisibilityMeters",
        "rtx.atmosphere.aerialPerspectiveSceneShadow",
        "rtx.atmosphere.aerialPerspectiveLocalLights",
        "rtx.atmosphere.cloudEnabled",
        "rtx.atmosphere.cloudCoverageMean",
        "rtx.atmosphere.cloudDensity",
        "rtx.atmosphere.cloudTypeMean",
        "rtx.atmosphere.cloudShadowStrength",
        "rtx.atmosphere.cloudPerformancePreset",
        "rtx.atmosphere.cloudMarchRate",
        "rtx.atmosphere.nightGainStops",
        "rtx.atmosphere.starGainStops",
        "rtx.atmosphere.moonlightGainStops",
        "rtx.atmosphere.moonDiscGainStops",
      };

      bool isUserSetting(const numos::SettingInfo& info) {
        for (const char* key : kUserSettingKeys) {
          if (info.key == key) {
            return true;
          }
        }
        return false;
      }

      template <typename T>
      RtxOptionArgs<T> optionArgs(const numos::SettingInfo& info) {
        RtxOptionArgs<T> args;
        if (info.flags & numos::NUMOS_SETTING_NO_SAVE) {
          args.flags |= RtxOptionFlags::NoSave;
        }
        if (isUserSetting(info)) {
          args.flags |= RtxOptionFlags::UserSetting;
        }
        if constexpr (!std::is_same_v<T, bool>) {
          if (info.minValue != numos::NUMOS_NO_MIN) {
            if constexpr (std::is_same_v<T, Vector3>) {
              args.minValue = Vector3(info.minValue);
            } else {
              args.minValue = T(info.minValue);
            }
          }
          if (info.maxValue != numos::NUMOS_NO_MAX) {
            if constexpr (std::is_same_v<T, Vector3>) {
              args.maxValue = Vector3(info.maxValue);
            } else {
              args.maxValue = T(info.maxValue);
            }
          }
        }
        return args;
      }

      // RtxOption's constructors are private to RTX_OPTION's factory, whose result is a prvalue: a
      // member initialised from it is constructed in place, at its final address.
      template <typename T>
      struct Held {
        RtxOption<T> option;
        Held(const char* category, const char* name, const T& value, const char* description, RtxOptionArgs<T> args)
          : option(RtxOption<T>::privateMacroFactory(category, name, value, description, args)) { }
      };

      // Built once, at static initialisation, before the config files load. The deques never move
      // their elements, which the option registry points at for the life of the process.
      struct Registry {
        std::deque<std::string> names;  // category, name pairs the options keep pointers to
        std::deque<Held<bool>> bools;
        std::deque<Held<int32_t>> ints;
        std::deque<Held<uint32_t>> uints;
        std::deque<Held<float>> floats;
        std::deque<Held<Vector3>> vec3s;
        std::vector<Entry> entries;

        Registry() {
          const numos::Settings defaults {};
          const std::vector<numos::SettingInfo>& infos = numos::settingInfos();
          entries.resize(infos.size());
          for (size_t i = 0; i < infos.size(); ++i) {
            const numos::SettingInfo& info = infos[i];
            Entry& e = entries[i];
            e.info = &info;
            const size_t dot = info.key.rfind('.');
            const char* category = names.emplace_back(info.key.substr(0, dot)).c_str();
            const char* name = names.emplace_back(info.key.substr(dot + 1)).c_str();
            const char* description = info.description ? info.description : "";
            RtxOptionImpl* registered = nullptr;
            switch (info.type) {
            case numos::SettingType::Bool:
              e.boolOption = &bools.emplace_back(category, name, field<bool>(defaults, info), description, optionArgs<bool>(info)).option;
              registered = e.boolOption;
              break;
            case numos::SettingType::Int:
              e.intOption = &ints.emplace_back(category, name, field<int32_t>(defaults, info), description, optionArgs<int32_t>(info)).option;
              registered = e.intOption;
              break;
            case numos::SettingType::Uint:
              e.uintOption = &uints.emplace_back(category, name, field<uint32_t>(defaults, info), description, optionArgs<uint32_t>(info)).option;
              registered = e.uintOption;
              break;
            case numos::SettingType::Float:
              e.floatOption = &floats.emplace_back(category, name, field<float>(defaults, info), description, optionArgs<float>(info)).option;
              registered = e.floatOption;
              break;
            case numos::SettingType::Vec3:
              e.vec3Option = &vec3s.emplace_back(category, name, toRemix(field<numos::Vector3>(defaults, info)), description, optionArgs<Vector3>(info)).option;
              registered = e.vec3Option;
              break;
            }
            // A key another option already owns: the registry refused this one, so leave it unused.
            if (RtxOptionImpl::getOptionByFullName(info.key) != registered) {
              e.boolOption = nullptr;
              e.intOption = nullptr;
              e.uintOption = nullptr;
              e.floatOption = nullptr;
              e.vec3Option = nullptr;
            }
          }
        }
      };

      Registry& registry() {
        static Registry r;
        return r;
      }

      // Registers the options at static initialisation, like every RTX_OPTION.
      const Registry& g_registry = registry();

      numos::Settings g_resolved;
      uint64_t g_resolvedGeneration = ~0ull;
      float g_resolvedSceneScale = -1.0f;
    }

    RtxOptionImpl* Entry::option() const {
      if (boolOption) return boolOption;
      if (intOption) return intOption;
      if (uintOption) return uintOption;
      if (floatOption) return floatOption;
      return vec3Option;
    }

    size_t count() {
      return registry().entries.size();
    }

    const Entry& entry(size_t index) {
      return registry().entries[index];
    }

    const Entry* find(const char* key) {
      const numos::SettingInfo* info = numos::findSetting(key);
      if (info == nullptr) {
        return nullptr;
      }
      const size_t index = size_t(info - numos::settingInfos().data());
      return index < count() ? &entry(index) : nullptr;
    }

    namespace {
      // Writes one setting's value from `s` into `layer` (null: the current RtxOptionLayerTarget),
      // when it differs from the option's resolved value.
      void writeEntry(const Entry& e, const numos::Settings& s, const RtxOptionLayer* layer) {
        if (e.boolOption) {
          if (e.boolOption->get() != field<bool>(s, *e.info)) e.boolOption->setDeferred(field<bool>(s, *e.info), layer);
        } else if (e.intOption) {
          if (e.intOption->get() != field<int32_t>(s, *e.info)) e.intOption->setDeferred(field<int32_t>(s, *e.info), layer);
        } else if (e.uintOption) {
          if (e.uintOption->get() != field<uint32_t>(s, *e.info)) e.uintOption->setDeferred(field<uint32_t>(s, *e.info), layer);
        } else if (e.floatOption) {
          if (e.floatOption->get() != field<float>(s, *e.info)) e.floatOption->setDeferred(field<float>(s, *e.info), layer);
        } else if (e.vec3Option) {
          const Vector3 v = toRemix(field<numos::Vector3>(s, *e.info));
          if (e.vec3Option->get() != v) e.vec3Option->setDeferred(v, layer);
        }
      }

      // The non-default layers that hold a value for `option` and take part in its resolve, strongest
      // first. "Set by a config" is presence in one of these, never "differs from the default": a
      // config line or a dev-menu edit that sets a value equal to the default is still a choice.
      std::vector<const RtxOptionLayer*> configLayers(const RtxOptionImpl& option) {
        std::vector<const RtxOptionLayer*> layers;
        const RtxOptionLayer* defaults = RtxOptionLayer::getDefaultLayer();
        option.forEachLayerValue([&](const RtxOptionLayer* layer, const GenericValue&) {
          if (layer != nullptr && layer != defaults) {
            layers.push_back(layer);
          }
          return true;
        });
        return layers;
      }
    }

    const numos::Settings& resolved() {
      const uint64_t generation = g_rtxOptionResolveGeneration.load(std::memory_order_acquire);
      const float sceneScale = RtxOptions::sceneScale();
      if (generation == g_resolvedGeneration && sceneScale == g_resolvedSceneScale) {
        return g_resolved;
      }
      installLog();
      numos::Settings s {};
      numos::ExplicitSettings explicitlySet(count(), false);
      // Deprecated keys some config layer holds: the migration consumes them.
      struct Consumed { const Entry* entry; std::vector<const RtxOptionLayer*> layers; };
      std::vector<Consumed> consumed;
      const RtxOptionLayer* writeBackLayer = nullptr;
      for (size_t i = 0; i < count(); ++i) {
        const Entry& e = entry(i);
        const RtxOptionImpl* option = e.option();
        if (option == nullptr) {
          continue;
        }
        std::vector<const RtxOptionLayer*> layers = configLayers(*option);
        explicitlySet[i] = !layers.empty();
        if (explicitlySet[i] && numos::isDeprecated(*e.info)) {
          // The migrated replacements go to the strongest layer that held a deprecated key, so saving
          // that layer (user.conf, rtx.conf) persists them in place of the keys they came from.
          if (writeBackLayer == nullptr || layers.front()->getLayerKey() < writeBackLayer->getLayerKey()) {
            writeBackLayer = layers.front();
          }
          consumed.push_back({ &e, std::move(layers) });
        }
        if (e.boolOption) field<bool>(s, *e.info) = e.boolOption->get();
        else if (e.intOption) field<int32_t>(s, *e.info) = e.intOption->get();
        else if (e.uintOption) field<uint32_t>(s, *e.info) = e.uintOption->get();
        else if (e.floatOption) field<float>(s, *e.info) = e.floatOption->get();
        else field<numos::Vector3>(s, *e.info) = toNumos(e.vec3Option->get());
      }
      // Starts from the options every time. Once the deprecated keys are consumed (below) nothing is
      // left to migrate, so this is a no-op on every later resolve.
      const numos::Settings before = s;
      numos::migrateDeprecated(s, explicitlySet, sceneScale);

      // Migrate once: write the changed replacements into the options and drop the deprecated keys
      // from every layer that held them, so the migration never again overrides what the options say
      // (an explicit rtx.atmosphere.worldScale = 1 included) and a save drops them. Both land at the
      // next option resolve, together; until then this frame uses the migrated values computed here.
      if (!consumed.empty()) {
        for (size_t i = 0; i < count(); ++i) {
          const Entry& e = entry(i);
          if (e.option() == nullptr || numos::isDeprecated(*e.info)) {
            continue;
          }
          if (numos::formatSetting(s, *e.info) != numos::formatSetting(before, *e.info)) {
            writeEntry(e, s, writeBackLayer);
            Logger::info(str::format("[NUMOS] Migrated into ", e.info->key, " = ", numos::formatSetting(s, *e.info),
                                     " (layer ", writeBackLayer->getName(), ")."));
          }
        }
        for (const Consumed& c : consumed) {
          RtxOptionImpl* option = c.entry->option();
          for (const RtxOptionLayer* layer : c.layers) {
            {
              std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());
              option->disableLayerValue(layer);
            }
            layer->onLayerValueChanged();
          }
          Logger::info(str::format("[NUMOS] Consumed deprecated ", c.entry->info->key, "; re-save the config to drop it."));
        }
      }
      g_resolved = s;
      g_resolvedGeneration = generation;
      g_resolvedSceneScale = sceneScale;
      return g_resolved;
    }

    void write(const numos::Settings& s) {
      for (size_t i = 0; i < count(); ++i) {
        writeEntry(entry(i), s, nullptr);
      }
    }

    void installLog() {
      static std::once_flag once;
      std::call_once(once, [] {
        numos::setLogCallback([](numos::LogLevel level, const char* message, void*) {
          // migrateDeprecated reports on every resolve, and some NUMOS messages repeat per frame:
          // each distinct message reaches the log once.
          static std::mutex mutex;
          static std::unordered_set<std::string> seen;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (!seen.insert(message ? message : "").second) {
              return;
            }
          }
          const std::string text = std::string("[NUMOS] ") + (message ? message : "");
          switch (level) {
          case numos::LogLevel::Info:  Logger::info(text); break;
          case numos::LogLevel::Warn:  Logger::warn(text); break;
          case numos::LogLevel::Error: Logger::err(text); break;
          }
        }, nullptr);
      });
    }
  }

}
