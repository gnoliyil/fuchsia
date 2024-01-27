// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/driver.h>

extern struct zx_driver_ops msd_driver_ops;

ZIRCON_DRIVER(magma_pdev_gpu, msd_driver_ops, "zircon", "0.1");
