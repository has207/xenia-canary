/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_D3D12_SHARED_MEMORY_H_
#define XENIA_GPU_D3D12_D3D12_SHARED_MEMORY_H_

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"
#include "xenia/ui/d3d12/d3d12_api.h"
#include "xenia/ui/d3d12/d3d12_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12CommandProcessor;

class D3D12SharedMemory : public SharedMemory {
 public:
  D3D12SharedMemory(D3D12CommandProcessor& command_processor, Memory& memory,
                    TraceWriter& trace_writer);
  ~D3D12SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  ID3D12Resource* GetBuffer() const { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const {
    return buffer_gpu_address_;
  }

  // A second buffer placed on a heap imported from guest RAM
  // (OpenExistingHeapFromAddress), or null if unavailable. Bound instead of
  // buffer() for memexport-touching draws, and the destination of resolve
  // readback, so their output is coherent with the CPU (no clobber, since it
  // aliases guest RAM). Mirrors the Vulkan host_buffer().
  ID3D12Resource* GetHostBuffer() const { return host_buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS GetHostGPUAddress() const {
    return host_buffer_gpu_address_;
  }

  // True when buffer_ aliases guest RAM directly (zero-copy import). Uploads
  // are then unnecessary and host_buffer_ is null, since buffer_ already is
  // guest RAM.
  bool is_zero_copy() const { return zero_copy_; }

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  // RequestRange may transition the buffer to copy destination - call it before
  // UseForReading or UseForWriting.

  // Makes the buffer usable for vertices, indices and texture untiling.
  void UseForReading() {
    // Vertex fetch is also allowed in pixel shaders.
    CommitUAVWritesAndTransitionBuffer(
        D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  // Makes the buffer usable for texture tiling after a resolve.
  void UseForWriting() {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  // Makes the buffer usable as a source for copy commands.
  void UseAsCopySource() {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  // Makes the buffer usable as a destination for copy commands (used to copy
  // memexport output back from the host buffer for texture loads).
  void UseAsCopyDestination() {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  }
  // Must be called when doing draws/dispatches modifying data within the shared
  // memory buffer as a UAV, to make sure that when UseForWriting is called the
  // next time, a UAV barrier will be done, and subsequent overlapping UAV
  // writes and reads are ordered.
  void MarkUAVWritesCommitNeeded() {
    if (buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      buffer_uav_writes_commit_needed_ = true;
    }
  }

  // Same as the UseFor* methods above, for the host buffer (guest RAM), used by
  // memexport-routed draws. No-ops when the host buffer is unavailable.
  void UseHostForReading() {
    CommitHostUAVWritesAndTransitionBuffer(
        D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  void UseHostForWriting() {
    CommitHostUAVWritesAndTransitionBuffer(
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  void UseHostAsCopySource() {
    CommitHostUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  void UseHostAsCopyDestination() {
    CommitHostUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  }
  void MarkHostUAVWritesCommitNeeded() {
    if (host_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      host_buffer_uav_writes_commit_needed_ = true;
    }
  }

  void WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);

  // Returns true if any downloads were submitted to the command processor.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_ranges) override;

 private:
  D3D12CommandProcessor& command_processor_;
  TraceWriter& trace_writer_;

  // The 512 MB tiled buffer.
  ID3D12Resource* buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS buffer_gpu_address_ = 0;
  std::vector<ID3D12Heap*> buffer_tiled_heaps_;
  D3D12_RESOURCE_STATES buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  bool buffer_uav_writes_commit_needed_ = false;
  void CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state);

  // True when buffer_ is placed directly on a heap imported from guest RAM, so
  // it aliases guest RAM and needs no per-frame uploads. host_buffer_ is then
  // not created. The import is tracked by host_buffer_view_/host_buffer_heap_.
  bool zero_copy_ = false;

  // A dedicated file-mapping view of guest RAM backing the imported heap, kept
  // separate from the memory-managed views so the write-watch VirtualProtect
  // never touches D3D12-owned pages. Unmapped after the heap is released. Backs
  // buffer_ in zero-copy mode, host_buffer_ otherwise.
  void* host_buffer_view_ = nullptr;
  ID3D12Heap* host_buffer_heap_ = nullptr;
  // Second buffer aliasing guest RAM, bound for memexport-touching draws. Null
  // when guest RAM can't be imported as a GPU heap (feature unsupported, or the
  // adapter rejects a UAV buffer on a CPU-visible heap).
  ID3D12Resource* host_buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS host_buffer_gpu_address_ = 0;
  D3D12_RESOURCE_STATES host_buffer_state_ = D3D12_RESOURCE_STATE_COMMON;
  bool host_buffer_uav_writes_commit_needed_ = false;
  void CommitHostUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state);
  // Imports guest RAM as host_buffer_. No-op when enable_host_buffer is off or
  // on failure (host_buffer_ stays null and memexport and resolve readback go
  // through staging copies instead).
  void TryInitializeHostBuffer();
  // Maps a dedicated guest RAM view and opens it as a D3D12 heap
  // (OpenExistingHeapFromAddress). Returns true with out_view/out_heap set;
  // false with nothing to clean up.
  bool ImportGuestRamHeap(void*& out_view, ID3D12Heap*& out_heap);
  // Places buffer_ directly on a heap imported from guest RAM (full zero-copy).
  // Returns true (buffer_/zero_copy_ set) or false to use the device-local
  // path.
  bool TryInitializeZeroCopy();

  // Non-shader-visible buffer descriptor heap for faster binding (via copying
  // rather than creation).
  enum class BufferDescriptorIndex : uint32_t {
    kRawSRV,
    kRawUAV,

    kCount,
  };
  ID3D12DescriptorHeap* buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE buffer_descriptor_heap_start_;

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> upload_buffer_pool_;
  // Upload ranges left after the memexport ones are refreshed on the GPU.
  std::vector<std::pair<uint32_t, uint32_t>> cpu_upload_ranges_;

  // Created temporarily, only for downloading.
  ID3D12Resource* trace_download_buffer_ = nullptr;
  void ResetTraceDownload();
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_D3D12_SHARED_MEMORY_H_
