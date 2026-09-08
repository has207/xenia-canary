/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/d3d12_shared_memory.h"

#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DECLARE_bool(gpu_allow_invalid_upload_range);
DECLARE_bool(enable_host_buffer);
DECLARE_bool(tiled_shared_memory);
DECLARE_bool(shared_memory_zero_copy);

namespace xe {
namespace gpu {
namespace d3d12 {

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

D3D12SharedMemory::~D3D12SharedMemory() { Shutdown(true); }

bool D3D12SharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Zero-copy: place buffer_ directly on a heap imported from guest RAM. On
  // success buffer_ aliases guest RAM, so the device-local buffer, its uploads
  // and the separate host_buffer_ are all unnecessary and skipped below.
  const bool zero_copy =
      cvars::shared_memory_zero_copy && TryInitializeZeroCopy();

  if (!zero_copy) {
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(
        buffer_desc, kBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    if (cvars::tiled_shared_memory &&
        provider.GetTiledResourcesTier() !=
            D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
        !provider.GetGraphicsAnalysis()) {
      if (FAILED(device->CreateReservedResource(
              &buffer_desc, buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
        XELOGE("Shared memory: Failed to create the {} MB tiled buffer",
               kBufferSize >> 20);
        Shutdown();
        return false;
      }
      static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
      InitializeSparseHostGpuMemory(
          std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
    } else {
      XELOGGPU(
          "Direct3D 12 tiled resources are not used for shared memory "
          "emulation - video memory usage may increase significantly "
          "because a full {} MB buffer will be created",
          kBufferSize >> 20);
      if (provider.GetGraphicsAnalysis()) {
        // As of October 8th, 2018, PIX doesn't support tiled buffers.
        // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
        XELOGGPU(
            "This is caused by PIX being attached, which doesn't support tiled "
            "resources yet.");
      }
      if (FAILED(device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesDefault,
              provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
              buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
        XELOGE("Shared memory: Failed to create the {} MB buffer",
               kBufferSize >> 20);
        Shutdown();
        return false;
      }
    }
    buffer_->SetName(L"Shared Memory Buffer");
    buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
    buffer_uav_writes_commit_needed_ = false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors =
      uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(
          &buffer_descriptor_heap_desc,
          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    XELOGE(
        "Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ =
      buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, xe::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                          size_t(1) << page_size_log2()));

  if (!zero_copy) {
    TryInitializeHostBuffer();
  }

  return true;
}

bool D3D12SharedMemory::ImportGuestRamHeap(void*& out_view,
                                           ID3D12Heap*& out_heap) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // OpenExistingHeapFromAddress is on ID3D12Device3.
  ID3D12Device3* device3 = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) {
    XELOGI("Shared memory host import: ID3D12Device3 not available");
    return false;
  }

  D3D12_FEATURE_DATA_EXISTING_HEAPS existing_heaps = {};
  if (FAILED(device3->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS,
                                          &existing_heaps,
                                          sizeof(existing_heaps))) ||
      !existing_heaps.Supported) {
    XELOGI("Shared memory host import: existing heaps not supported");
    device3->Release();
    return false;
  }

  // Map a dedicated view of guest physical RAM for the heap import. Importing
  // the memory-managed view (TranslatePhysical(0)) makes D3D12 own those pages
  // and Xenia's write-watch VirtualProtect on the same view then fails. A
  // separate view of the same file-mapping pages keeps them coherent (shared
  // backing) while isolating protection (per-view PTEs).
  const size_t physical_offset =
      size_t(memory().physical_membase() - memory().virtual_membase());
  void* view = xe::memory::MapFileView(
      memory().mapping_handle(), nullptr, kBufferSize,
      xe::memory::PageAccess::kReadWrite, physical_offset);
  if (view == nullptr) {
    XELOGI(
        "Shared memory host import: failed to map a dedicated guest RAM view");
    device3->Release();
    return false;
  }

  // Import the 512 MB view as a heap. Its size comes from the enclosing OS
  // allocation (this fresh view is exactly the buffer), so the buffer is placed
  // at offset 0.
  ID3D12Heap* heap = nullptr;
  HRESULT hr = device3->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap));
  device3->Release();
  if (FAILED(hr)) {
    XELOGI(
        "Shared memory host import: OpenExistingHeapFromAddress failed "
        "(0x{:08X})",
        static_cast<uint32_t>(hr));
    xe::memory::UnmapFileView(memory().mapping_handle(), view, kBufferSize);
    return false;
  }

  // Log the imported heap's coherency properties. CPUPageProperty WRITE_BACK
  // (3) is cache-coherent, WRITE_COMBINE (2) is not for GPU-write readback.
  // This is driver-chosen for the diagnostic heap, not app-selectable.
  const D3D12_HEAP_DESC heap_desc = heap->GetDesc();
  XELOGI(
      "Shared memory host import: heap CPUPageProperty={} MemoryPool={} "
      "flags=0x{:X}",
      uint32_t(heap_desc.Properties.CPUPageProperty),
      uint32_t(heap_desc.Properties.MemoryPoolPreference),
      uint32_t(heap_desc.Flags));

  out_view = view;
  out_heap = heap;
  return true;
}

void D3D12SharedMemory::TryInitializeHostBuffer() {
  if (!cvars::enable_host_buffer) {
    return;
  }
  void* view = nullptr;
  ID3D12Heap* heap = nullptr;
  if (!ImportGuestRamHeap(view, heap)) {
    // Without it memexport and resolve output stays device-local, so both fall
    // back to reading back through a staging buffer.
    XELOGW(
        "Shared memory: guest RAM import failed - memexport and resolve "
        "readback fall back to staging copies");
    return;
  }

  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // OpenExistingHeapFromAddress returns a SHARED_CROSS_ADAPTER heap, so the
  // buffer needs ALLOW_CROSS_ADAPTER. Memexport writes to it as a UAV.
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, kBufferSize,
      D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  host_buffer_state_ = D3D12_RESOURCE_STATE_COMMON;
  HRESULT hr =
      device->CreatePlacedResource(heap, 0, &buffer_desc, host_buffer_state_,
                                   nullptr, IID_PPV_ARGS(&host_buffer_));
  if (FAILED(hr)) {
    XELOGI("Shared memory host import: UAV buffer placement failed (0x{:08X})",
           static_cast<uint32_t>(hr));
    host_buffer_ = nullptr;
    heap->Release();
    xe::memory::UnmapFileView(memory().mapping_handle(), view, kBufferSize);
    return;
  }
  host_buffer_->SetName(L"Shared Memory Host Buffer");
  host_buffer_heap_ = heap;
  host_buffer_view_ = view;
  host_buffer_gpu_address_ = host_buffer_->GetGPUVirtualAddress();
  XELOGI("Shared memory: guest RAM host buffer ready");
}

bool D3D12SharedMemory::TryInitializeZeroCopy() {
  void* view = nullptr;
  ID3D12Heap* heap = nullptr;
  if (!ImportGuestRamHeap(view, heap)) {
    return false;
  }

  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // The imported heap is SHARED_CROSS_ADAPTER, so buffer_ needs
  // ALLOW_CROSS_ADAPTER. As the primary buffer it is read as index, vertex and
  // texture data and written by memexport as a UAV.
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, kBufferSize,
      D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COMMON;
  HRESULT hr = device->CreatePlacedResource(
      heap, 0, &buffer_desc, buffer_state_, nullptr, IID_PPV_ARGS(&buffer_));
  if (FAILED(hr)) {
    XELOGI("Shared memory zero-copy: buffer placement failed (0x{:08X})",
           static_cast<uint32_t>(hr));
    buffer_ = nullptr;
    heap->Release();
    xe::memory::UnmapFileView(memory().mapping_handle(), view, kBufferSize);
    return false;
  }
  buffer_->SetName(L"Shared Memory Buffer (zero-copy)");
  host_buffer_heap_ = heap;
  host_buffer_view_ = view;
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;
  zero_copy_ = true;
  XELOGI("Shared memory: using zero-copy guest RAM aliasing");
  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  ResetTraceDownload();

  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // Free both buffers before the imported heap that backs them, and unmap the
  // view backing that heap only after every resource on it is gone. In
  // zero-copy mode buffer_ is placed on host_buffer_heap_ too, so it must be
  // released first.
  ui::d3d12::util::ReleaseAndNull(buffer_);
  ui::d3d12::util::ReleaseAndNull(host_buffer_);
  ui::d3d12::util::ReleaseAndNull(host_buffer_heap_);
  host_buffer_gpu_address_ = 0;
  zero_copy_ = false;
  if (host_buffer_view_ != nullptr) {
    xe::memory::UnmapFileView(memory().mapping_handle(), host_buffer_view_,
                              kBufferSize);
    host_buffer_view_ = nullptr;
  }

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
  host_buffer_uav_writes_commit_needed_ = false;
  // The host buffer is used only on memexport frames, so unlike buffer_ it can
  // sit idle across submissions. Buffers decay to COMMON at each
  // ExecuteCommandLists, so track it from COMMON every submission to avoid a
  // stale before-state on the next transition.
  host_buffer_state_ = D3D12_RESOURCE_STATE_COMMON;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(
    D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
        buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitHostUAVWritesAndTransitionBuffer(
    D3D12_RESOURCE_STATES new_state) {
  if (host_buffer_ == nullptr) {
    return;
  }
  if (host_buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
        host_buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(host_buffer_);
      host_buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(host_buffer_, host_buffer_state_,
                                           new_state);
  host_buffer_state_ = new_state;
  host_buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    return false;
  }
  D3D12_RESOURCE_DESC download_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      download_buffer_desc, download_page_count << page_size_log2(),
      D3D12_RESOURCE_FLAG_NONE);
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &download_buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&trace_download_buffer_)))) {
    XELOGE(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }
  auto& command_list = command_processor_.GetDeferredCommandList();
  UseAsCopySource();
  command_processor_.SubmitBarriers();
  command_processor_.InsertDebugMarker(
      "Trace Download: %u KB, %zu ranges",
      download_page_count << page_size_log2() >> 10,
      trace_download_ranges().size());
  uint32_t download_buffer_offset = 0;
  for (const auto& download_range : trace_download_ranges()) {
    command_list.D3DCopyBufferRegion(
        trace_download_buffer_, download_buffer_offset, buffer_,
        download_range.first, download_range.second);
    download_buffer_offset += download_range.second;
  }
  return true;
}

void D3D12SharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_buffer_) {
    return;
  }
  void* download_mapping;
  if (SUCCEEDED(trace_download_buffer_->Map(0, nullptr, &download_mapping))) {
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(
          download_range.first, download_range.second,
          reinterpret_cast<const uint8_t*>(download_mapping) +
              download_buffer_offset);
      download_buffer_offset += download_range.second;
    }
    D3D12_RANGE download_write_range = {};
    trace_download_buffer_->Unmap(0, &download_write_range);
  } else {
    XELOGE(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
}

void D3D12SharedMemory::ResetTraceDownload() {
  ui::d3d12::util::ReleaseAndNull(trace_download_buffer_);
  ReleaseTraceDownloadRanges();
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(
    uint32_t offset_allocations, uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations
                          << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations
                          << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS |
                    provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    XELOGE("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X =
      offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = false;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  direct_queue->UpdateTileMappings(
      buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
      &range_flags, &heap_range_start_offset, &region_size.NumTiles,
      D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_page_ranges) {
  SCOPE_profile_cpu_f("gpu");
  if (!num_upload_page_ranges) {
    return true;
  }
  if (zero_copy_) {
    // buffer_ aliases guest RAM - nothing to copy, just mark the pages valid so
    // RequestRange stops asking to upload them.
    for (uint32_t i = 0; i < num_upload_page_ranges; ++i) {
      trace_writer_.WriteMemoryRead(
          upload_page_ranges[i].first << page_size_log2(),
          upload_page_ranges[i].second << page_size_log2());
      MakeRangeValid(upload_page_ranges[i].first << page_size_log2(),
                     upload_page_ranges[i].second << page_size_log2(), false);
    }
    return true;
  }
  // Ranges holding memexport output live in the host buffer (guest RAM) and may
  // still be being written by the GPU, so reading them with the CPU below races
  // those writes. Refresh those on the GPU and upload only what is left.
  cpu_upload_ranges_.clear();
  for (uint32_t i = 0; i < num_upload_page_ranges; ++i) {
    uint32_t range_base = upload_page_ranges[i].first << page_size_log2();
    uint32_t range_size = upload_page_ranges[i].second << page_size_log2();
    if (command_processor_.EnsureMemexportRangeInDeviceBuffer(range_base,
                                                              range_size)) {
      MakeRangeValid(range_base, range_size, false);
      continue;
    }
    cpu_upload_ranges_.push_back(upload_page_ranges[i]);
  }
  if (cpu_upload_ranges_.empty()) {
    return true;
  }
  upload_page_ranges = cpu_upload_ranges_.data();
  num_upload_page_ranges = uint32_t(cpu_upload_ranges_.size());

  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& upload_range_front = upload_page_ranges[0];
  auto& upload_range_back = upload_page_ranges[num_upload_page_ranges - 1];
  uint32_t total_upload_bytes =
      (upload_range_back.first + upload_range_back.second -
       upload_range_front.first)
      << page_size_log2();
  command_processor_.PushDebugMarker(
      "UploadRanges (SharedMem): 0x%08X-0x%08X (%u KB, %u ranges)",
      upload_range_front.first << page_size_log2(),
      (upload_range_back.first + upload_range_back.second) << page_size_log2(),
      total_upload_bytes >> 10, num_upload_page_ranges);
  auto& command_list = command_processor_.GetDeferredCommandList();
  for (uint32_t i = 0; i < num_upload_page_ranges; ++i) {
    auto& upload_range = upload_page_ranges[i];
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());

    if (upload_range_length > 0 && !cvars::gpu_allow_invalid_upload_range) {
      // Check both start and end of the range for unmapped memory.
      const uint32_t range_start_addr = upload_range_start << page_size_log2();
      const uint32_t upload_range_last_page =
          upload_range_start + upload_range_length - 1;
      const uint32_t range_end_addr = upload_range_last_page
                                      << page_size_log2();

      const memory::PageAccess start_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_start_addr,
                                                       range_start_addr);
      const memory::PageAccess end_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_end_addr,
                                                       range_end_addr);

      if (start_access == xe::memory::PageAccess::kNoAccess ||
          end_access == xe::memory::PageAccess::kNoAccess) {
        XELOGE("Invalid upload range for GPU: {:08X} length {:08X}",
               upload_range_start, upload_range_length);
        return false;
      }
    }

    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(),
          upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset,
          &upload_buffer_size, nullptr);
      if (upload_buffer_mapping == nullptr) {
        XELOGE("Shared memory: Failed to get an upload buffer");
        return false;
      }
      MakeRangeValid(upload_range_start << page_size_log2(),
                     uint32_t(upload_buffer_size), false);

      if (upload_buffer_size < (1ULL << 32) && upload_buffer_size > 8192) {
        memory::vastcpy(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            static_cast<uint32_t>(upload_buffer_size));
        swcache::WriteFence();
      } else {
        std::memcpy(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            upload_buffer_size);
      }
      command_list.D3DCopyBufferRegion(
          buffer_, upload_range_start << page_size_log2(), upload_buffer,
          UINT64(upload_buffer_offset), UINT64(upload_buffer_size));
      uint32_t upload_buffer_pages =
          uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
  }
  command_processor_.PopDebugMarker();
  return true;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
