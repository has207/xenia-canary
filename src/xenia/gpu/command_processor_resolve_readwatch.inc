/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Resolve-to-guest-RAM copy decisions and read-watch consumption tracking,
// shared by the D3D12 and Vulkan command processors. Like
// command_processor_memexport.inc, this is #included into each backend's class
// body so each gets its own inlinable copy with no inheritance or virtual
// dispatch. The include site must already provide SharedMemory, the base
// memory_ (Memory*), texture_cache_, xenia/memory.h (VirtualHeap,
// IsWritableProtect), xenia/base/assert.h, xenia/base/logging.h,
// xenia/gpu/draw_util.h, xenia/gpu/registers.h, and <atomic> <deque> <memory>
// <unordered_map> <unordered_set> <algorithm>. No include guard: injected into
// a class body.
//
// It must also define, before including this:
//   struct ResolveHoldSnapshotBuffer
//     Whatever the backend needs to hold one downscaled capture.
//   bool CreateResolveHoldSnapshotBuffer(ResolveHoldSnapshotBuffer&, uint32_t)
//   void DestroyResolveHoldSnapshotBuffer(ResolveHoldSnapshotBuffer&)
//   void PrepareResolveHoldSnapshotEviction()
//     Called once before the first destroy of an acquire, for backends that
//     have to drain rather than defer.
//   void FlushResolveRangeToGuestRam(uint32_t, uint32_t, bool from_snapshot)

// How much of a guest range is committed and writable, so copying GPU output
// back into it is safe. Writing past that corrupts memory the guest never
// committed.
uint32_t WritableGuestRangeLength(uint32_t address, uint32_t length) const {
  VirtualHeap* physical_heap = memory_->GetPhysicalHeap();
  if (!physical_heap || !length) {
    return 0;
  }
  HeapAllocationInfo alloc_info;
  if (!physical_heap->QueryRegionInfo(address, &alloc_info) ||
      !(alloc_info.state & kMemoryAllocationCommit) ||
      !IsWritableProtect(alloc_info.protect)) {
    return 0;
  }
  uint64_t region_end =
      uint64_t(alloc_info.base_address) + alloc_info.region_size;
  if (uint64_t(address) >= region_end) {
    return 0;
  }
  return uint32_t(std::min(uint64_t(length), region_end - uint64_t(address)));
}

// Whether a resolve's whole destination is writable - it is copied back whole
// or not at all. An empty range still has to name writable memory.
bool IsResolveDestinationResident(uint32_t address, uint32_t length) const {
  return length ? WritableGuestRangeLength(address, length) == length
                : WritableGuestRangeLength(address, 1) != 0;
}

// Marks the guest pages the CPU has read since a resolve wrote them, from the
// memory read callback on the guest thread. Read on the GPU thread, so atomic.
void MarkResolvePagesRead(uint32_t physical_address, uint32_t length) {
  if (!resolve_read_pages_ || !length) {
    return;
  }
  const uint32_t page_count = SharedMemory::kBufferSize >> 12;
  uint32_t first_page = physical_address >> 12;
  if (first_page >= page_count) {
    return;
  }
  uint32_t last_page =
      std::min((physical_address + length - 1) >> 12, page_count - 1);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    resolve_read_pages_[page >> 6].fetch_or(uint64_t(1) << (page & 63),
                                            std::memory_order_relaxed);
  }
}

// Returns whether the CPU has read any guest page in the range since a resolve
// last wrote it.
bool AnyResolvePageRead(uint32_t base, uint32_t length) const {
  if (!resolve_read_pages_ || !length) {
    return false;
  }
  const uint32_t page_count = SharedMemory::kBufferSize >> 12;
  uint32_t first_page = base >> 12;
  if (first_page >= page_count) {
    return false;
  }
  uint32_t last_page = std::min((base + length - 1) >> 12, page_count - 1);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (resolve_read_pages_[page >> 6].load(std::memory_order_relaxed) &
        (uint64_t(1) << (page & 63))) {
      return true;
    }
  }
  return false;
}

// The watch is one-shot, so it is re-armed on every resolve.
void ArmResolveReadWatch(uint32_t address, uint32_t length) {
  memory_->EnablePhysicalMemoryAccessCallbacks(address, length, false, true);
}

// Ends the resolve frame, and records the presented frontbuffer so scanout
// targets can be excluded from ring detection.
void NoteResolveFrame(uint32_t frontbuffer_ptr) {
  resolve_occurrences_.clear();
  ++resolve_frame_index_;
  uint32_t address = frontbuffer_ptr & 0x1FFFFFFF;
  if (!address) {
    return;
  }
  for (uint32_t frontbuffer : resolve_frontbuffers_) {
    if (frontbuffer == address) {
      return;
    }
  }
  resolve_frontbuffers_[resolve_frontbuffer_next_] = address;
  resolve_frontbuffer_next_ =
      (resolve_frontbuffer_next_ + 1) % kResolveFrontbufferCount;
}

bool IsScanoutResolveTarget(uint32_t address) const {
  for (uint32_t frontbuffer : resolve_frontbuffers_) {
    if (frontbuffer && frontbuffer == address) {
      return true;
    }
  }
  return false;
}

// Whether this resolve is one draw cycling a ring of destinations it owns
// exclusively, meaning something consumes the output a frame or more later.
// Slots are keyed by (extent length, occurrence of it this frame) so unrelated
// draws coming and going do not shift the key. Slots sharing an address are a
// transient pool instead, so any sharing disqualifies both forever.
bool IsPrivateRingResolve(uint32_t address, uint32_t length) {
  uint32_t occurrence = resolve_occurrences_[length]++;
  uint64_t slot_key = (uint64_t(length) << 32) | occurrence;
  auto slot = resolve_slots_.find(slot_key);
  if (slot == resolve_slots_.end()) {
    if (resolve_slots_.size() >= kMaxResolveSlots) {
      return false;
    }
    slot = resolve_slots_.emplace(slot_key, ResolveSlot()).first;
  }
  auto owner = resolve_slot_owners_.find(address);
  if (owner == resolve_slot_owners_.end()) {
    if (resolve_slot_owners_.size() < kMaxResolveSlotOwners) {
      resolve_slot_owners_.emplace(address, slot_key);
    }
  } else if (owner->second != slot_key) {
    slot->second.shared = true;
    auto other = resolve_slots_.find(owner->second);
    if (other != resolve_slots_.end()) {
      other->second.shared = true;
    }
  }
  if (slot->second.shared) {
    return false;
  }
  if (slot->second.address_count < ResolveSlot::kMaxAddresses) {
    bool known = false;
    for (uint32_t i = 0; i < slot->second.address_count; ++i) {
      if (slot->second.addresses[i] == address) {
        known = true;
        break;
      }
    }
    if (!known) {
      slot->second.addresses[slot->second.address_count++] = address;
    }
  }
  // Two distinct destinations for one slot is the ring, and nothing is written
  // before that, since guessing early and stopping later leaves a stale copy.
  return slot->second.address_count >= 2 && !IsScanoutResolveTarget(address);
}

// A coherency request the guest made while a destination is held back. The two
// kinds mean opposite things.
void NoteResolveCoherency(uint32_t base, uint32_t size, uint32_t status) {
  constexpr uint32_t kVcActionEna = uint32_t(1) << 24;
  constexpr uint32_t kTcActionEna = uint32_t(1) << 25;
  if (status & kVcActionEna) {
    // The driver's post-resolve flush, identified by the vertex cache action
    // rather than rb_copy_dest_base_ena, which only some titles set. It lands
    // on the same frame as the resolve, the only timing that helps. Matched by
    // base since the request carries the rect and not the tiled extent, which
    // is also what keeps titles that never name a destination from firing it.
    auto held = resolve_held_by_base_.find(base);
    if (held == resolve_held_by_base_.end()) {
      return;
    }
    ResolveHold hold = held->second;
    resolve_held_by_base_.erase(held);
    FlushResolveRangeToGuestRam(base, hold.length, hold.snapshot);
    return;
  }
  if (status & kTcActionEna) {
    // Texture cache invalidate alone is the opposite direction, the guest
    // saying it wrote this memory itself. Copying over it would destroy what
    // the guest produced, so only record the range as one it works with.
    uint64_t resolve_key = MakeReadbackResolveKey(base, size);
    if (resolve_destination_keys_.count(resolve_key)) {
      resolve_coherency_keys_.insert(resolve_key);
    }
  }
}

void NoteResolveDestination(uint64_t resolve_key) {
  if (!resolve_destination_keys_.insert(resolve_key).second) {
    return;
  }
  resolve_destination_key_order_.push_back(resolve_key);
  if (resolve_destination_key_order_.size() > kMaxResolveDestinationKeys) {
    resolve_destination_keys_.erase(resolve_destination_key_order_.front());
    resolve_destination_key_order_.pop_front();
  }
}

// snapshot means the bytes are in the backend's hold snapshot rather than the
// shared memory buffer.
void HoldResolveOutput(uint32_t address, uint32_t length, bool snapshot) {
  ResolveHold hold;
  hold.length = length;
  hold.snapshot = snapshot;
  auto held = resolve_held_by_base_.emplace(address, hold);
  if (!held.second) {
    // Same base at a different extent, the newest resolve is what a coherency
    // request would be about.
    held.first->second = hold;
    return;
  }
  resolve_held_base_order_.push_back(address);
  if (resolve_held_base_order_.size() > kMaxResolveHeldKeys) {
    resolve_held_by_base_.erase(resolve_held_base_order_.front());
    resolve_held_base_order_.pop_front();
  }
}

// Frames since this destination was last resolved, and records this resolve.
// A destination touched now and then is a capture rather than a render target,
// and captures are what gets read back. An unseen one reports a large gap, so
// a first resolve counts as intermittent.
uint32_t ResolveFrameGap(uint64_t resolve_key) {
  auto last = resolve_last_frame_.find(resolve_key);
  if (last != resolve_last_frame_.end()) {
    uint32_t gap = resolve_frame_index_ - last->second;
    last->second = resolve_frame_index_;
    return gap;
  }
  if (resolve_last_frame_.size() < kMaxResolveHeldKeys) {
    resolve_last_frame_.emplace(resolve_key, resolve_frame_index_);
    resolve_last_frame_order_.push_back(resolve_key);
    if (resolve_last_frame_order_.size() > kMaxResolveHeldKeys) {
      resolve_last_frame_.erase(resolve_last_frame_order_.front());
      resolve_last_frame_order_.pop_front();
    }
  }
  return UINT32_MAX;
}

// What a scaled resolve's readback needs before it can touch the GPU. False
// means it cannot be read back at all, with the reason already logged.
struct ScaledResolveReadbackInfo {
  uint32_t pixel_size_log2;
  uint32_t tile_count;
  // Whole 32x32 tiles only, a partial tail is truncated rather than copying
  // stale bytes to the guest.
  uint32_t readback_length;
  uint32_t scale_x;
  uint32_t scale_y;
  uint64_t scaled_start;
  uint64_t scaled_readback_length;
};
bool GetScaledResolveReadbackInfo(uint32_t address, uint32_t length,
                                  reg::RB_COPY_DEST_INFO copy_dest_info,
                                  ScaledResolveReadbackInfo& info_out) {
  // Texel size from the normalized copy_dest_info, not a re-read of
  // RB_COPY_DEST_INFO, for depth the register can hold a different size.
  uint32_t pixel_size_log2 =
      draw_util::GetResolveDownscalePixelSizeLog2(copy_dest_info);
  if (pixel_size_log2 > 3) {
    // 128bpp, which the downscale shader's tiled addressing reversal cannot
    // handle.
    XELOGGPU(
        "Skipping readback of a resolution-scaled resolve to a 128bpp "
        "destination - not supported by the downscale shader");
    return false;
  }
  // The scaled addressing is periodic per guest group, so the written extent
  // must be group-aligned for the per-tile reversal to be valid.
  uint32_t group_bytes_log2 = pixel_size_log2 <= 2 ? 7 : 6;
  if (address & ((uint32_t(1) << group_bytes_log2) - 1)) {
    XELOGGPU(
        "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
        "destination is not aligned to the scaled addressing group size",
        address);
    return false;
  }
  uint32_t tile_size_1x = (32u * 32u) << pixel_size_log2;
  uint32_t tile_count = length / tile_size_1x;
  if (!tile_count) {
    return false;
  }
  uint32_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t scale_y = texture_cache_->draw_resolution_scale_y();
  assert_true(scale_x >= 1 &&
              scale_x <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
  assert_true(scale_y >= 1 &&
              scale_y <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
  assert_true(scale_x > 1 || scale_y > 1);
  uint32_t readback_length = tile_count * tile_size_1x;
  uint32_t scale_area = scale_x * scale_y;
  uint64_t scaled_start = uint64_t(address) * scale_area;
  uint64_t scaled_readback_length = uint64_t(readback_length) * scale_area;
  // The source is bound at the written extent, which starts later than the
  // destination base, so the extent has to be inside the current range.
  uint64_t range_start_scaled =
      texture_cache_->GetCurrentScaledResolveRangeStartScaled();
  uint64_t range_length_scaled =
      texture_cache_->GetCurrentScaledResolveRangeLengthScaled();
  if (!range_length_scaled || scaled_start < range_start_scaled ||
      scaled_start + scaled_readback_length >
          range_start_scaled + range_length_scaled) {
    XELOGGPU(
        "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
        "written extent is not within the current scaled resolve range",
        address);
    return false;
  }
  info_out.pixel_size_log2 = pixel_size_log2;
  info_out.tile_count = tile_count;
  info_out.readback_length = readback_length;
  info_out.scale_x = scale_x;
  info_out.scale_y = scale_y;
  info_out.scaled_start = scaled_start;
  info_out.scaled_readback_length = scaled_readback_length;
  return true;
}

void DestroyResolveHoldSnapshot(uint32_t base) {
  auto snapshot = resolve_hold_snapshots_.find(base);
  if (snapshot == resolve_hold_snapshots_.end()) {
    return;
  }
  resolve_hold_snapshot_bytes_ -= snapshot->second.size;
  DestroyResolveHoldSnapshotBuffer(snapshot->second.buffer);
  resolve_hold_snapshots_.erase(snapshot);
}

// The buffer a held scaled resolve is downscaled into, null meaning not held.
ResolveHoldSnapshotBuffer* AcquireResolveHoldSnapshot(uint32_t base,
                                                      uint32_t length) {
  auto existing = resolve_hold_snapshots_.find(base);
  if (existing != resolve_hold_snapshots_.end() &&
      existing->second.size >= length) {
    // Repeat captures of one destination reuse its buffer.
    existing->second.last_frame = resolve_frame_index_;
    return &existing->second.buffer;
  }
  uint32_t size = AlignReadbackBufferSize(length);
  bool eviction_prepared = false;
  auto drop = [this, &eviction_prepared](uint32_t drop_base) {
    if (!eviction_prepared) {
      PrepareResolveHoldSnapshotEviction();
      eviction_prepared = true;
    }
    DestroyResolveHoldSnapshot(drop_base);
  };
  if (existing != resolve_hold_snapshots_.end()) {
    drop(base);
  }
  // Evicting rather than refusing, since every destination looks intermittent
  // on its first resolve and would otherwise fill the pool for good.
  while (resolve_hold_snapshot_bytes_ + size > kMaxResolveHoldSnapshotBytes &&
         !resolve_hold_snapshots_.empty()) {
    auto oldest = resolve_hold_snapshots_.begin();
    for (auto it = resolve_hold_snapshots_.begin();
         it != resolve_hold_snapshots_.end(); ++it) {
      if (it->second.last_frame < oldest->second.last_frame) {
        oldest = it;
      }
    }
    drop(oldest->first);
  }
  if (resolve_hold_snapshot_bytes_ + size > kMaxResolveHoldSnapshotBytes) {
    // One snapshot larger than the whole budget.
    return nullptr;
  }
  ResolveHoldSnapshot& snapshot = resolve_hold_snapshots_[base];
  if (!CreateResolveHoldSnapshotBuffer(snapshot.buffer, size)) {
    resolve_hold_snapshots_.erase(base);
    return nullptr;
  }
  snapshot.size = size;
  snapshot.last_frame = resolve_frame_index_;
  resolve_hold_snapshot_bytes_ += size;
  return &snapshot.buffer;
}

ResolveHoldSnapshotBuffer* FindResolveHoldSnapshot(uint32_t base) {
  auto snapshot = resolve_hold_snapshots_.find(base);
  return snapshot != resolve_hold_snapshots_.end() ? &snapshot->second.buffer
                                                   : nullptr;
}

// Only safe where teardown is, the backend hook does not defer past shutdown.
void ClearResolveHoldSnapshots() {
  for (auto& snapshot : resolve_hold_snapshots_) {
    DestroyResolveHoldSnapshotBuffer(snapshot.second.buffer);
  }
  resolve_hold_snapshots_.clear();
  resolve_hold_snapshot_bytes_ = 0;
}

// Decides what a resolve of [address, length) should do with its output, and
// whether a copy must stall the GPU afterwards. all copies every resolve. The
// default mode copies only what some signal says the guest will read back: a
// CPU read caught by the watch, a destination cycling a private ring, or an
// explicit coherency request for the range. Anything else stays where the
// resolve put it, which for GPU-side consumers is where they read it anyway.
// is_scaled is this resolve's own destination class, not the global scale.
ResolveHostCopyAction DecideResolveHostCopy(ReadbackResolveMode mode,
                                            uint32_t address, uint32_t length,
                                            bool sync, bool is_scaled,
                                            bool& stall_after_copy) {
  stall_after_copy = sync;
  if (mode != ReadbackResolveMode::kFast) {
    return ResolveHostCopyAction::kToGuestRam;
  }
  uint64_t resolve_key = MakeReadbackResolveKey(address, length);
  NoteResolveDestination(resolve_key);
  ArmResolveReadWatch(address, length);
  bool cpu_read = AnyResolvePageRead(address, length);
  bool private_ring = IsPrivateRingResolve(address, length);
  bool coherency = resolve_coherency_keys_.count(resolve_key) != 0;
  if (cpu_read || private_ring || coherency) {
    resolve_held_by_base_.erase(address);
    stall_after_copy = sync || coherency;
    return ResolveHostCopyAction::kToGuestRam;
  }
  // Hold the output, so the driver's post-resolve coherency request can release
  // it later this same frame. Only for a destination the guest has left alone
  // for a while: one it redraws every frame is a render target, and holding
  // those would hand over every resolve of them.
  if (ResolveFrameGap(resolve_key) < kResolveIntermittentFrames) {
    return ResolveHostCopyAction::kSkip;
  }
  if (!is_scaled) {
    // Already in the shared memory buffer, so a release is just a copy out.
    HoldResolveOutput(address, length, false);
    return ResolveHostCopyAction::kSkip;
  }
  // A scaled resolve has to be downscaled into a snapshot instead, and the
  // backend records that hold once the snapshot succeeds.
  return ResolveHostCopyAction::kToHoldSnapshot;
}

// Allocates the read-page bitset. Called from each backend's SetupContext,
// which then registers the per-backend read callback thunk.
void InitResolveReadWatch() {
  resolve_read_pages_ = std::make_unique<std::atomic<uint64_t>[]>(
      (SharedMemory::kBufferSize >> 12) / 64);
}

// Unregisters the read callback and drops all tracking. Called from each
// backend's ShutdownContext.
void ResetResolveReadWatch() {
  if (resolve_read_callback_) {
    memory_->UnregisterPhysicalMemoryReadCallback(resolve_read_callback_);
    resolve_read_callback_ = nullptr;
  }
  resolve_read_pages_.reset();
  resolve_slots_.clear();
  resolve_slot_owners_.clear();
  resolve_occurrences_.clear();
  resolve_coherency_keys_.clear();
  resolve_held_by_base_.clear();
  resolve_held_base_order_.clear();
  resolve_last_frame_.clear();
  resolve_last_frame_order_.clear();
  resolve_destination_keys_.clear();
  resolve_destination_key_order_.clear();
}

// Guest-page bitset of pages the CPU read since a resolve wrote them. Set on
// the guest thread from the read callback, read on the GPU thread, so atomic.
std::unique_ptr<std::atomic<uint64_t>[]> resolve_read_pages_;

// One per (extent length, occurrence within the frame).
struct ResolveSlot {
  static constexpr uint32_t kMaxAddresses = 8;
  uint32_t addresses[kMaxAddresses] = {};
  uint32_t address_count = 0;
  // Another slot writes one of these addresses, so this is a shared pool.
  bool shared = false;
};
std::unordered_map<uint64_t, ResolveSlot> resolve_slots_;
// Destination address to the slot that owns it, for the sharing test.
std::unordered_map<uint32_t, uint64_t> resolve_slot_owners_;
// Extent length to how many resolves of it have happened this frame. Cleared
// by NoteResolveFrame.
std::unordered_map<uint32_t, uint32_t> resolve_occurrences_;
static constexpr size_t kMaxResolveSlots = 1024;
static constexpr size_t kMaxResolveSlotOwners = 8192;
// Ranges an exact-extent coherency request has asked for. Sticky, so once the
// guest has shown it reads a range back, later resolves of it are written
// without waiting to be asked again.
std::unordered_set<uint64_t> resolve_coherency_keys_;
// A resolve's output held back from guest RAM, still flushable if the guest
// asks. snapshot tells the backend which buffer to flush out of.
struct ResolveHold {
  uint32_t length = 0;
  bool snapshot = false;
};
// Downscaled output of a held scaled resolve, taken at resolve time because
// the scaled resolve buffer cannot be downscaled from once the release comes.
struct ResolveHoldSnapshot {
  ResolveHoldSnapshotBuffer buffer;
  uint32_t size = 0;
  // Resolve frame this was last taken in, for eviction.
  uint32_t last_frame = 0;
};
std::unordered_map<uint32_t, ResolveHoldSnapshot> resolve_hold_snapshots_;
uint64_t resolve_hold_snapshot_bytes_ = 0;
// Only intermittent destinations are ever held, so a few at a time is enough.
static constexpr uint64_t kMaxResolveHoldSnapshotBytes = 64 * 1024 * 1024;
// Held ranges by destination base. Bounded via the FIFO below.
std::unordered_map<uint32_t, ResolveHold> resolve_held_by_base_;
std::deque<uint32_t> resolve_held_base_order_;
// Frame index of each destination's last resolve, for the intermittency test.
std::unordered_map<uint64_t, uint32_t> resolve_last_frame_;
std::deque<uint64_t> resolve_last_frame_order_;
static constexpr size_t kMaxResolveHeldKeys = 4096;
// How many frames a destination must go untouched to count as a capture
// rather than a render target. BG&E redraws its render targets every frame and
// resolves its screenshot buffer once per capture, so anything well above 1
// separates them.
static constexpr uint32_t kResolveIntermittentFrames = 8;
uint32_t resolve_frame_index_ = 0;
// Every range some resolve has written, held or not. A coherency request is
// only honoured for one of these, so a request naming unrelated memory cannot
// make us copy whatever happens to be in the buffer there.
std::unordered_set<uint64_t> resolve_destination_keys_;
std::deque<uint64_t> resolve_destination_key_order_;
static constexpr size_t kMaxResolveDestinationKeys = 4096;
// Recently presented frontbuffers, excluded from ring detection.
static constexpr uint32_t kResolveFrontbufferCount = 4;
uint32_t resolve_frontbuffers_[kResolveFrontbufferCount] = {};
uint32_t resolve_frontbuffer_next_ = 0;
// Handle from RegisterPhysicalMemoryReadCallback, or null when not armed.
void* resolve_read_callback_ = nullptr;
