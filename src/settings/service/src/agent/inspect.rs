// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Agent for capturing setting values of messages between proxies and setting
/// handlers.
pub(crate) mod setting_values;

/// Agent for writing the recent request payloads to inspect.
pub(crate) mod setting_proxy;

/// Agent for writing api usage counts.
pub(crate) mod usage_counts;

/// Agent for writing external api calls and responses to inspect.
pub(crate) mod external_apis;
