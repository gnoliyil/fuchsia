// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_BRINGUP_BIN_MISCSVC_SYSMEM_H_
#define SRC_BRINGUP_BIN_MISCSVC_SYSMEM_H_

#include <lib/svc/service.h>

const zx_service_provider_t* sysmem2_get_service_provider();

#endif  // SRC_BRINGUP_BIN_MISCSVC_SYSMEM_H_
