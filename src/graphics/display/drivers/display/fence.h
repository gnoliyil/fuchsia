// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_DISPLAY_FENCE_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_DISPLAY_FENCE_H_

#ifndef _ALL_SOURCE
#define _ALL_SOURCE  // To get MTX_INIT
#endif

#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>
#include <lib/zx/event.h>
#include <threads.h>

#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_single_list.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "src/graphics/display/drivers/display/id-map.h"

namespace display {

class FenceReference;
class Fence;

class FenceCallback {
 public:
  virtual void OnFenceFired(FenceReference* ref) = 0;
  virtual void OnRefForFenceDead(Fence* fence) = 0;
};

// Class which wraps an event into a fence. A single Fence can have multiple FenceReference
// objects, which allows an event to be treated as a semaphore independently of it being
// imported/released (i.e. can be released while still in use).
class Fence : public fbl::RefCounted<Fence>,
              public IdMappable<fbl::RefPtr<Fence>>,
              public fbl::SinglyLinkedListable<fbl::RefPtr<Fence>> {
 public:
  Fence(FenceCallback* cb, async_dispatcher_t* dispatcher, uint64_t id, zx::event&& event);
  ~Fence();

  // Creates a new FenceReference when an event is imported.
  bool CreateRef();
  // Clears a FenceReference when an event is released. Note that references to the cleared
  // FenceReference might still exist within the driver.
  void ClearRef();
  // Decrements the reference count and returns true if the last ref died.
  bool OnRefDead();

  // Gets the fence reference for the current import. An individual fence reference cannot
  // be used for multiple things simultaneously.
  fbl::RefPtr<FenceReference> GetReference();

  // The raw event underlying this fence. Only used for validation.
  zx_handle_t event() const { return event_.get(); }

 private:
  void Signal();
  void OnRefDied();
  zx_status_t OnRefArmed(fbl::RefPtr<FenceReference>&& ref);
  void OnRefDisarmed(FenceReference* ref);

  // The fence reference corresponding to the current event import.
  fbl::RefPtr<FenceReference> cur_ref_;

  // A queue of fence references which are being waited upon. When the event is
  // signaled, the signal will be cleared and the first fence ref will be marked ready.
  fbl::DoublyLinkedList<fbl::RefPtr<FenceReference>> armed_refs_;

  void OnReady(async_dispatcher_t* dispatcher, async::WaitBase* self, zx_status_t status,
               const zx_packet_signal_t* signal);
  async::WaitMethod<Fence, &Fence::OnReady> ready_wait_{this};

  FenceCallback* cb_;
  async_dispatcher_t* dispatcher_;
  zx::event event_;
  int ref_count_ = 0;
  zx_koid_t koid_ = 0;

  friend FenceReference;

  DISALLOW_COPY_ASSIGN_AND_MOVE(Fence);
};

class FenceReference : public fbl::RefCounted<FenceReference>,
                       public fbl::DoublyLinkedListable<fbl::RefPtr<FenceReference>> {
 public:
  explicit FenceReference(fbl::RefPtr<Fence> fence);
  ~FenceReference();

  void Signal();

  zx_status_t StartReadyWait();
  void ResetReadyWait();
  // Sets the fence which will be signaled immediately when this fence is ready.
  void SetImmediateRelease(fbl::RefPtr<FenceReference>&& fence);

  void OnReady();

 private:
  fbl::RefPtr<Fence> fence_;

  fbl::RefPtr<FenceReference> release_fence_;

  DISALLOW_COPY_ASSIGN_AND_MOVE(FenceReference);
};

// FenceCollection controls the access and lifecycles for several display::Fences.
class FenceCollection : private FenceCallback {
 public:
  FenceCollection() = delete;
  FenceCollection(const FenceCollection&) = delete;
  FenceCollection(FenceCollection&&) = delete;

  // fired_cb will be called whenever a fence fires, from dispatcher's threads.
  FenceCollection(async_dispatcher_t* dispatcher, fit::function<void(FenceReference*)>&& fired_cb);

  // Explicit destruction step. Use this to control when fences are destroyed.
  void Clear() __TA_EXCLUDES(mtx_);

  zx_status_t ImportEvent(zx::event event, uint64_t id) __TA_EXCLUDES(mtx_);
  void ReleaseEvent(uint64_t id) __TA_EXCLUDES(mtx_);
  fbl::RefPtr<FenceReference> GetFence(uint64_t id) __TA_EXCLUDES(mtx_);

 private:
  // |FenceCallback|
  void OnFenceFired(FenceReference* fence) override;

  // |FenceCallback|
  void OnRefForFenceDead(Fence* fence) __TA_EXCLUDES(mtx_) override;

  mtx_t mtx_ = MTX_INIT;
  Fence::Map fences_ __TA_GUARDED(mtx_);
  async_dispatcher_t* dispatcher_;
  fit::function<void(FenceReference*)> fired_cb_;
};

}  // namespace display

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_DISPLAY_FENCE_H_
