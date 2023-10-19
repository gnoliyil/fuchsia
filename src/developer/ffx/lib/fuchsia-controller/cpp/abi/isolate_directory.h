// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_DEVELOPER_FFX_LIB_FUCHSIA_CONTROLLER_CPP_ABI_ISOLATE_DIRECTORY_H_
#define SRC_DEVELOPER_FFX_LIB_FUCHSIA_CONTROLLER_CPP_ABI_ISOLATE_DIRECTORY_H_
#include <filesystem>

#include "src/developer/ffx/lib/fuchsia-controller/cpp/abi/macros.h"
#include "src/developer/ffx/lib/fuchsia-controller/cpp/python/py_header.h"

namespace isolate {

extern PyTypeObject *IsolateDirType;

int IsolateDirTypeInit();

IGNORE_EXTRA_SC
using IsolateDir = struct {
  PyObject_HEAD;
  std::filesystem::path dir;
};

}  // namespace isolate
#endif  // SRC_DEVELOPER_FFX_LIB_FUCHSIA_CONTROLLER_CPP_ABI_ISOLATE_DIRECTORY_H_
