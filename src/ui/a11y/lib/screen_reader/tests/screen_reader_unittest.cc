// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/screen_reader/screen_reader.h"

#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/macros.h>

#include <gmock/gmock.h>

#include "fuchsia/accessibility/gesture/cpp/fidl.h"
#include "src/lib/testing/loop_fixture/test_loop_fixture.h"
#include "src/ui/a11y/bin/a11y_manager/tests/util/util.h"
#include "src/ui/a11y/lib/annotation/tests/mocks/mock_annotation_view.h"
#include "src/ui/a11y/lib/focus_chain/tests/mocks/mock_focus_chain_registry.h"
#include "src/ui/a11y/lib/focus_chain/tests/mocks/mock_focus_chain_requester.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_listener_registry.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_manager.h"
#include "src/ui/a11y/lib/gesture_manager/recognizers/one_finger_drag_recognizer.h"
#include "src/ui/a11y/lib/gesture_manager/recognizers/one_finger_n_tap_recognizer.h"
#include "src/ui/a11y/lib/gesture_manager/recognizers/swipe_recognizer_base.h"
#include "src/ui/a11y/lib/gesture_manager/tests/mocks/mock_gesture_handler.h"
#include "src/ui/a11y/lib/gesture_manager/tests/mocks/mock_gesture_listener.h"
#include "src/ui/a11y/lib/screen_reader/focus/tests/mocks/mock_a11y_focus_manager.h"
#include "src/ui/a11y/lib/screen_reader/tests/mocks/mock_screen_reader_context.h"
#include "src/ui/a11y/lib/screen_reader/tests/mocks/mock_tts_engine.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantic_provider.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantic_tree.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantic_tree_service_factory.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantics_event_manager.h"
#include "src/ui/a11y/lib/testing/input.h"
#include "src/ui/a11y/lib/tts/tests/mocks/mock_tts_manager.h"
#include "src/ui/a11y/lib/tts/tts_manager.h"
#include "src/ui/a11y/lib/util/util.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_accessibility_view.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_view_injector_factory.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_view_semantics.h"
#include "src/ui/a11y/lib/view/view_manager.h"

namespace accessibility_test {
namespace {

using AccessibilityPointerEvent = fuchsia::ui::input::accessibility::PointerEvent;
using GestureType = a11y::GestureHandlerV2::GestureType;
using PointerEventPhase = fuchsia::ui::input::PointerEventPhase;
using fuchsia::accessibility::gesture::Type;
using fuchsia::accessibility::semantics::Node;
using Phase = fuchsia::ui::input::PointerEventPhase;
using testing::ElementsAre;
using testing::StrEq;

class MockScreenReaderActionRegistryImpl : public a11y::ScreenReaderActionRegistry,
                                           public a11y::ScreenReaderAction {
 public:
  MockScreenReaderActionRegistryImpl() = default;
  ~MockScreenReaderActionRegistryImpl() override = default;
  void AddAction(std::string name, std::unique_ptr<ScreenReaderAction> action) override {
    actions_.insert(std::move(name));
  }

  ScreenReaderAction* GetActionByName(const std::string& name) override {
    auto action_it = actions_.find(name);
    if (action_it == actions_.end()) {
      return nullptr;
    }
    invoked_actions_.push_back(name);
    return this;
  }

  void Run(a11y::gesture_util_v2::GestureContext gesture_context) override {}

  std::vector<std::string>& invoked_actions() { return invoked_actions_; }

 private:
  std::unordered_set<std::string> actions_;
  std::vector<std::string> invoked_actions_;
};

class ScreenReaderTest : public gtest::TestLoopFixture {
 public:
  ScreenReaderTest() = default;
  ~ScreenReaderTest() override = default;

  void InitializeScreenReader() {
    screen_reader_ = std::make_unique<a11y::ScreenReader>(
        std::move(context_), view_manager_.get(), view_manager_.get(),
        gesture_listener_registry_.get(), mock_tts_manager_.get(), announce_screen_reader_enabled_,
        std::move(mock_action_registry_));
    screen_reader_->BindGestures(mock_gesture_handler_.get());
    gesture_listener_registry_->Register(mock_gesture_listener_->NewBinding(), []() {});
  }

  void SetUp() override {
    gtest::TestLoopFixture::SetUp();

    context_provider_ = std::make_unique<sys::testing::ComponentContextProvider>();
    mock_gesture_handler_ = std::make_unique<MockGestureHandlerV2>();
    view_manager_ = std::make_unique<a11y::ViewManager>(
        std::make_unique<MockSemanticTreeServiceFactory>(),
        std::make_unique<MockViewSemanticsFactory>(), std::make_unique<MockAnnotationViewFactory>(),
        std::make_unique<MockViewInjectorFactory>(), std::make_unique<MockSemanticsEventManager>(),
        std::make_shared<MockAccessibilityView>(), context_provider_->context());
    context_ = std::make_unique<MockScreenReaderContext>();
    context_ptr_ = context_.get();
    a11y_focus_manager_ptr_ = context_ptr_->mock_a11y_focus_manager_ptr();
    mock_speaker_ptr_ = context_ptr_->mock_speaker_ptr();
    mock_action_registry_ = std::make_unique<MockScreenReaderActionRegistryImpl>();
    mock_action_registry_ptr_ = mock_action_registry_.get();
    mock_tts_manager_ = std::make_unique<MockTtsManager>(context_provider_->context());
    semantic_provider_ = std::make_unique<MockSemanticProvider>(view_manager_.get());
    gesture_manager_ = std::make_unique<a11y::GestureManager>();
    gesture_listener_registry_ = std::make_unique<a11y::GestureListenerRegistry>();
    mock_gesture_handler_ = std::make_unique<MockGestureHandlerV2>();
    mock_gesture_listener_ = std::make_unique<MockGestureListener>();
  }

  void ConnectSpeakerAndEngine() {
    // The speaker and engine need to be connected to the tts manager before the
    // screen reader announces it's on. In order to verify that the screen reader
    // correctly vocalizes, we need to expicitly connect the speaker and engine.
    fuchsia::accessibility::tts::EnginePtr engine_ptr;
    mock_tts_manager_->OpenEngine(
        engine_ptr.NewRequest(),
        [](fuchsia::accessibility::tts::TtsManager_OpenEngine_Result result) {});
    RunLoopUntilIdle();

    MockTtsEngine mock_tts_engine;
    mock_tts_manager_->RegisterEngine(
        mock_tts_engine.GetHandle(),
        [](fuchsia::accessibility::tts::EngineRegistry_RegisterEngine_Result result) {});
    RunLoopUntilIdle();
  }

  bool announce_screen_reader_enabled_ = true;
  std::unique_ptr<sys::testing::ComponentContextProvider> context_provider_;
  std::unique_ptr<a11y::ViewManager> view_manager_;
  std::unique_ptr<a11y::GestureManager> gesture_manager_;
  std::unique_ptr<a11y::GestureListenerRegistry> gesture_listener_registry_;
  std::unique_ptr<MockGestureListener> mock_gesture_listener_;
  std::unique_ptr<MockGestureHandlerV2> mock_gesture_handler_;

  std::unique_ptr<MockScreenReaderContext> context_;
  MockScreenReaderContext* context_ptr_;
  MockA11yFocusManager* a11y_focus_manager_ptr_;
  MockScreenReaderContext::MockSpeaker* mock_speaker_ptr_;
  std::unique_ptr<MockScreenReaderActionRegistryImpl> mock_action_registry_;
  MockScreenReaderActionRegistryImpl* mock_action_registry_ptr_;
  std::unique_ptr<MockTtsManager> mock_tts_manager_;
  std::unique_ptr<a11y::ScreenReader> screen_reader_;
  std::unique_ptr<MockSemanticProvider> semantic_provider_;
};  // namespace

TEST_F(ScreenReaderTest, GestureHandlersAreRegisteredIntheRightOrder) {
  InitializeScreenReader();
  // The order in which the Screen Reader registers the gesture handlers at startup is relevant.
  // Each registered handler is saved in the mock, so we can check if they are in the right order
  // here.
  EXPECT_THAT(mock_gesture_handler_->bound_gestures(),
              ElementsAre(GestureType::kThreeFingerUpSwipe, GestureType::kThreeFingerDownSwipe,
                          GestureType::kThreeFingerLeftSwipe, GestureType::kThreeFingerRightSwipe,
                          GestureType::kOneFingerDownSwipe, GestureType::kOneFingerUpSwipe,
                          GestureType::kOneFingerLeftSwipe, GestureType::kOneFingerRightSwipe,
                          GestureType::kOneFingerDoubleTap, GestureType::kOneFingerDoubleTapDrag,
                          GestureType::kOneFingerSingleTap, GestureType::kOneFingerDrag,
                          GestureType::kTwoFingerSingleTap));
}

TEST_F(ScreenReaderTest, RegisteredActionsAreInvokedWhenGestureTriggers) {
  InitializeScreenReader();
  mock_gesture_handler_->TriggerGesture(
      GestureType::kThreeFingerUpSwipe);  // corresponds to physical right.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kThreeFingerDownSwipe);  // Corresponds to physical left.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kThreeFingerLeftSwipe);  // Corresponds to physical up
  mock_gesture_handler_->TriggerGesture(
      GestureType::kThreeFingerRightSwipe);  // Corresponds to physical down.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerUpSwipe);  // Corresponds to physical right.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerDownSwipe);  // Corresponds to physical left.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerLeftSwipe);  // Corresponds to a physical up.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerRightSwipe);  // Corresponds to a physical down.
  mock_gesture_handler_->TriggerGesture(GestureType::kOneFingerDoubleTap);
  // Note that since one finger single tap and drag both trigger the explore action, we expect to
  // see it twice in the list of called actions.
  mock_gesture_handler_->TriggerGesture(GestureType::kOneFingerSingleTap);

  // Corresponds to four times the action being invoked! 2 to start a new stream, one to inject one
  // event from the stream, 1 to end the stream.
  mock_gesture_handler_->TriggerGesture(GestureType::kOneFingerDoubleTapDrag);
  mock_gesture_handler_->TriggerGesture(GestureType::kOneFingerDrag);
  RunLoopUntilIdle();
  EXPECT_THAT(
      mock_action_registry_ptr_->invoked_actions(),
      ElementsAre(StrEq("Three finger Right Swipe Action"), StrEq("Three finger Left Swipe Action"),
                  StrEq("Three finger Up Swipe Action"), StrEq("Three finger Down Swipe Action"),
                  StrEq("Next Action"), StrEq("Previous Action"),
                  StrEq("Previous Semantic Level Action"), StrEq("Next Semantic Level Action"),
                  StrEq("Default Action"), StrEq("Explore Action"),
                  StrEq("Inject Pointer Event Action"), StrEq("Inject Pointer Event Action"),
                  StrEq("Inject Pointer Event Action"), StrEq("Inject Pointer Event Action"),
                  StrEq("Explore Action")));
}

TEST_F(ScreenReaderTest, ScreenReaderResetsSemanticLevelBeforeExploreAction) {
  InitializeScreenReader();

  // Set semantic level to something other than kDefault.
  context_ptr_->set_semantic_level(a11y::ScreenReaderContext::SemanticLevel::kAdjustValue);

  // Perform a single-tap. The screen reader should change the semantic level
  // back to kDefault before running the action.
  mock_gesture_handler_->TriggerGesture(GestureType::kOneFingerSingleTap);

  // Verify that the semantic level was reset to kDefault.
  EXPECT_EQ(context_ptr_->semantic_level(), a11y::ScreenReaderContext::SemanticLevel::kDefault);

  // Repeat the above steps for the one-finger drag gesture.
  context_ptr_->set_semantic_level(a11y::ScreenReaderContext::SemanticLevel::kAdjustValue);
  mock_gesture_handler_->TriggerGesture(GestureType::kOneFingerDrag);
  EXPECT_EQ(context_ptr_->semantic_level(), a11y::ScreenReaderContext::SemanticLevel::kDefault);
}

TEST_F(ScreenReaderTest, TrivialActionsAreInvokedWhenGestureTriggers) {
  InitializeScreenReader();
  // Trivial actions are not registered in the action registry, but are jusst the callback parked at
  // the gesture handler. Verify that the results of the callback are seen when it runs.
  mock_gesture_handler_->TriggerGesture(GestureType::kTwoFingerSingleTap);
  EXPECT_TRUE(mock_speaker_ptr_->ReceivedCancel());
}

TEST_F(ScreenReaderTest, ScreenReaderSpeaksWhenItTurnsOnAndOff) {
  InitializeScreenReader();
  // No output should be spoken until the tts engine is connected.
  EXPECT_TRUE(mock_speaker_ptr_->message_ids().empty());

  // The screen reader will not announce it's on until the speaker and engine
  // are connected.
  ConnectSpeakerAndEngine();

  // The screen reader object has already been initialized, check if it announced it:
  EXPECT_EQ(mock_speaker_ptr_->message_ids().size(), 1u);
  EXPECT_EQ(mock_speaker_ptr_->message_ids()[0],
            fuchsia::intl::l10n::MessageIds ::SCREEN_READER_ON_HINT);
  // Because the Screen Reader owns the speaker, when it is destroyed, so is the speaker.
  // This callback makes sure that we have the chance to take a last look at the speaker before it
  // goes out of scope.
  bool callback_ran = false;
  MockScreenReaderContext::MockSpeaker::OnDestructionCallback callback =
      [&callback_ran](MockScreenReaderContext::MockSpeaker* speaker) {
        callback_ran = true;
        EXPECT_EQ(speaker->epitaph(), fuchsia::intl::l10n::MessageIds ::SCREEN_READER_OFF_HINT);
      };
  mock_speaker_ptr_->set_on_destruction_callback(std::move(callback));
  screen_reader_.reset();
  EXPECT_TRUE(callback_ran);
}

TEST_F(ScreenReaderTest, ScreenReaderSpeaksWhenInitializedAfterEngineAndSpeakerConnected) {
  // The screen reader will not announce it's on until the speaker and engine
  // are connected.
  ConnectSpeakerAndEngine();

  // When the screen reader's destructor was called above
  // (via sceen_reader_.reset()), screen_reader_'s callback should have been
  // unregistered from the tts manager. If not, the tts manager would have
  // invoked the stale callback. This check ensures that the unregistration was
  // handled correctly.
  EXPECT_TRUE(mock_speaker_ptr_->message_ids().empty());

  InitializeScreenReader();

  EXPECT_EQ(mock_speaker_ptr_->message_ids().size(), 1u);
}

TEST_F(ScreenReaderTest, NextOrPreviousActionInvokesActionsBasedOnTheSemanticLevel) {
  InitializeScreenReader();

  // This test makes sure that when the next / previous action is invoked, bound to right / left one
  // finger swipes, it corresponds to the appropriate action to the current semantic level.
  EXPECT_EQ(context_ptr_->semantic_level(), a11y::ScreenReaderContext::SemanticLevel::kDefault);
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerUpSwipe);  // Corresponds to physical right.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerDownSwipe);  // Corresponds to physical left.
  context_ptr_->set_semantic_level(a11y::ScreenReaderContext::SemanticLevel::kAdjustValue);
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerUpSwipe);  // Corresponds to physical right.
  mock_gesture_handler_->TriggerGesture(
      GestureType::kOneFingerDownSwipe);  // Corresponds to physical left.
  EXPECT_THAT(
      mock_action_registry_ptr_->invoked_actions(),
      ElementsAre(StrEq("Next Action"), StrEq("Previous Action"),
                  StrEq("Increment Range Value Action"), StrEq("Decrement Range Value Action")));
}

TEST_F(ScreenReaderTest, SemanticEventsTriggerScreenReaderAction) {
  InitializeScreenReader();

  view_manager_->GetSemanticsEventManager()->Register(
      screen_reader_->GetSemanticsEventListenerWeakPtr());
  view_manager_->GetSemanticsEventManager()->OnEvent(
      {.event_type = a11y::SemanticsEventType::kSemanticTreeUpdated});
  EXPECT_THAT(mock_action_registry_ptr_->invoked_actions(),
              ElementsAre(StrEq("Recover A11Y Focus Action"), StrEq("Process Update Action")));
}

TEST_F(ScreenReaderTest, SemanticEventAnnounceCausesScreenReaderToSpeak) {
  InitializeScreenReader();

  view_manager_->GetSemanticsEventManager()->Register(
      screen_reader_->GetSemanticsEventListenerWeakPtr());
  a11y::SemanticsEventInfo event_info;
  fuchsia::accessibility::semantics::AnnounceEvent announce;
  announce.set_message("hello world");
  fuchsia::accessibility::semantics::SemanticEvent semantic_event;
  semantic_event.set_announce(std::move(announce));
  event_info.semantic_event = std::move(semantic_event);
  view_manager_->GetSemanticsEventManager()->OnEvent(std::move(event_info));

  RunLoopUntilIdle();
  EXPECT_EQ(mock_speaker_ptr_->messages().size(), 1u);
  EXPECT_EQ(mock_speaker_ptr_->messages()[0], "hello world");
}

TEST_F(ScreenReaderTest, ScreenReaderSilentWhenSpecifiedDuringInit) {
  announce_screen_reader_enabled_ = false;
  InitializeScreenReader();

  // No output should be spoken until the tts engine is connected.
  EXPECT_TRUE(mock_speaker_ptr_->message_ids().empty());

  ConnectSpeakerAndEngine();

  // No output should be spoken since reboot was not user-initiated.
  EXPECT_TRUE(mock_speaker_ptr_->message_ids().empty());
}

}  // namespace
}  // namespace accessibility_test
