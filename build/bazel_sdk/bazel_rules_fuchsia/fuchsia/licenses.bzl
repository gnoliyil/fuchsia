# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Public definitions for licenses related rules."""

load(
    "@rules_fuchsia//fuchsia/private/licenses:fuchsia_licenses_classification.bzl",
    _fuchsia_licenses_classification = "fuchsia_licenses_classification",
)
load(
    "@rules_fuchsia//fuchsia/private/licenses:fuchsia_licenses_notice.bzl",
    _fuchsia_licenses_notice = "fuchsia_licenses_notice",
)
load(
    "@rules_fuchsia//fuchsia/private/licenses:fuchsia_licenses_review.bzl",
    _fuchsia_licenses_review = "fuchsia_licenses_review",
)
load(
    "@rules_fuchsia//fuchsia/private/licenses:fuchsia_licenses_spdx.bzl",
    _fuchsia_licenses_spdx = "fuchsia_licenses_spdx",
)

fuchsia_licenses_classification = _fuchsia_licenses_classification
fuchsia_licenses_notice = _fuchsia_licenses_notice
fuchsia_licenses_review = _fuchsia_licenses_review
fuchsia_licenses_spdx = _fuchsia_licenses_spdx
