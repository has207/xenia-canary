/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Two-buffer memexport page tracking, shared by the D3D12 and Vulkan command
// processors. This is #included into each backend's class body (like the pm4
// command processor sections) so each backend gets its own inlinable copy with
// no inheritance or virtual dispatch. The include site must already provide
// SharedMemory, Shader, RegisterFile, xenos, xe::bit_scan_forward and <array>.
// No include guard: this is injected into a class body, not a standalone
// header.
//
// It must also define, before including this:
//   void FlushMemexportStagingReadback()
//     Copies export output the backend staged for readback into guest RAM, now
//     that the wait has made it available. Empty where output already lands
//     there, which is zero-copy and the host buffer.

// Records the guest pages any memexport draw wrote so geometry draws consuming
// that output route to the host-imported buffer. Marking rounds outward.
void MarkMemexportPagesWritten(uint32_t base_bytes, uint32_t size_bytes) {
  if (!size_bytes || base_bytes >= SharedMemory::kBufferSize) {
    return;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  uint32_t first_page = base_bytes >> 12;
  uint32_t last_page = (base_bytes + size_bytes - 1) >> 12;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    memexport_written_pages_[page >> 6] |= uint64_t(1) << (page & 63);
  }
  any_memexport_pages_written_ = true;
  memexport_await_pending_ = true;
  memexport_written_page_min_ =
      std::min(memexport_written_page_min_, first_page);
  memexport_written_page_max_ =
      std::max(memexport_written_page_max_, last_page);
}

// Waits for export output recorded since the last wait to reach guest RAM, so a
// fence or interrupt the guest is about to observe means the data is there.
// Ring buffer read-pointer writeback is deliberately not covered: it reports
// parse progress rather than execution, but fires per batch, so stalling there
// would serialize the command processor against the GPU.
void AwaitMemexportForFence() {
  if (!memexport_await_pending_) {
    return;
  }
  memexport_await_pending_ = false;
  // Flushes the open submission before waiting, so a draw that is still only
  // recorded gets submitted rather than skipped.
  AwaitAllQueueOperationsCompletion();
  FlushMemexportStagingReadback();
}

// Same, for the guest asking the GPU to flush a range it is about to read
// (COHER_BASE_HOST/COHER_SIZE_HOST). Gated on the range actually holding export
// output, unlike the fence path which cannot know what the fence refers to.
void AwaitMemexportForCoherency(uint32_t base_bytes, uint32_t size_bytes) {
  if (!memexport_await_pending_ || !IsMemexportRange(base_bytes, size_bytes)) {
    return;
  }
  memexport_await_pending_ = false;
  AwaitAllQueueOperationsCompletion();
  FlushMemexportStagingReadback();
}

// Clears memexport marks for a range a resolve just wrote, so its device-buffer
// output is no longer treated as memexport (host buffer) data. Clearing rounds
// inward: a page straddling the edge holds both this range's data and
// neighbouring memexport output, so it keeps its mark. The window is left
// as-is, it only needs to bound the set bits.
void ClearMemexportPages(uint32_t base_bytes, uint32_t size_bytes) {
  if (!any_memexport_pages_written_ || !size_bytes ||
      base_bytes >= SharedMemory::kBufferSize) {
    return;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  uint32_t first_page = (base_bytes + 0xFFF) >> 12;
  uint32_t end_page = (base_bytes + size_bytes) >> 12;
  for (uint32_t page = first_page; page < end_page; ++page) {
    memexport_written_pages_[page >> 6] &= ~(uint64_t(1) << (page & 63));
  }
}

bool IsMemexportRange(uint32_t base_bytes, uint32_t size_bytes) const {
  if (!any_memexport_pages_written_ || !size_bytes ||
      base_bytes >= SharedMemory::kBufferSize) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  // Clamp the scan to the written memexport window so a range entirely outside
  // it (most textures and geometry) scans nothing.
  uint32_t first_page = std::max(base_bytes >> 12, memexport_written_page_min_);
  uint32_t last_page = std::min((base_bytes + size_bytes - 1) >> 12,
                                memexport_written_page_max_);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (memexport_written_pages_[page >> 6] & (uint64_t(1) << (page & 63))) {
      return true;
    }
  }
  return false;
}

bool VertexFetchInMemexportRange(const RegisterFile& regs,
                                 const Shader& vertex_shader) const {
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(vfetch_bits, &j)) {
      vfetch_bits = xe::clear_lowest_bit(vfetch_bits);
      xenos::xe_gpu_vertex_fetch_t vfetch_constant =
          regs.GetVertexFetch(i * 32 + j);
      if (vfetch_constant.type != xenos::FetchConstantType::kVertex) {
        continue;
      }
      if (IsMemexportRange(vfetch_constant.address << 2,
                           vfetch_constant.size << 2)) {
        return true;
      }
    }
  }
  return false;
}

// Resets all tracking, called from each backend's ShutdownContext.
void ResetMemexportPages() {
  memexport_written_pages_.fill(0);
  any_memexport_pages_written_ = false;
  memexport_await_pending_ = false;
  memexport_written_page_min_ = kMemexportPageCount;
  memexport_written_page_max_ = 0;
}

// Guest pages (4 KB) any memexport draw has written, at host buffer offsets.
static constexpr uint32_t kMemexportPageCount = SharedMemory::kBufferSize >> 12;
std::array<uint64_t, kMemexportPageCount / 64> memexport_written_pages_{};
// Cleared until the first memexport draw, so consumer-side routing is skipped
// entirely for titles that never memexport.
bool any_memexport_pages_written_ = false;
// Export output has been recorded but not yet awaited for the guest to read.
bool memexport_await_pending_ = false;
// Inclusive min/max written page, so IsMemexportRange rejects a range outside
// the memexport region in O(1). Empty (min > max) until the first memexport.
uint32_t memexport_written_page_min_ = kMemexportPageCount;
uint32_t memexport_written_page_max_ = 0;
