// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FDIO_SPAWN_ACTIONS_H_
#define LIB_FDIO_SPAWN_ACTIONS_H_

#include <lib/fdio/spawn.h>
#include <lib/zx/object.h>
#include <zircon/availability.h>

#include <vector>

struct FdioSpawnActionWithHandle {
  fdio_spawn_action_t action;
  zx_handle_t handle;
} ZX_AVAILABLE_SINCE(1);

// FdioSpawnActions maintains a fdio_spawn_action_t array and all the handles associated with the
// actions. All the handles would be closed on destruction unless 'GetActions' is called and then
// caller should pass the returned actions array that owns the handles to fdio_spawn_etc to transfer
// the handles.
class FdioSpawnActions final {
 public:
  ~FdioSpawnActions() {
    for (FdioSpawnActionWithHandle action_with_handle : actions_with_handle_) {
      if (action_with_handle.handle != ZX_HANDLE_INVALID) {
        zx_handle_close(action_with_handle.handle);
      }
    }
  }

  void AddAction(fdio_spawn_action_t action) ZX_AVAILABLE_SINCE(1) {
    FdioSpawnActionWithHandle action_with_handle = {
        .action = action,
        .handle = ZX_HANDLE_INVALID,
    };
    actions_with_handle_.push_back(action_with_handle);
  }

  template <typename T,
            typename = typename std::enable_if<std::is_base_of<zx::object_base, T>::value>::type>
  void AddActionWithHandle(fdio_spawn_action_t action, T handle) ZX_AVAILABLE_SINCE(1) {
    AddActionWithHandleInner(action, handle.release());
  }

  template <typename T,
            typename = typename std::enable_if<std::is_base_of<zx::object_base, T>::value>::type>
  void AddActionWithNamespace(fdio_spawn_action_t action, T handle) ZX_AVAILABLE_SINCE(1) {
    AddActionWithNamespaceInner(action, handle.release());
  }

  std::vector<fdio_spawn_action_t> GetActions() ZX_AVAILABLE_SINCE(1) {
    // Return the stored actions array along with the ownership for all the associated objects back
    // to the caller.
    // Caller should call fdio_spawn_etc immediately after this call and this class's state would be
    // reinitialized.
    std::vector<fdio_spawn_action_t> actions;
    for (FdioSpawnActionWithHandle action_with_handle : actions_with_handle_) {
      actions.push_back(action_with_handle.action);
    }
    actions_with_handle_.clear();
    return actions;
  }

 private:
  std::vector<FdioSpawnActionWithHandle> actions_with_handle_;

  void AddActionWithHandleInner(fdio_spawn_action_t action, zx_handle_t handle) {
    action.h.handle = handle;
    FdioSpawnActionWithHandle action_with_handle = {
        .action = action,
        .handle = handle,
    };
    actions_with_handle_.push_back(action_with_handle);
  }

  void AddActionWithNamespaceInner(fdio_spawn_action_t action, zx_handle_t handle) {
    action.ns.handle = handle;
    FdioSpawnActionWithHandle action_with_handle = {
        .action = action,
        .handle = handle,
    };
    actions_with_handle_.push_back(action_with_handle);
  }
} ZX_AVAILABLE_SINCE(1);

#endif  // LIB_FDIO_SPAWN_ACTIONS_H_
