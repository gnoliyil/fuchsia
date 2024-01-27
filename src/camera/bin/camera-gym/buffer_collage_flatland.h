// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_BIN_CAMERA_GYM_BUFFER_COLLAGE_FLATLAND_H_
#define SRC_CAMERA_BIN_CAMERA_GYM_BUFFER_COLLAGE_FLATLAND_H_

#include <fuchsia/math/cpp/fidl.h>
#include <fuchsia/sysmem/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/composition/cpp/fidl.h>
#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/wait.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fpromise/promise.h>
#include <lib/fpromise/result.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/resources.h>

#include <cstdint>
#include <map>

#include "fuchsia/camera/gym/cpp/fidl.h"
#include "src/lib/ui/flatland-frame-scheduling/src/simple_present.h"
#include "src/ui/scenic/lib/allocation/buffer_collection_import_export_tokens.h"

namespace camera_flatland {
using fuchsia::ui::composition::ContentId;
using fuchsia::ui::composition::LayoutInfo;
using fuchsia::ui::composition::TransformId;

// Returns an event such that when the event is signaled and the dispatcher executed, the provided
// eventpair is closed. This can be used to bridge event- and eventpair-based fence semantics. If
// this function returns an error, |eventpair| is closed immediately.
inline fpromise::result<zx::event, zx_status_t> MakeEventBridge(async_dispatcher_t* dispatcher,
                                                                zx::eventpair* eventpair) {
  zx::event caller_event;
  zx::event waiter_event;
  zx_status_t status = zx::event::create(0, &caller_event);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  status = caller_event.duplicate(ZX_RIGHT_SAME_RIGHTS, &waiter_event);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  // A shared_ptr is necessary in order to begin the wait after setting the wait handler.
  auto wait = std::make_shared<async::Wait>(waiter_event.get(), ZX_EVENT_SIGNALED);
  wait->set_handler([wait, waiter_event = std::move(waiter_event)](
                        async_dispatcher_t* /*unused*/, async::Wait* /*unused*/,
                        zx_status_t /*unused*/, const zx_packet_signal_t* /*unused*/) mutable {
    // Close the waiter along with its captures.
    wait = nullptr;
  });
  status = wait->Begin(dispatcher);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status);
    return fpromise::error(status);
  }
  return fpromise::ok(std::move(caller_event));
}

// Defines a view used to render a single camera stream.
struct CollectionView {
  fuchsia::sysmem::ImageFormat_2 image_format;
  fuchsia::sysmem::BufferCollectionPtr buffer_collection;
  std::map<uint32_t, ContentId> buffer_id_to_content_id;
  uint32_t buffer_count;
  bool muted = false;
  bool view_created = false;
  TransformId transform_id{0};  // transform_id stays constant throughout view lifecycle.
  allocation::BufferCollectionImportExportTokens ref_pair;
};

// This class takes ownership of the display and presents the contents of buffer collections in a
// grid pattern. Unless otherwise noted, public methods are thread-safe and private methods must
// only be called from the loop's thread.
class BufferCollageFlatland : public fuchsia::ui::app::ViewProvider {
 public:
  using CommandStatusHandler =
      fit::function<void(fuchsia::camera::gym::Controller_SendCommand_Result)>;

  ~BufferCollageFlatland() override;

  // Creates a new BufferCollage instance using the provided interface handles. After returning, if
  // the instance stops running, either due to an error or explicit action, |stop_callback| is
  // invoked exactly once if non-null.
  static fpromise::result<std::unique_ptr<BufferCollageFlatland>, zx_status_t> Create(
      std::unique_ptr<simple_present::FlatlandConnection> flatland_connection,
      fuchsia::ui::composition::AllocatorHandle flatland_allocator,
      fuchsia::sysmem::AllocatorHandle sysmem_allocator, fit::closure stop_callback = nullptr);

  // Returns the view request handler.
  fidl::InterfaceRequestHandler<fuchsia::ui::app::ViewProvider> GetHandler();

  // Registers a new buffer collection and adds it to the views, updating the layout of existing
  // collections to fit. Returns an id representing the collection. Collections are initially
  // hidden and must be made visible using PostSetCollectionVisibility.
  fpromise::promise<uint32_t> AddCollection(fuchsia::sysmem::BufferCollectionTokenHandle token,
                                            fuchsia::sysmem::ImageFormat_2 image_format,
                                            std::string description);

  // Removes the collection with the given |collection_id| from the view and updates the layout to
  // fill the vacated space. If |id| is not a valid collection, the instance stops.
  void RemoveCollection(uint32_t collection_id);

  // Updates the view to show the given |buffer_index| in for the given |collection_id|'s node.
  // Holds |release_fence| until the buffer is no longer needed, then closes the handle. If
  // non-null, |subregion| specifies what sub-region of the buffer to highlight.
  void PostShowBuffer(uint32_t collection_id, uint32_t buffer_index, zx::eventpair* release_fence,
                      std::optional<fuchsia::math::RectF> subregion);

 private:
  BufferCollageFlatland();

  // Requests a new view.
  void OnNewRequest(fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> request);

  // Disconnects all channels, quits the loop, and calls the stop callback.
  void Stop();

  // Registers the provided interface's error handler to invoke Stop.
  template <typename T>
  void SetStopOnError(fidl::InterfacePtr<T>& p, std::string name = T::Name_);

  // Registers the provided interface's error handler to invoke normal tear down upon any error.
  // Primarily designed to handle respond to any BufferCollection errors by removing CollectionView.
  template <typename T>
  void SetRemoveCollectionViewOnError(fidl::InterfacePtr<T>& p, uint32_t view_id, std::string name);

  // Presents buffer content on screen. See PostShowBuffer.
  void ShowBuffer(uint32_t collection_id, uint32_t buffer_index, zx::eventpair* release_fence,
                  std::optional<fuchsia::math::RectF> subregion);

  // Repositions scenic nodes to fit all collections on the screen.
  void UpdateLayout();

  // Initialize flatland root view.
  void SetupBaseView();

  // |fuchsia::ui::app::ViewProvider|
  void CreateView2(fuchsia::ui::app::CreateView2Args args) override;

  // |fuchsia::ui::app::ViewProvider|
  void CreateViewWithViewRef(zx::eventpair view_token,
                             fuchsia::ui::views::ViewRefControl view_ref_control,
                             fuchsia::ui::views::ViewRef view_ref) override;

  // Thread used for processing camera stream buffers and calling Flatland API.
  async::Loop loop_;
  fuchsia::sysmem::AllocatorPtr sysmem_allocator_;
  fit::closure stop_callback_;
  std::unique_ptr<simple_present::FlatlandConnection> flatland_connection_;
  fuchsia::ui::composition::Flatland* flatland_;
  fuchsia::ui::composition::ParentViewportWatcherPtr parent_watcher_;
  fuchsia::ui::composition::AllocatorPtr flatland_allocator_;
  fidl::Binding<fuchsia::ui::app::ViewProvider> view_provider_binding_;
  std::map<uint32_t, CollectionView> collection_views_;

  uint32_t width_ = 0;
  uint32_t height_ = 0;

  const TransformId kRootTransformId{.value = 1};
  int32_t next_collection_id_ = 1;
  unsigned int next_transform_id = 3;
  unsigned int next_content_id = 2;

  zx::time start_time_;
};

}  // namespace camera_flatland

#endif  // SRC_CAMERA_BIN_CAMERA_GYM_BUFFER_COLLAGE_FLATLAND_H_
