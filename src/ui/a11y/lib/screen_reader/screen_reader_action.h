// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_ACTION_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_ACTION_H_

#include <fuchsia/accessibility/tts/cpp/fidl.h>
#include <fuchsia/ui/input/accessibility/cpp/fidl.h>
#include <lib/fpromise/promise.h>

#include "src/ui/a11y/lib/gesture_manager/gesture_util_v2/util.h"
#include "src/ui/a11y/lib/input_injection/injector_manager.h"
#include "src/ui/a11y/lib/screen_reader/screen_reader_context.h"
#include "src/ui/a11y/lib/screen_reader/speaker.h"
#include "src/ui/a11y/lib/semantics/semantics_source.h"
#include "src/ui/a11y/lib/view/view_manager.h"

namespace a11y {

// Base class to implement Screen Reader actions.
//
// This is the base class in which all Screen Reader actions depend upon. An
// action is bound to an input (gesture, keyboard shortcut, braille display
// keys, etc), and is triggered whenever that input happens. An action may call
// the Fuchsia Accessibility APIs and / or produce some type of output (Tts, for
// example). This is achieved by accessing information available to this action
// through the context, which is passed in the constructor.
class ScreenReaderAction {
 public:
  // Struct to hold pointers to various services, which will be required to
  // complete an action.
  struct ActionContext {
    a11y::SemanticsSource* semantics_source;
    a11y::InjectorManagerInterface* injector_manager;
  };

  explicit ScreenReaderAction(ActionContext* context, ScreenReaderContext* screen_reader_context);
  virtual ~ScreenReaderAction();

  // Action implementations override this method with the necessary method parameters to perform
  // that action.
  virtual void Run(a11y::gesture_util_v2::GestureContext gesture_context) = 0;

 protected:
  // Constructor for mocks.
  ScreenReaderAction() = default;

  // Helper function to call hit testing based on ActionContext and
  // GestureContext.
  void ExecuteHitTesting(
      ActionContext* context, a11y::gesture_util_v2::GestureContext gesture_context,
      fuchsia::accessibility::semantics::SemanticListener::HitTestCallback callback);

  // Returns a promise that executes an accessibility action targeting the semantic tree
  // corresponding to |view_ref_koid|, on the node |node_id|. An error is thrown if the semantic
  // tree can't be found or if the semantic provider did not handle this action.
  fpromise::promise<> ExecuteAccessibilityActionPromise(
      zx_koid_t view_ref_koid, uint32_t node_id, fuchsia::accessibility::semantics::Action action);

  // Returns a promise that sets a new A11y Focus. If the operation is not successful, throws an
  // error.
  fpromise::promise<> SetA11yFocusPromise(zx_koid_t view_koid, uint32_t node_id);

  // Returns a promise that from a node_id and view_koid, builds a speech task to speak the node
  // description. An error is thrown if the semantic tree or the semantic node are missing data
  // necessary to build an utterance.
  fpromise::promise<> BuildSpeechTaskFromNodePromise(zx_koid_t view_koid, uint32_t node_id,
                                                     Speaker::Options options = {
                                                         .interrupt = true});

  // Updates the current and previous navigation contexts based on the newly focused node.
  void UpdateNavigationContext(zx_koid_t newly_focused_view_koid, uint32_t newly_focused_node_id);

  // Gets the message context for the currently focused node.
  ScreenReaderMessageGenerator::ScreenReaderMessageContext GetMessageContext();

  // ActionContext which is used to make calls to Semantics Manager and TTS.
  ActionContext* action_context_;

  // Pointer to the screen reader context, which owns the executor used by this class.
  ScreenReaderContext* screen_reader_context_;
};

// An interface to retrieeve actions.
class ScreenReaderActionRegistry {
 public:
  ScreenReaderActionRegistry() = default;
  virtual ~ScreenReaderActionRegistry() = default;

  // Adds an |action| with |name| to the registry. |name| can be later used to retrieeve this
  // action.
  virtual void AddAction(std::string name, std::unique_ptr<ScreenReaderAction> action) = 0;

  // Returns the action registered with |name|, nullptr if not found.
  virtual ScreenReaderAction* GetActionByName(const std::string& name) = 0;
};

}  // namespace a11y
#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_ACTION_H_
