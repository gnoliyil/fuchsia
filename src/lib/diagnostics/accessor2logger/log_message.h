// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_DIAGNOSTICS_ACCESSOR2LOGGER_LOG_MESSAGE_H_
#define SRC_LIB_DIAGNOSTICS_ACCESSOR2LOGGER_LOG_MESSAGE_H_

#include <fuchsia/diagnostics/cpp/fidl.h>
#include <fuchsia/logger/cpp/fidl.h>
#include <lib/fpromise/result.h>

#include <vector>

namespace diagnostics::accessor2logger {

// Prints formatted content to the log.
fpromise::result<std::vector<fpromise::result<fuchsia::logger::LogMessage, std::string>>,
                 std::string>
ConvertFormattedContentToLogMessages(fuchsia::diagnostics::FormattedContent content);

}  // namespace diagnostics::accessor2logger

#endif  // SRC_LIB_DIAGNOSTICS_ACCESSOR2LOGGER_LOG_MESSAGE_H_
