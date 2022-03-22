// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test-surface.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <zircon/syscalls.h>

#define TRACE 0

static void gfx_log(const char* format, ...) {
  if (TRACE) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
  }
}

static void gfx_panic(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  assert(0);
}

static void gfx_flush_cache(void* ptr, size_t size) {
  zx_cache_flush(ptr, size, ZX_CACHE_FLUSH_DATA);
}

static const gfx::Context g_ctx = {
    .log = gfx_log,
    .panic = gfx_panic,
    .flush_cache = gfx_flush_cache,
};

// Create a new graphics surface object
gfx::Surface* CreateTestSurface(void* ptr, unsigned width, unsigned height, unsigned stride,
                                unsigned format, uint32_t flags) {
  return gfx::CreateSurfaceWithContext(ptr, &g_ctx, width, height, stride, format, flags);
}
