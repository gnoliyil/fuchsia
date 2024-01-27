// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_MISC_DRIVERS_COMPAT_LOADER_H_
#define SRC_DEVICES_MISC_DRIVERS_COMPAT_LOADER_H_

#include <fidl/fuchsia.ldsvc/cpp/wire.h>

namespace compat {

constexpr char kLibDriverName[] = "libdriver.so";

// Loader is a loader service that is used to override the DFv1 driver library
// with an alternative implementation.
//
// For most requests, it passes them along to a backing loader service, however
// if the DFv1 driver library is requested, it will return the compatibility
// driver's VMO.
class Loader : public fidl::WireServer<fuchsia_ldsvc::Loader> {
 public:
  Loader(async_dispatcher_t* dispatcher, fidl::UnownedClientEnd<fuchsia_ldsvc::Loader> loader,
         zx::vmo driver_vmo);

 private:
  // fidl::WireServer<fuchsia_ldsvc::Loader>
  void Done(DoneCompleter::Sync& completer) override;
  void LoadObject(LoadObjectRequestView request, LoadObjectCompleter::Sync& completer) override;
  void Config(ConfigRequestView request, ConfigCompleter::Sync& completer) override;
  void Clone(CloneRequestView request, CloneCompleter::Sync& completer) override;

  async_dispatcher_t* dispatcher_;
  fidl::UnownedClientEnd<fuchsia_ldsvc::Loader> client_;
  zx::vmo driver_vmo_;
};

}  // namespace compat

#endif  // SRC_DEVICES_MISC_DRIVERS_COMPAT_LOADER_H_
