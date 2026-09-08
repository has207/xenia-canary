/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_PIPELINE_PUBLISH_ORDER_H_
#define XENIA_GPU_PIPELINE_PUBLISH_ORDER_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace xe {
namespace gpu {

// Orders the publication of asynchronously created pipelines. One that finishes
// early is held until every pipeline queued before it has been published, so
// the real pipelines replace their placeholders in the order the game drew
// them, and a pass never starts sampling a producer still drawing with its
// placeholder. Compilation stays parallel - only the store is ordered.
//
// T is whatever one backend needs to carry out a single store.
template <typename T>
class PipelinePublishOrder {
 public:
  // Sequence for a pipeline being queued, 1-based. Call it under the same lock
  // that orders the queue push, so the numbering matches the queue. 0 is
  // reserved for entries that publish as soon as they are built - the storage
  // warm-up, whose pipelines nothing is drawing yet.
  uint32_t NextSequence() { return sequence_next_++; }

  // Takes a finished pipeline and stores whatever run of publishes it
  // completes, in order - nothing while an earlier one is still being built.
  // |store| runs under the order lock, so a thread that completes the next run
  // waits for the one before it to be stored rather than overtaking it. Keep it
  // to the store itself, and don't take a lock anything holds while publishing.
  // Sequence 0 passes straight through.
  template <typename F>
  void Publish(uint32_t sequence, T value, F&& store) {
    if (!sequence) {
      store(value);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    parked_.emplace(sequence, std::move(value));
    auto it = parked_.begin();
    while (it != parked_.end() && it->first == cursor_) {
      store(it->second);
      it = parked_.erase(it);
      ++cursor_;
    }
  }

  // Returns whatever is still held back, for the backend to destroy, and starts
  // the order over. For shutdown, once the creation threads are joined.
  std::vector<T> Reset() {
    std::vector<T> parked;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : parked_) {
      parked.push_back(std::move(entry.second));
    }
    parked_.clear();
    cursor_ = 1;
    sequence_next_ = 1;
    return parked;
  }

 private:
  std::mutex mutex_;
  // Not guarded by mutex_ - see NextSequence.
  uint32_t sequence_next_ = 1;
  uint32_t cursor_ = 1;
  std::map<uint32_t, T> parked_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_PIPELINE_PUBLISH_ORDER_H_
