// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_DATA_PROVIDER_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_DATA_PROVIDER_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/async/dispatcher.h>
#include <lib/fpromise/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/vfs/cpp/vmo_file.h>

#include <functional>
#include <map>
#include <memory>

#include "src/developer/forensics/feedback/annotations/annotation_manager.h"
#include "src/developer/forensics/feedback/annotations/metrics.h"
#include "src/developer/forensics/feedback/attachments/attachment_manager.h"
#include "src/developer/forensics/feedback/attachments/metrics.h"
#include "src/developer/forensics/feedback_data/inspect_data_budget.h"
#include "src/developer/forensics/feedback_data/metadata.h"
#include "src/developer/forensics/feedback_data/screenshot.h"
#include "src/developer/forensics/utils/cobalt/logger.h"
#include "src/developer/forensics/utils/redact/redactor.h"
#include "src/lib/fsl/vmo/sized_vmo.h"
#include "src/lib/timekeeper/clock.h"
#include "src/lib/timekeeper/system_clock.h"

namespace forensics {
namespace feedback_data {

// Serves snapshot archive through a channel using |fuchsia.io.File|
class ServedArchive {
 public:
  explicit ServedArchive(fsl::SizedVmo data);
  bool Serve(zx::channel server_end, async_dispatcher_t* dispatcher,
             std::function<void()> completed);

 private:
  vfs::VmoFile file_;
  std::unique_ptr<async::WaitOnce> channel_closed_observer_ = nullptr;
};

// Internal-only class for collecting snapshots that avoids converting data to FIDL domain objects.
// Should be used by Feedback itself because the conversion to FIDL is lossy and information on why
// data is missing, like annotations, is dropped.
class DataProviderInternal {
 public:
  virtual ~DataProviderInternal() = default;

  using GetSnapshotInternalCallback =
      fit::callback<void(feedback::Annotations, fuchsia::feedback::Attachment)>;
  virtual void GetSnapshotInternal(zx::duration timeout, GetSnapshotInternalCallback callback) = 0;
};

// Provides data useful to attach in feedback reports (crash, user feedback or bug reports).
class DataProvider : public fuchsia::feedback::DataProvider, public DataProviderInternal {
 public:
  DataProvider(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
               timekeeper::Clock* clock, RedactorBase* redactor, bool is_first_instance,
               const std::set<std::string>& annotation_allowlist,
               const feedback::AttachmentKeys& attachment_allowlist, cobalt::Logger* cobalt,
               feedback::AnnotationManager* annotation_manager,
               feedback::AttachmentManager* attachment_manager,
               InspectDataBudget* inspect_data_budget);

  // |fuchsia::feedback::DataProvider|
  void GetAnnotations(fuchsia::feedback::GetAnnotationsParameters params,
                      GetAnnotationsCallback callback) override;
  void GetSnapshot(fuchsia::feedback::GetSnapshotParameters params,
                   GetSnapshotCallback callback) override;
  void GetSnapshotInternal(zx::duration timeout, GetSnapshotInternalCallback callback) override;
  void GetScreenshot(fuchsia::feedback::ImageEncoding encoding,
                     GetScreenshotCallback callback) override;

  size_t NumCurrentServedArchives() { return served_archives_.size(); }

 private:
  ::fpromise::promise<feedback::Annotations> GetAnnotations(const zx::duration timeout);
  ::fpromise::promise<feedback::Attachments> GetAttachments(const zx::duration timeout);
  void GetSnapshotInternal(zx::duration timeout,
                           fit::callback<void(feedback::Annotations, fsl::SizedVmo)> callback);

  bool ServeArchive(fsl::SizedVmo archive, zx::channel server_end);

  async_dispatcher_t* dispatcher_;
  std::shared_ptr<sys::ServiceDirectory> services_;
  Metadata metadata_;
  cobalt::Logger* cobalt_;

  feedback::AnnotationManager* annotation_manager_;
  feedback::AnnotationMetrics annotation_metrics_;

  feedback::AttachmentManager* attachment_manager_;
  feedback::AttachmentMetrics attachment_metrics_;

  async::Executor executor_;

  InspectDataBudget* inspect_data_budget_;

  std::map<size_t, std::unique_ptr<ServedArchive>> served_archives_;
  size_t next_served_archive_index_ = 0;
};

}  // namespace feedback_data
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_DATA_PROVIDER_H_
