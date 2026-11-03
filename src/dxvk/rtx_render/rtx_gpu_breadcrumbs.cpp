#include "rtx_gpu_breadcrumbs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../dxvk_device.h"

namespace dxvk {

  namespace {
    // The marker buffer is filled with this value. It is never used as a sequence number.
    constexpr uint32_t kUnwritten = 0xFFFFFFFFu;

    // Zones are strictly nested per thread, so a per-thread stack gives each zone its parent.
    struct ZoneStack {
      uint32_t seqs[GpuBreadcrumbs::kMaxDepth] = {};
      uint32_t depth = 0;
    };
    thread_local ZoneStack t_zoneStack;

    std::string hex(uint64_t value) {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
      return buf;
    }

    const char* faultAddressTypeName(VkDeviceFaultAddressTypeEXT type) {
      switch (type) {
      case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
      case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read invalid";
      case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write invalid";
      case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute invalid";
      case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer unknown";
      case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction pointer invalid";
      case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer fault";
      default: return "unknown type";
      }
    }
  }


  std::unique_ptr<GpuBreadcrumbs> GpuBreadcrumbs::create(DxvkDevice* device) {
    const bool markers = device->extensions().amdBufferMarker;
    const bool fault = device->extensions().extDeviceFault;
    if (!markers && !fault) {
      return nullptr;
    }

    std::unique_ptr<GpuBreadcrumbs> result(new GpuBreadcrumbs(device));
    const bool markersReady = markers && result->initMarkers();
    // Without the fault report there is no address to match, so the ranges are not kept.
    result->m_trackAddresses = fault;
    if (fault) {
      result->m_freedRanges.resize(kFreedRanges);
    }
    Logger::info(str::format("[DeviceFault] Device-loss diagnostics: pass markers ", markersReady ? "on" : "off",
      ", VK_EXT_device_fault ", fault ? "on" : "off", ", buffer address ranges ", fault ? "on" : "off"));
    return result;
  }


  GpuBreadcrumbs::GpuBreadcrumbs(DxvkDevice* device)
  : m_device(device),
    m_vkd(device->vkd()) {
  }


  GpuBreadcrumbs::~GpuBreadcrumbs() {
    if (m_buffer != VK_NULL_HANDLE) {
      m_vkd->vkDestroyBuffer(m_vkd->device(), m_buffer, nullptr);
    }
    // Freeing the memory also unmaps it.
    if (m_memory != VK_NULL_HANDLE) {
      m_vkd->vkFreeMemory(m_vkd->device(), m_memory, nullptr);
    }
  }


  bool GpuBreadcrumbs::initMarkers() {
    const VkDeviceSize size = VkDeviceSize(kSlots) * 2 * sizeof(uint32_t);

    // Zones are recorded on several queues, so the buffer is shared by every queue family in use.
    std::vector<uint32_t> families;
    const DxvkDeviceQueueSet& queues = m_device->queues();
    for (const DxvkDeviceQueue* q : { &queues.graphics, &queues.transfer, &queues.asyncCompute,
                                      &queues.present, &queues.imageAcquire, &queues.fsrPresent }) {
      if (q->queueHandle != VK_NULL_HANDLE && std::find(families.begin(), families.end(), q->queueFamily) == families.end()) {
        families.push_back(q->queueFamily);
      }
    }

    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (families.size() > 1) {
      bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
      bufferInfo.queueFamilyIndexCount = uint32_t(families.size());
      bufferInfo.pQueueFamilyIndices = families.data();
    } else {
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (m_vkd->vkCreateBuffer(m_vkd->device(), &bufferInfo, nullptr, &m_buffer) != VK_SUCCESS) {
      m_buffer = VK_NULL_HANDLE;
      Logger::warn("[DeviceFault] Failed to create the pass marker buffer.");
      return false;
    }

    VkMemoryRequirements req = {};
    m_vkd->vkGetBufferMemoryRequirements(m_vkd->device(), m_buffer, &req);

    // The memory is host-visible and coherent so the CPU can read it after device loss without a Vulkan call.
    const VkPhysicalDeviceMemoryProperties memProps = m_device->adapter()->memoryProperties();
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & wanted) == wanted) {
        typeIndex = i;
        break;
      }
    }
    if (typeIndex == UINT32_MAX) {
      Logger::warn("[DeviceFault] No host-visible coherent memory type for the pass marker buffer.");
      return false;
    }

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    if (m_vkd->vkAllocateMemory(m_vkd->device(), &allocInfo, nullptr, &m_memory) != VK_SUCCESS) {
      m_memory = VK_NULL_HANDLE;
      Logger::warn("[DeviceFault] Failed to allocate the pass marker buffer.");
      return false;
    }

    void* mapped = nullptr;
    if (m_vkd->vkBindBufferMemory(m_vkd->device(), m_buffer, m_memory, 0) != VK_SUCCESS
     || m_vkd->vkMapMemory(m_vkd->device(), m_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      Logger::warn("[DeviceFault] Failed to bind or map the pass marker buffer.");
      return false;
    }

    std::memset(mapped, 0xFF, size_t(size));
    m_entries.reset(new Entry[kSlots]);
    m_mapped = static_cast<const volatile uint32_t*>(mapped);
    return true;
  }


  uint32_t GpuBreadcrumbs::begin(VkCommandBuffer cmdBuffer, const char* name) {
    if (m_mapped == nullptr || cmdBuffer == VK_NULL_HANDLE) {
      return 0;
    }

    uint32_t seq = m_nextSeq.fetch_add(1, std::memory_order_relaxed);
    while (seq == 0 || seq == kUnwritten) {
      seq = m_nextSeq.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t slot = seq & (kSlots - 1);
    Entry& e = m_entries[slot];
    ZoneStack& stack = t_zoneStack;

    // Readers skip an entry whose seq changes while they copy it.
    e.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    e.frameId = m_device->getCurrentFrameId();
    e.parentSeq = stack.depth != 0 ? stack.seqs[std::min(stack.depth, kMaxDepth) - 1] : 0;
    e.depth = uint16_t(std::min<uint32_t>(stack.depth, 0xFFFF));
    std::strncpy(e.name, name != nullptr ? name : "?", kNameLength - 1);
    e.name[kNameLength - 1] = '\0';
    e.seq.store(seq, std::memory_order_release);

    if (stack.depth < kMaxDepth) {
      stack.seqs[stack.depth] = seq;
    }
    stack.depth += 1;

    m_vkd->vkCmdWriteBufferMarkerAMD(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_buffer,
                                     VkDeviceSize(slot) * sizeof(uint32_t), seq);
    return seq;
  }


  void GpuBreadcrumbs::end(VkCommandBuffer cmdBuffer, uint32_t seq) {
    if (seq == 0) {
      return;
    }
    ZoneStack& stack = t_zoneStack;
    if (stack.depth != 0) {
      stack.depth -= 1;
    }
    if (cmdBuffer == VK_NULL_HANDLE) {
      return;
    }
    const uint32_t slot = seq & (kSlots - 1);
    m_vkd->vkCmdWriteBufferMarkerAMD(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_buffer,
                                     VkDeviceSize(kSlots + slot) * sizeof(uint32_t), seq);
  }


  void GpuBreadcrumbs::noteBufferCreated(VkDeviceAddress address, VkDeviceSize size, const char* name) {
    if (!m_trackAddresses || address == 0) {
      return;
    }
    AddressRange range;
    range.begin = address;
    range.size = size;
    range.frame = m_device->getCurrentFrameId();
    std::strncpy(range.name, name != nullptr ? name : "?", kNameLength - 1);

    std::lock_guard<dxvk::mutex> lock(m_addressMutex);
    m_liveRanges[address] = range;
  }


  void GpuBreadcrumbs::noteBufferDestroyed(VkDeviceAddress address, VkDeviceSize size, const char* name) {
    if (!m_trackAddresses || address == 0) {
      return;
    }
    const uint32_t frame = m_device->getCurrentFrameId();

    std::lock_guard<dxvk::mutex> lock(m_addressMutex);
    AddressRange range;
    auto it = m_liveRanges.find(address);
    if (it != m_liveRanges.end()) {
      range = it->second;
      m_liveRanges.erase(it);
    } else {
      range.begin = address;
      range.size = size;
      std::strncpy(range.name, name != nullptr ? name : "?", kNameLength - 1);
    }
    range.frame = frame;
    m_freedRanges[m_freedCursor] = range;
    m_freedCursor = (m_freedCursor + 1) % kFreedRanges;
  }


  std::string GpuBreadcrumbs::describeAddress(VkDeviceAddress address, uint64_t precision) {
    // The driver rounds the address down to its precision, which the specification says is a power of two.
    const bool powerOfTwo = precision != 0 && (precision & (precision - 1)) == 0;
    const uint64_t lo = powerOfTwo ? (address & ~(precision - 1)) : address;
    const uint64_t hi = lo + std::max<uint64_t>(precision, 1);
    const auto contains = [address](const AddressRange& r) {
      return address >= r.begin && address - r.begin < r.size;
    };
    const auto overlaps = [lo, hi](const AddressRange& r) {
      return r.begin < hi && lo < r.begin + r.size;
    };
    const auto describe = [](const AddressRange& r, const char* frameWord) {
      return str::format("\"", r.name, "\" [", hex(r.begin), ", ", hex(r.begin + r.size), ") ", r.size, " bytes, ",
        frameWord, " frame ", r.frame);
    };

    std::lock_guard<dxvk::mutex> lock(m_addressMutex);
    std::string s;
    auto it = m_liveRanges.upper_bound(address);
    if (it != m_liveRanges.begin() && contains((--it)->second)) {
      s = "in live buffer " + describe(it->second, "created");
    }

    // The freed buffers that held the address, newest first.
    std::vector<const AddressRange*> freed;
    for (const AddressRange& r : m_freedRanges) {
      if (r.size != 0 && contains(r)) {
        freed.push_back(&r);
      }
    }
    std::sort(freed.begin(), freed.end(), [](const AddressRange* a, const AddressRange* b) { return a->frame > b->frame; });
    if (!freed.empty()) {
      s += s.empty() ? "held by freed buffer" : "; held before by freed buffer";
      for (size_t i = 0; i < freed.size() && i < 4; ++i) {
        s += (i == 0 ? " " : " | ") + describe(*freed[i], "freed");
      }
      if (freed.size() > 4) {
        s += str::format(" | ... ", freed.size() - 4, " more");
      }
    }
    if (!s.empty()) {
      return s;
    }

    // No buffer holds the exact address, so list what the imprecise range overlaps.
    uint32_t shown = 0;
    for (const auto& [begin, r] : m_liveRanges) {
      if (begin >= hi) {
        break;
      }
      if (overlaps(r) && shown < 4) {
        s += (shown == 0 ? "no buffer holds the address; the reported range overlaps live buffer " : " | live ")
          + describe(r, "created");
        ++shown;
      }
    }
    for (const AddressRange& r : m_freedRanges) {
      if (r.size != 0 && overlaps(r) && shown < 8) {
        s += (shown == 0 ? "no buffer holds the address; the reported range overlaps freed buffer " : " | freed ")
          + describe(r, "freed");
        ++shown;
      }
    }
    if (s.empty()) {
      s = "no buffer with a device address holds or recently held it (images and driver-internal memory are not tracked)";
    }
    return s;
  }


  void GpuBreadcrumbs::logDeviceLost(const char* reason) {
    if (m_logged.exchange(true)) {
      return;
    }
    Logger::err(str::format("[DeviceFault] Device lost (", reason != nullptr ? reason : "?", "), Remix frame ",
      m_device->getCurrentFrameId(), "."));
    logDeviceFault();
    logMarkers();
  }


  namespace {
    std::string describeZone(const char* name, uint32_t seq, uint32_t frameId) {
      return str::format(name, " (seq ", seq, ", frame ", frameId, ")");
    }
  }


  void GpuBreadcrumbs::logMarkers() {
    if (m_mapped == nullptr) {
      Logger::err("[DeviceFault] Pass markers: unavailable (VK_AMD_buffer_marker is not enabled).");
      return;
    }

    std::vector<Snapshot> v;
    v.reserve(kSlots);
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      const Entry& e = m_entries[slot];
      const uint32_t seq = e.seq.load(std::memory_order_acquire);
      if (seq == 0) {
        continue;
      }
      Snapshot s;
      s.seq = seq;
      s.frameId = e.frameId;
      s.parentSeq = e.parentSeq;
      s.depth = e.depth;
      std::memcpy(s.name, e.name, kNameLength);
      s.name[kNameLength - 1] = '\0';
      std::atomic_thread_fence(std::memory_order_acquire);
      if (e.seq.load(std::memory_order_relaxed) != seq) {
        continue;
      }
      s.started = m_mapped[slot] == seq;
      s.ended = m_mapped[kSlots + slot] == seq;
      v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](const Snapshot& a, const Snapshot& b) { return a.seq < b.seq; });

    const auto describe = [](const Snapshot& s) { return describeZone(s.name, s.seq, s.frameId); };

    std::vector<size_t> inFlight;
    size_t lastStarted = SIZE_MAX;
    size_t lastEnded = SIZE_MAX;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].started) {
        lastStarted = i;
      }
      if (v[i].ended) {
        lastEnded = i;
      }
      if (v[i].started && !v[i].ended) {
        inFlight.push_back(i);
      }
    }

    Logger::err(str::format("[DeviceFault] Pass markers: the GPU began up to ",
      lastStarted != SIZE_MAX ? describe(v[lastStarted]) : std::string("none in the ring"),
      " and finished up to ", lastEnded != SIZE_MAX ? describe(v[lastEnded]) : std::string("none in the ring"), "."));

    // The oldest in-flight zone without an in-flight child is the innermost pass the GPU was executing.
    size_t culprit = SIZE_MAX;
    for (size_t idx : inFlight) {
      bool hasChild = false;
      for (size_t other : inFlight) {
        if (v[other].parentSeq == v[idx].seq) {
          hasChild = true;
          break;
        }
      }
      if (!hasChild) {
        culprit = idx;
        break;
      }
    }

    if (inFlight.empty()) {
      Logger::err("[DeviceFault] Passes in flight: none. The fault happened outside every marked pass, after the last finished one.");
    } else {
      Logger::err(str::format("[DeviceFault] Passes in flight (began, never finished, oldest first): ", inFlight.size(),
        culprit != SIZE_MAX ? str::format(", likely culprit: ", v[culprit].name) : std::string()));
      constexpr size_t kMaxInFlight = 32;
      for (size_t n = 0; n < inFlight.size() && n < kMaxInFlight; ++n) {
        const Snapshot& s = v[inFlight[n]];
        Logger::err(str::format("[DeviceFault]   ", std::string(size_t(std::min<uint16_t>(s.depth, 16)) * 2, ' '), describe(s),
          inFlight[n] == culprit ? "  <-- innermost and oldest" : ""));
      }
      if (inFlight.size() > kMaxInFlight) {
        Logger::err(str::format("[DeviceFault]   ... ", inFlight.size() - kMaxInFlight, " more"));
      }
    }

    {
      std::vector<size_t> ended;
      for (size_t i = v.size(); i-- > 0 && ended.size() < 8;) {
        if (v[i].ended) {
          ended.push_back(i);
        }
      }
      std::string line = "[DeviceFault] Last finished passes (oldest first):";
      if (ended.empty()) {
        line += " none";
      }
      for (size_t n = ended.size(); n-- > 0;) {
        line += " | " + describe(v[ended[n]]);
      }
      Logger::err(line);
    }

    {
      std::string line = "[DeviceFault] Next passes, never started:";
      uint32_t shown = 0;
      uint32_t total = 0;
      const uint32_t after = lastStarted != SIZE_MAX ? v[lastStarted].seq : 0;
      for (const Snapshot& s : v) {
        if (s.seq > after && !s.started) {
          if (shown < 8) {
            line += " | " + describe(s);
            ++shown;
          }
          ++total;
        }
      }
      if (total == 0) {
        line += " none";
      } else if (total > shown) {
        line += str::format(" | ... ", total - shown, " more");
      }
      Logger::err(line);
    }
  }


  void GpuBreadcrumbs::logDeviceFault() {
    if (!m_device->extensions().extDeviceFault || m_vkd->vkGetDeviceFaultInfoEXT == nullptr) {
      Logger::err("[DeviceFault] VK_EXT_device_fault: not enabled on this device.");
      return;
    }

    VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    VkResult result = m_vkd->vkGetDeviceFaultInfoEXT(m_vkd->device(), &counts, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      Logger::err(str::format("[DeviceFault] VK_EXT_device_fault: the count query returned ", result, "."));
      return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    info.pVendorBinaryData = nullptr;
    // The vendor binary is not fetched because deviceFaultVendorBinary is not enabled.
    counts.vendorBinarySize = 0;
    result = m_vkd->vkGetDeviceFaultInfoEXT(m_vkd->device(), &counts, &info);

    Logger::err(str::format("[DeviceFault] VK_EXT_device_fault: ", result, ", description \"", info.description, "\", ",
      counts.addressInfoCount, " address info(s), ", counts.vendorInfoCount, " vendor info(s)"));
    for (uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
      const VkDeviceFaultAddressInfoEXT& a = addresses[i];
      const uint64_t precision = std::max<uint64_t>(1, a.addressPrecision);
      const uint64_t lo = a.reportedAddress & ~(precision - 1);
      Logger::err(str::format("[DeviceFault]   address #", i, ": ", faultAddressTypeName(a.addressType), " ",
        hex(a.reportedAddress), " (range [", hex(lo), ", ", hex(lo + precision), "))"));
      if (m_trackAddresses) {
        Logger::err(str::format("[DeviceFault]     ", describeAddress(a.reportedAddress, precision)));
      }
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i) {
      const VkDeviceFaultVendorInfoEXT& vendor = vendors[i];
      Logger::err(str::format("[DeviceFault]   vendor info #", i, ": \"", vendor.description, "\" code ",
        hex(vendor.vendorFaultCode), " data ", hex(vendor.vendorFaultData)));
    }
  }
}
