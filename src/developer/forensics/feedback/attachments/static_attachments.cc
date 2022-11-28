// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback/attachments/static_attachments.h"

#include <lib/syslog/cpp/macros.h>

#include <functional>
#include <string>

#include "src/developer/forensics/feedback/attachments/types.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/lib/files/file.h"

namespace forensics::feedback {
namespace {

AttachmentValue FromFile(const std::string& filepath) {
  if (std::string content; files::ReadFileToString(filepath, &content)) {
    return content.empty() ? AttachmentValue(Error::kMissingValue)
                           : AttachmentValue(std::move(content));
  }

  FX_LOGS(WARNING) << "Failed to read: " << filepath;
  return AttachmentValue(Error::kFileReadFailure);
}

}  // namespace

Attachments GetStaticAttachments() {
  AttachmentValue build_snapshot = FromFile("/config/build-info/snapshot");
  if (!build_snapshot.HasValue()) {
    FX_LOGS(WARNING) << "Failed to build attachment " << feedback_data::kAttachmentBuildSnapshot;
  }

  Attachments attachments;
  attachments.insert({feedback_data::kAttachmentBuildSnapshot, std::move(build_snapshot)});

  return attachments;
}

}  // namespace forensics::feedback
