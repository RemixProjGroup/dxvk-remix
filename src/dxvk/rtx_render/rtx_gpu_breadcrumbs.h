#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rtx_option.h"
#include "../dxvk_include.h"
#include "../../util/thread.h"

namespace dxvk {
  class DxvkDevice;

  // Device-loss diagnostics without Aftermath. All log lines start with [DeviceFault].
  // With VK_AMD_buffer_marker, every GPU profiler zone writes its sequence number into a host-visible buffer,
  // at zone begin with TOP_OF_PIPE and at zone end with BOTTOM_OF_PIPE. A CPU-side ring keeps each zone's name.
  // On device loss the buffer is read back to list the zones the GPU began but never finished.
  // With VK_EXT_device_fault, the driver's fault description and faulting addresses are logged as well, and each
  // faulting address is matched against the device address ranges of the live buffers and of the buffers freed
  // recently, which DxvkBuffer reports here.
  // GPUs other than NVIDIA always get these. NVIDIA GPUs get them with rtx.deviceFaultDiagnostics.
  class GpuBreadcrumbs {
  public:
    RTX_OPTION_ENV("rtx", bool, deviceFaultDiagnostics, false, "DXVK_DEVICE_FAULT_DIAGNOSTICS",
                   "Turns the device-loss diagnostics on for NVIDIA GPUs as well (other GPUs always have them): "
                   "VK_AMD_buffer_marker pass markers and VK_EXT_device_fault. After a device loss, remix-dxvk.log gets "
                   "[DeviceFault] lines with the driver's fault description, every faulting GPU address, the buffer that "
                   "holds or recently held that address, and the pass the GPU was in. Takes effect after a restart. "
                   "Costs two marker writes per GPU profiler zone and one bookkeeping step per buffer created or freed. "
                   "Use it for a device loss that does not reproduce with Aftermath on.");

    // Ring size in zones. Must be a power of two.
    static constexpr uint32_t kSlots = 1u << 15;
    static constexpr uint32_t kNameLength = 48;
    static constexpr uint32_t kMaxDepth = 32;
    // How many freed buffer ranges are kept for the fault address match.
    static constexpr uint32_t kFreedRanges = 1u << 14;

    // Returns nullptr when the device has neither VK_AMD_buffer_marker nor VK_EXT_device_fault enabled.
    static std::unique_ptr<GpuBreadcrumbs> create(DxvkDevice* device);

    ~GpuBreadcrumbs();

    // Writes a zone begin marker into cmdBuffer and returns the sequence number for end().
    // Returns 0 when markers are unavailable.
    uint32_t begin(VkCommandBuffer cmdBuffer, const char* name);
    void end(VkCommandBuffer cmdBuffer, uint32_t seq);

    // True when buffer device address ranges are kept for the fault address match, which needs VK_EXT_device_fault.
    bool tracksAddresses() const {
      return m_trackAddresses;
    }

    // Records the device address range of a buffer that was just created, or of one about to be destroyed.
    // Thread safe. Both do nothing without tracksAddresses(), and for an address of 0.
    void noteBufferCreated(VkDeviceAddress address, VkDeviceSize size, const char* name);
    void noteBufferDestroyed(VkDeviceAddress address, VkDeviceSize size, const char* name);

    // Logs the device-loss report. Only the first call logs.
    void logDeviceLost(const char* reason);

  private:
    struct Entry {
      // Zero while the entry is empty or being written.
      std::atomic<uint32_t> seq { 0 };
      uint32_t frameId = 0;
      uint32_t parentSeq = 0;
      uint16_t depth = 0;
      char name[kNameLength] = {};
    };

    struct Snapshot {
      uint32_t seq;
      uint32_t frameId;
      uint32_t parentSeq;
      uint16_t depth;
      bool started;
      bool ended;
      char name[kNameLength];
    };

    // A buffer's device address range. The frame is the one the buffer was created in while it lives,
    // and the one it was freed in once the range sits in the freed ring.
    struct AddressRange {
      VkDeviceAddress begin = 0;
      VkDeviceSize size = 0;
      uint32_t frame = 0;
      char name[kNameLength] = {};
    };

    explicit GpuBreadcrumbs(DxvkDevice* device);
    bool initMarkers();
    void logMarkers();
    void logDeviceFault();
    // Names the live buffer that holds the address and the freed buffers that held it.
    // Names what the driver's imprecise range overlaps when no buffer holds the exact address.
    std::string describeAddress(VkDeviceAddress address, uint64_t precision);

    DxvkDevice*              m_device;
    Rc<vk::DeviceFn>         m_vkd;
    VkBuffer                 m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory           m_memory = VK_NULL_HANDLE;
    // Begin markers in [0, kSlots), end markers in [kSlots, 2 * kSlots).
    const volatile uint32_t* m_mapped = nullptr;
    std::atomic<uint32_t>    m_nextSeq { 1 };
    std::atomic<bool>        m_logged { false };
    std::unique_ptr<Entry[]> m_entries;

    bool                     m_trackAddresses = false;
    dxvk::mutex              m_addressMutex;
    // Live ranges keyed by their first address. The freed ring overwrites its oldest entry.
    std::map<VkDeviceAddress, AddressRange> m_liveRanges;
    std::vector<AddressRange> m_freedRanges;
    size_t                   m_freedCursor = 0;
  };
}
