// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/types.h>

bool test_prep_and_segv();

void test_segv_pc(zx_handle_t thread);

void test_memory_ops(zx_handle_t inferior, zx_handle_t thread);

void fix_inferior_segv(zx_handle_t thread, const char* what);
