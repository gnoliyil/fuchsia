// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_H_

#include <memory>
#include <string>
#include <unordered_map>

#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_handler_v2.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_listener_registry.h"
#include "src/ui/a11y/lib/screen_reader/i18n/messages.h"
#include "src/ui/a11y/lib/screen_reader/screen_reader_action.h"
#include "src/ui/a11y/lib/screen_reader/screen_reader_context.h"
#include "src/ui/a11y/lib/semantics/semantics_event_listener.h"
#include "src/ui/a11y/lib/tts/tts_manager.h"
#include "src/ui/a11y/lib/util/boot_info_manager.h"

namespace a11y {

// The Fuchsia Screen Reader.
//
// This is the base class for the Fuchsia Screen Reader. It connects to all
// services necessary to make a funcional Screen Reader.
//
// A common loop would be something like:
//   User performes some sort of input (via touch screen for example). The input
//   triggers an Screen Reader action, which then calls the Fuchsia
//   Accessibility APIs. Finally, some output is communicated (via speech, for
//   example).
class ScreenReader : public SemanticsEventListener {
 public:
  // Pointers to Semantics Manager, Gesture Listener Registry and Gesture Manager must
  // outlive screen reader. A11y App is responsible for creating these pointers along with Screen
  // Reader object.
  ScreenReader(std::unique_ptr<ScreenReaderContext> context, SemanticsSource* semantics_source,
               InjectorManagerInterface* injector_manager,
               GestureListenerRegistry* gesture_listener_registry, TtsManager* tts_manager,
               bool announce_screen_reader_enabled);
  // Same as above, but accepts a custom |action_registry|.
  ScreenReader(std::unique_ptr<ScreenReaderContext> context, SemanticsSource* semantics_source,
               InjectorManagerInterface* injector_manager,
               GestureListenerRegistry* gesture_listener_registry, TtsManager* tts_manager,
               bool announce_screen_reader_enabled,
               std::unique_ptr<ScreenReaderActionRegistry> action_registry);

  ~ScreenReader();

  void BindGestures(a11y::GestureHandlerV2* gesture_handler);

  ScreenReaderContext* context() { return context_.get(); }

  // Returns a Semantics Event Listener managed by the Screen Reader.
  fxl::WeakPtr<SemanticsEventListener> GetSemanticsEventListenerWeakPtr();

 private:
  class ScreenReaderActionRegistryImpl;

  // |SemanticsEventListener|
  void OnEvent(SemanticsEventInfo event_info) override;

  void InitializeActions();

  // Helps finding the appropriate Action based on Action Name and calls Run()
  // for the matched Action.
  // Functions returns false, if no action matches the provided "action_name",
  // returns true if Run() is called.
  bool ExecuteAction(const std::string& action_name,
                     a11y::gesture_util_v2::GestureContext gesture_context);

  // Speaks the message represented by |message_id|.
  void SpeakMessage(fuchsia::intl::l10n::MessageIds message_id);
  void SpeakMessage(const std::string& message);

  // The Screen Reader can simulate a touch screen tap down, an optional sequence of injected moves,
  // followed by a tap up event to directly interact with runtimes.
  void SimulateTapDown(a11y::gesture_util_v2::GestureContext context);
  void SimulateTapUp(a11y::gesture_util_v2::GestureContext context);

  // Stores information about the Screen Reader state.
  std::unique_ptr<ScreenReaderContext> context_;

  // Stores Action context which is required to build an Action.
  std::unique_ptr<ScreenReaderAction::ActionContext> action_context_;

  // Pointer to Gesture Listener Registry.
  GestureListenerRegistry* gesture_listener_registry_;

  // Pointer to TTS manager.
  TtsManager* tts_manager_;

  // Maps action names to screen reader actions.
  // Different triggering methods may invoke the same action. For example, both one finger tap and
  // dragging the finger on the screen invoke the explore action.
  std::unique_ptr<ScreenReaderActionRegistry> action_registry_;

  fxl::WeakPtrFactory<SemanticsEventListener> weak_ptr_factory_;
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_SCREEN_READER_H_
