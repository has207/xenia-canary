/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_PIPELINE_CREATION_QUEUE_H_
#define XENIA_GPU_PIPELINE_CREATION_QUEUE_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/pipeline_publish_order.h"

namespace xe {
namespace gpu {

// Asynchronous pipeline creation, shared by the backends: the request queue,
// the creation threads, the publish ordering and the completion handshake. The
// backend supplies the request, the handle creating it produces, the state a
// creation thread needs to do the work, and the two functions that create and
// store one pipeline.
template <typename TRequest, typename THandle, typename TThreadContext>
class PipelineCreationQueue {
 public:
  using Publication = std::pair<TRequest, THandle>;
  // Creates one pipeline, returning a null handle if it failed. Runs on a
  // creation thread, with that thread's context.
  using Creator = std::function<THandle(const TRequest&, TThreadContext*)>;
  // Swaps a created pipeline - or a failure, a null handle - into its entry.
  // Called in queue order, under the publish order's lock, never the queue's.
  using Store = std::function<void(const TRequest&, THandle)>;
  // The state one creation thread builds and owns for its lifetime.
  using ContextFactory = std::function<std::unique_ptr<TThreadContext>()>;

  // Prepares the queue. Call it even with no creation threads - the storage
  // warm-up may start some later.
  void Initialize(const char* thread_name, Creator creator, Store store,
                  ContextFactory context_factory) {
    thread_name_ = thread_name;
    creator_ = std::move(creator);
    store_ = std::move(store);
    context_factory_ = std::move(context_factory);
    threads_busy_ = 0;
    completion_set_event_ = false;
    threads_shutdown_from_ = SIZE_MAX;
    completion_event_ = xe::threading::Event::CreateManualResetEvent(true);
    assert_not_null(completion_event_);
  }

  // Stops the threads and drops everything still queued. Returns what was held
  // back for publishing, for the backend to destroy - nothing will publish it
  // now.
  std::vector<Publication> Shutdown() {
    SetThreadCount(0);
    completion_event_.reset();
    {
      std::lock_guard<std::mutex> lock(lock_);
      queue_ = {};
      completion_callback_ = nullptr;
      completion_set_event_ = false;
    }
    return publish_order_.Reset();
  }

  bool has_threads() const { return !threads_.empty(); }
  size_t thread_count() const { return threads_.size(); }

  // Grows or shrinks the pool. Shrink with the queue drained - a thread only
  // leaves between requests, so anything still queued would be stranded.
  void SetThreadCount(size_t count) {
    if (count < threads_.size()) {
      {
        std::lock_guard<std::mutex> lock(lock_);
        threads_shutdown_from_ = count;
      }
      cond_.notify_all();
      while (threads_.size() > count) {
        xe::threading::Wait(threads_.back().get(), false);
        threads_.pop_back();
      }
      // So threads can be added again later.
      std::lock_guard<std::mutex> lock(lock_);
      threads_shutdown_from_ = SIZE_MAX;
      return;
    }
    while (threads_.size() < count) {
      size_t thread_index = threads_.size();
      std::unique_ptr<xe::threading::Thread> thread =
          xe::threading::Thread::Create(
              {}, [this, thread_index]() { ThreadMain(thread_index); });
      assert_not_null(thread);
      thread->set_name(thread_name_);
      threads_.push_back(std::move(thread));
    }
  }

  // Queues a pipeline the game is drawing now - it is stored once every request
  // queued before it has been.
  void Push(TRequest request) { PushRequest(std::move(request), true); }

  // Queues one nothing is drawing yet (the storage warm-up), stored as soon as
  // it is built.
  void PushUnordered(TRequest request) {
    PushRequest(std::move(request), false);
  }

  // Whether anything is queued or being created.
  bool IsBusy() {
    if (threads_.empty()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(lock_);
    return IsBusyLocked();
  }

  // Wakes a creation thread without waiting for it.
  void Notify() { cond_.notify_one(); }

  // Blocks until everything queued has been created.
  void AwaitCompletion() {
    if (threads_.empty()) {
      return;
    }
    bool await;
    {
      std::lock_guard<std::mutex> lock(lock_);
      await = IsBusyLocked();
      if (await) {
        completion_event_->Reset();
        completion_set_event_ = true;
      }
    }
    if (await) {
      cond_.notify_one();
      xe::threading::Wait(completion_event_.get(), false);
    }
  }

  // Takes the callback to invoke when everything queued has been created, and
  // leaves it with the caller if there is nothing to wait for.
  void TakeCompletionCallback(std::function<void()>& callback) {
    std::lock_guard<std::mutex> lock(lock_);
    if (!IsBusyLocked()) {
      return;
    }
    completion_callback_ = std::move(callback);
    callback = nullptr;
  }

  // Creates whatever is queued on the calling thread, with a context of its
  // own. For the storage warm-up, which has a thread to spare.
  void DrainOnCallingThread(TThreadContext* context) {
    while (CreateOneQueued(context)) {
    }
  }

 private:
  bool IsBusyLocked() const { return !queue_.empty() || threads_busy_ != 0; }

  void PushRequest(TRequest request, bool ordered) {
    {
      std::lock_guard<std::mutex> lock(lock_);
      // Numbered under the lock that orders the push, so the publish order
      // matches the queue order.
      queue_.emplace(ordered ? publish_order_.NextSequence() : 0,
                     std::move(request));
    }
    cond_.notify_one();
  }

  // Creates the next queued pipeline, or returns false with nothing queued.
  bool CreateOneQueued(TThreadContext* context) {
    TRequest request;
    uint32_t sequence;
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (queue_.empty()) {
        return false;
      }
      // Busy until the pipeline is created rather than just dequeued: other
      // threads must be able to take requests, but must not report completion
      // while this one is still building.
      sequence = queue_.front().first;
      request = std::move(queue_.front().second);
      queue_.pop();
      ++threads_busy_;
    }
    THandle handle = creator_(request, context);
    // Published even when creation failed: the order has to advance past a
    // failure, or everything queued after it waits on it forever.
    publish_order_.Publish(sequence, {std::move(request), handle},
                           [this](Publication& publication) {
                             store_(publication.first, publication.second);
                           });
    std::unique_lock<std::mutex> lock(lock_);
    --threads_busy_;
    SignalCompletionLocked(lock);
    return true;
  }

  // Called by an idle thread with the lock held, which it drops around the
  // callback.
  void SignalCompletionLocked(std::unique_lock<std::mutex>& lock) {
    if (IsBusyLocked()) {
      return;
    }
    if (completion_set_event_) {
      // Blocking mode.
      completion_set_event_ = false;
      completion_event_->Set();
    }
    if (completion_callback_) {
      // Non-blocking mode.
      auto callback = std::move(completion_callback_);
      completion_callback_ = nullptr;
      lock.unlock();
      callback();
      lock.lock();
    }
  }

  void ThreadMain(size_t thread_index) {
    std::unique_ptr<TThreadContext> context =
        context_factory_ ? context_factory_() : nullptr;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(lock_);
        // A thread being shut down stops taking work, leaving the rest of the
        // queue to whoever is left.
        if (thread_index >= threads_shutdown_from_ || queue_.empty()) {
          // Also the path a thread woken only to report completion takes.
          SignalCompletionLocked(lock);
          if (thread_index >= threads_shutdown_from_) {
            return;
          }
          cond_.wait(lock);
          continue;
        }
      }
      CreateOneQueued(context.get());
    }
  }

  std::string thread_name_;
  Creator creator_;
  Store store_;
  ContextFactory context_factory_;

  std::mutex lock_;
  std::condition_variable cond_;
  // Pipelines are never evicted - games have a finite set that should all stay
  // cached - so this holds nothing but requests waiting for a thread. FIFO, so
  // they build in the order the game first drew them.
  std::queue<std::pair<uint32_t, TRequest>> queue_;
  PipelinePublishOrder<Publication> publish_order_;
  // Threads that have taken a request but not finished creating it. Guarded by
  // lock_.
  size_t threads_busy_ = 0;
  // Manual-reset event set when the queue drains, for a blocking wait. Whether
  // it is wanted is guarded by lock_.
  std::unique_ptr<xe::threading::Event> completion_event_;
  bool completion_set_event_ = false;
  // Invoked instead when the wait is non-blocking. Guarded by lock_.
  std::function<void()> completion_callback_;
  // Threads with this index or above leave as soon as they are idle. Guarded
  // by lock_, notify_all cond_ when set.
  size_t threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<xe::threading::Thread>> threads_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_PIPELINE_CREATION_QUEUE_H_
