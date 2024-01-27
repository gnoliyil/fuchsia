// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_MANIFEST_PARSER_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_MANIFEST_PARSER_H_

#include <lib/zx/result.h>

#include <string>

#include "src/devices/lib/log/log.h"

// Get the path to the base directory of a package.
zx::result<std::string> GetBasePathFromUrl(const std::string& url);

// Get the full path to a file within a package.
// E.g: fuchsia-pkg://fuchsia.com/my-package#driver/my-driver.so
//      will return the full path to the my-driver.so file.
zx::result<std::string> GetPathFromUrl(const std::string& url);

// Returns true if url starts with 'fuchsia-boot://'
bool IsFuchsiaBootScheme(std::string_view url);

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_MANIFEST_PARSER_H_
