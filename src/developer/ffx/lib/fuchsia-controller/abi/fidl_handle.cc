// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl_handle.h"

#include "mod.h"

namespace fidl_handle {

void FidlHandle_dealloc(FidlHandle *self) {
  ffx_close_handle(self->handle);
  Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

int FidlHandle_init(FidlHandle *self, PyObject *args, PyObject *kwds) {
  static const char *kwlist[] = {"handle", nullptr};
  zx_handle_t handle;
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "I", const_cast<char **>(kwlist), &handle)) {
    return -1;
  }
  handle = RawHandleFromU64Checked(handle);
  if (handle == static_cast<zx_handle_t>(-1) && PyErr_Occurred()) {
    return -1;
  }
  self->handle = handle;
  return 0;
}

DES_MIX PyTypeObject FidlHandleType = {
    PyVarObject_HEAD_INIT(nullptr, 0)

        .tp_name = "fuchsia_controller_py.FidlHandle",
    .tp_basicsize = sizeof(FidlHandle),
    .tp_itemsize = 0,
    .tp_dealloc = reinterpret_cast<destructor>(FidlHandle_dealloc),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Fuchsia controller FIDL handle. This is used to bootstrap processes for FIDL interactions.",
    .tp_init = reinterpret_cast<initproc>(FidlHandle_init),
    .tp_new = PyType_GenericNew,
};

}  // namespace fidl_handle
