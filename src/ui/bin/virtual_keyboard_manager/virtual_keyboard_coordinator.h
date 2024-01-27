// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_BIN_VIRTUAL_KEYBOARD_MANAGER_VIRTUAL_KEYBOARD_COORDINATOR_H_
#define SRC_UI_BIN_VIRTUAL_KEYBOARD_MANAGER_VIRTUAL_KEYBOARD_COORDINATOR_H_

#include <fuchsia/input/virtualkeyboard/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/component_context.h>
#include <zircon/types.h>

#include <memory>

#include "src/ui/bin/virtual_keyboard_manager/focus_listener.h"
#include "src/ui/bin/virtual_keyboard_manager/virtual_keyboard_controller.h"
#include "src/ui/bin/virtual_keyboard_manager/virtual_keyboard_manager.h"

namespace virtual_keyboard_manager {

class VirtualKeyboardController;

// Methods called by `VirtualKeyboardControllerCreator` and `VirtualKeyboardManager`.
// Factored into a separate class to support unit testing.
class VirtualKeyboardCoordinator : public FocusListener {
 public:
  virtual ~VirtualKeyboardCoordinator() = default;

  // Reports a change in the virtual keyboard's visibility, along with the reason
  // for the change.
  virtual void NotifyVisibilityChange(
      bool is_visible, fuchsia::input::virtualkeyboard::VisibilityChangeReason reason) = 0;

  // Requests a change in the visibility and/or text type of the virtual keyboard.
  virtual void RequestTypeAndVisibility(zx_koid_t requestor_view_koid,
                                        fuchsia::input::virtualkeyboard::TextType text_type,
                                        bool is_visibile) = 0;

  // Reports an error from the Manager. The coordinator should close the corresponding FIDL
  // connection with `error`.
  virtual void NotifyManagerError(zx_status_t error) = 0;

  // Reports a change in view focus. The coordinator should
  // a) dismiss the keyboard, and
  // b) process any pending RequestTypeAndVisibility() calls for `view_ref`, and
  // c) accept new RequestTypeAndVisibility() calls for `view_ref`.
  virtual void NotifyFocusChange(fuchsia::ui::views::ViewRef focused_view) = 0;
};

// Coordinates all activities for a single virtual keyboard.
//
// This includes:
// * Publishing the `fuchsia.input.virtualkeyboard.ControllerCreator` FIDL
//   protocol, and binding `VirtualKeyboardController`s to the virtual keyboard.
// * Publishing the `fuchsia.input.virtualkeyboard.Manager` FIDL protocol, and
//   binding a `VirtualKeyboardManager` to the virtual keyboard.
// * Relaying messages between `VirtualKeyboardController`s and the
//   `VirtualKeyboardManager`
class FidlBoundVirtualKeyboardCoordinator
    : public fuchsia::input::virtualkeyboard::ControllerCreator,
      public VirtualKeyboardCoordinator {
 public:
  // Constructs a VirtualKeyboardCoordinator, and publishes the relevant FIDLs
  // using `component_context`.
  //
  // Callers _should_ construct this object before entering the event loop.
  explicit FidlBoundVirtualKeyboardCoordinator(sys::ComponentContext* component_context);
  ~FidlBoundVirtualKeyboardCoordinator() override;

  fxl::WeakPtr<FidlBoundVirtualKeyboardCoordinator> GetWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  // |VirtualKeyboardCoordinator|
  void NotifyVisibilityChange(
      bool is_visible, fuchsia::input::virtualkeyboard::VisibilityChangeReason reason) override;
  void NotifyManagerError(zx_status_t error) override;
  void RequestTypeAndVisibility(zx_koid_t requestor_view_koid,
                                fuchsia::input::virtualkeyboard::TextType text_type,
                                bool is_visible) override;
  void NotifyFocusChange(fuchsia::ui::views::ViewRef focused_view) override;

  void SetControllerForTest(std::unique_ptr<VirtualKeyboardController> controller) {
    controller_bindings_.CloseAll();
    controller_bindings_.AddBinding(std::move(controller));
  }

 private:
  struct KeyboardConfig {
    fuchsia::input::virtualkeyboard::TextType text_type;
    bool is_visible;
  };

  // |fuchsia.input.virtualkeyboard.ControllerCreator|
  void Create(fuchsia::ui::views::ViewRef view_ref,
              fuchsia::input::virtualkeyboard::TextType text_type,
              fidl::InterfaceRequest<fuchsia::input::virtualkeyboard::Controller>
                  controller_request) override;

  // Creates a VirtualKeyboardManager and binds the manager to the provided channel.
  void BindManager(fidl::InterfaceRequest<fuchsia::input::virtualkeyboard::Manager>);

  // Destroys the VirtualKeyboardManager, and closes the associated channel with
  // the provided status as the epitaph.
  void HandleManagerBindingError(zx_status_t);

  // Applies the `KeyboardConfig` for `focused_view_koid_` in
  // `view_koid_to_pending_manager_config_`, if
  // a) such a `KeyboardConfig` exists, AND
  // b) a `fuchsia.input.virtualkeyboard.Manager` is bound.
  //
  // Also clears the entry for `focused_view_koid_` in the map,
  // if it exists.
  //
  // Returns the KOID of the View whose request was applied, or
  // a string describing why the request was not applied.
  std::variant<zx_koid_t, std::string> ApplyFocusedRequest();

  fidl::BindingSet<fuchsia::input::virtualkeyboard::ControllerCreator> creator_bindings_;
  fidl::BindingSet<fuchsia::input::virtualkeyboard::Controller,
                   std::unique_ptr<VirtualKeyboardController>>
      controller_bindings_;

  std::optional<fidl::Binding<fuchsia::input::virtualkeyboard::Manager,
                              std::unique_ptr<VirtualKeyboardManager>>>
      manager_binding_;

  // Unfulfilled configuration requests for the virtual keyboard.
  //
  // * Used to buffer configuration changes when
  //   * the requestor does not have focus, OR
  //   * there is no manager client connected.
  // * Equal to `nullopt`, except in the transient state where
  //   * `this` received a RequestTypeAndVisibility() call
  //     when there was no manager connected, and
  //   * no manager has connected since the RequestTypeAndVisibility()
  //     call.
  std::map<zx_koid_t /* requestor_view_koid */, KeyboardConfig>
      view_koid_to_pending_manager_config_;

  // The view that is currently focused.
  //
  // * This is initially `nullopt`.
  // * This will have a value after FocusDispatcher receives the first
  //   `OnFocusChange()` from Scenic.
  // * This will have a value forever thereafter.
  std::optional<zx_koid_t> focused_view_koid_;

  // Must be last, to invalidate weak pointers held by other fields before their
  // destructors are called.
  fxl::WeakPtrFactory<FidlBoundVirtualKeyboardCoordinator> weak_ptr_factory_;
};

}  // namespace virtual_keyboard_manager

#endif  // SRC_UI_BIN_VIRTUAL_KEYBOARD_MANAGER_VIRTUAL_KEYBOARD_COORDINATOR_H_
