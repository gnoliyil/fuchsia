// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/driver/component/cpp/driver_base.h>

namespace fragment_irq {

// Get an interrupt with name |instance_name|.
zx::result<zx::interrupt> GetInterrupt(const fdf::Namespace& ns, std::string_view instance_name);

// Get interrupt index |which|. This will attempt to use fragments and FIDL to get the
// interrupt.
zx::result<zx::interrupt> GetInterrupt(const fdf::Namespace& ns, uint32_t which);

}  // namespace fragment_irq
