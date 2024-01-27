// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/gesture_manager/recognizers_v2/any_recognizer.h"

namespace a11y::recognizers_v2 {

void AnyRecognizer::HandleEvent(
    const fuchsia::ui::input::accessibility::PointerEvent& pointer_event) {}

void AnyRecognizer::OnContestStarted(std::unique_ptr<ContestMember> contest_member) {
  contest_member->Accept();
}

std::string AnyRecognizer::DebugName() const { return "any"; }

}  // namespace a11y::recognizers_v2
