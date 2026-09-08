/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Staging buffers for reading GPU output back into guest RAM, shared by the
// D3D12 and Vulkan command processors. Like command_processor_memexport.inc,
// this is #included into each backend's class body so each gets its own
// inlinable copy with no inheritance or virtual dispatch.
//
// The fallback for hosts without the guest RAM host buffer (see the
// enable_host_buffer cvar): output stays in the device-local buffer and reaches
// guest RAM by a GPU copy into a host-visible buffer and a CPU copy out. Each
// key gets two of them, so the caller's `deferred` can take the previous copy
// and wait only on the older submission that filled it. Memexport and resolve
// both stage here and their extents can coincide, so export keys are tagged.
//
// The include site must already provide memory_ (Memory*), frame_current_,
// GetCurrentSubmission, AlignReadbackBufferSize, xenia/base/assert.h,
// xenia/base/logging.h, xenia/base/memory.h and <cstdint> <cstring>
// <unordered_map>. It must also define, before including this:
//   struct ReadbackStagingBuffer
//     One host-visible buffer, persistently mapped as `void* mapped`.
//   bool CreateReadbackStagingBuffer(ReadbackStagingBuffer&, uint32_t size)
//   void DestroyReadbackStagingBuffer(ReadbackStagingBuffer&)
//   void PrepareReadbackStagingEviction()
//     Called once before an eviction's first destroy, for backends that drain
//     rather than defer.
//   bool AwaitReadbackStagingSubmission(uint64_t submission)
//     Waits for one submission, or for everything recorded so far on
//     UINT64_MAX. False if it could not.
//   void InvalidateReadbackStaging(const ReadbackStagingBuffer&)
//     Makes a completed GPU write visible to the CPU. Empty where the readback
//     memory is always host-coherent.
// No include guard: injected into a class body.

// Keeps an export range off the slot of a resolve destination with the same
// extent. Guest addresses are under 512 MB, so the key's top bits are free.
static constexpr uint64_t kReadbackStagingMemexportTag = uint64_t(1) << 63;

// Age eviction alone only runs at frame close, so a title that never swaps
// would grow without bound.
static constexpr uint64_t kMaxReadbackStagingBytes = 64 * 1024 * 1024;

struct ReadbackStagingSlot {
  ReadbackStagingBuffer buffers[2];
  // 0 until that buffer is created. A key carries its own length, so a buffer
  // is sized once and never resized - a copy recorded into one is never left
  // pointing at a freed buffer.
  uint32_t sizes[2] = {0, 0};
  // The buffer the next copy is recorded into.
  uint32_t write_index = 0;
  // Submission each buffer's last copy was recorded in, 0 for never written.
  uint64_t submissions[2] = {0, 0};
  uint64_t last_used_frame = 0;
};

// Returns the slot for this key with the buffer the next copy goes into ready,
// or null if that buffer could not be created.
ReadbackStagingSlot* AcquireReadbackStagingSlot(uint64_t key, uint32_t length) {
  if (!length) {
    return nullptr;
  }
  ReadbackStagingSlot& slot = readback_staging_slots_[key];
  uint32_t index = slot.write_index;
  if (slot.sizes[index]) {
    assert_true(slot.sizes[index] >= length);
    slot.last_used_frame = frame_current_;
    return &slot;
  }
  uint32_t size = AlignReadbackBufferSize(length);
  EvictReadbackStagingToFit(size, key);
  if (!CreateReadbackStagingBuffer(slot.buffers[index], size)) {
    if (!slot.sizes[index ^ 1]) {
      readback_staging_slots_.erase(key);
    }
    // Without touching last_used_frame, so a slot left holding one buffer
    // ages out instead of retrying forever.
    return nullptr;
  }
  slot.sizes[index] = size;
  readback_staging_bytes_ += size;
  slot.last_used_frame = frame_current_;
  return &slot;
}

// Frees least recently used slots until size more bytes fit. Skips keep_key
// and anything used this frame, which may hold a copy waiting to be read out.
void EvictReadbackStagingToFit(uint32_t size, uint64_t keep_key) {
  bool eviction_prepared = false;
  while (readback_staging_bytes_ + size > kMaxReadbackStagingBytes) {
    auto oldest = readback_staging_slots_.end();
    for (auto it = readback_staging_slots_.begin();
         it != readback_staging_slots_.end(); ++it) {
      if (it->first == keep_key ||
          it->second.last_used_frame == frame_current_ ||
          !(it->second.sizes[0] | it->second.sizes[1])) {
        continue;
      }
      if (oldest == readback_staging_slots_.end() ||
          it->second.last_used_frame < oldest->second.last_used_frame) {
        oldest = it;
      }
    }
    if (oldest == readback_staging_slots_.end()) {
      // All in use this frame - go over budget rather than drop a live copy.
      return;
    }
    if (!eviction_prepared) {
      PrepareReadbackStagingEviction();
      eviction_prepared = true;
    }
    DestroyReadbackStagingSlot(oldest->second);
    readback_staging_slots_.erase(oldest);
  }
}

// The buffer a copy should be recorded into.
ReadbackStagingBuffer& ReadbackStagingWriteBuffer(ReadbackStagingSlot& slot) {
  return slot.buffers[slot.write_index];
}

// The slot held for this key, or null if there is none.
ReadbackStagingSlot* FindReadbackStagingSlot(uint64_t key) {
  auto slot = readback_staging_slots_.find(key);
  return slot != readback_staging_slots_.end() ? &slot->second : nullptr;
}

// Copies a recorded staging read out to guest RAM, waiting first. deferred
// takes the previous copy instead, waiting only on the older submission that
// filled it. The first use of a key has no previous copy and waits either way.
// Call once per recorded copy, since it also rotates the slot.
bool FinishReadbackStagingToGuestRam(ReadbackStagingSlot& slot,
                                     uint32_t address, uint32_t length,
                                     bool deferred) {
  uint32_t read_index = slot.write_index;
  uint64_t await_submission = UINT64_MAX;
  if (deferred && slot.submissions[read_index ^ 1]) {
    read_index ^= 1;
    await_submission = slot.submissions[read_index];
  }
  // Before the wait, which closes the submission the copy was recorded into.
  slot.submissions[slot.write_index] = GetCurrentSubmission();
  slot.write_index ^= 1;
  if (!AwaitReadbackStagingSubmission(await_submission)) {
    XELOGE("Readback staging: dropped the copy out of 0x{:08X} ({} bytes)",
           address, length);
    return false;
  }
  InvalidateReadbackStaging(slot.buffers[read_index]);
  ReadbackStagingToGuestRam(slot.buffers[read_index], address, length);
  return true;
}

// Copies length bytes out of a staging buffer into guest RAM at address. The
// GPU copy into the staging buffer must already have completed.
void ReadbackStagingToGuestRam(const ReadbackStagingBuffer& buffer,
                               uint32_t address, uint32_t length) {
  if (buffer.mapped == nullptr || !length) {
    return;
  }
  uint8_t* dest = memory_->TranslatePhysical(address);
  uint8_t* source = static_cast<uint8_t*>(buffer.mapped);
  // vastcpy's non-temporal stores need both pointers and the length to be
  // cache-line aligned.
  uintptr_t unaligned = reinterpret_cast<uintptr_t>(dest) |
                        reinterpret_cast<uintptr_t>(source) | uintptr_t(length);
  if (unaligned & 63) {
    std::memcpy(dest, source, length);
  } else {
    memory::vastcpy(dest, source, length);
  }
}

// Releases staging buffers untouched for kReadbackBufferEvictionAgeFrames.
// Called once per frame.
void EvictOldReadbackStaging() {
  if (frame_current_ <= kReadbackBufferEvictionAgeFrames) {
    return;
  }
  uint64_t oldest_kept = frame_current_ - kReadbackBufferEvictionAgeFrames;
  bool eviction_prepared = false;
  for (auto it = readback_staging_slots_.begin();
       it != readback_staging_slots_.end();) {
    if (it->second.last_used_frame >= oldest_kept) {
      ++it;
      continue;
    }
    if (!eviction_prepared) {
      PrepareReadbackStagingEviction();
      eviction_prepared = true;
    }
    DestroyReadbackStagingSlot(it->second);
    it = readback_staging_slots_.erase(it);
  }
}

// Only safe where teardown is, the backend hook does not defer past shutdown.
void ClearReadbackStagingBuffers() {
  for (auto& slot : readback_staging_slots_) {
    DestroyReadbackStagingSlot(slot.second);
  }
  readback_staging_slots_.clear();
  readback_staging_bytes_ = 0;
}

void DestroyReadbackStagingSlot(ReadbackStagingSlot& slot) {
  for (uint32_t i = 0; i < 2; ++i) {
    if (slot.sizes[i]) {
      DestroyReadbackStagingBuffer(slot.buffers[i]);
      readback_staging_bytes_ -= slot.sizes[i];
      slot.sizes[i] = 0;
    }
  }
}

// One pair of buffers per destination extent, keyed by MakeReadbackResolveKey
// plus kReadbackStagingMemexportTag for export ranges.
std::unordered_map<uint64_t, ReadbackStagingSlot> readback_staging_slots_;
uint64_t readback_staging_bytes_ = 0;
