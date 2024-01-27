// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "codec_isolate.h"

#include <fuchsia/component/cpp/fidl.h>
#include <fuchsia/component/decl/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <inttypes.h>
#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>

#include <utility>

#include "src/lib/fxl/strings/string_printf.h"

std::string GetCollectionFromIsolate(IsolateType type) {
  switch (type) {
    case IsolateType::kSw:
      return "sw-codecs";
    case IsolateType::kMagma:
      return "magma-codecs";
    default:
      return "";
  }
}

void ForwardToIsolate(std::string component_url, IsolateType type,
                      sys::ComponentContext* component_context,
                      fit::function<void(fuchsia::mediacodec::CodecFactoryPtr)> connect_func,
                      fit::function<void(void)> failure_func) {
  auto failure_defer = fit::defer_callback(std::move(failure_func));
  fuchsia::component::decl::Child isolate;
  uint64_t rand_num;
  zx_cprng_draw(&rand_num, sizeof rand_num);
  std::string msg = fxl::StringPrintf("isolate-%" PRIu64 "", rand_num);
  isolate.set_name(msg);
  isolate.set_url(std::move(component_url));
  isolate.set_startup(fuchsia::component::decl::StartupMode::LAZY);
  isolate.set_on_terminate(fuchsia::component::decl::OnTerminate::NONE);

  fuchsia::component::decl::CollectionRef collection{.name = GetCollectionFromIsolate(type)};
  fuchsia::component::RealmPtr realm_svc;

  fuchsia::component::CreateChildArgs child_args;
  child_args.set_numbered_handles(std::vector<fuchsia::process::HandleInfo>());

  component_context->svc()->Connect(realm_svc.NewRequest());
  realm_svc.set_error_handler([](zx_status_t err) {
    FX_LOGS(WARNING) << "FIDL error using fuchsia.component.Realm protocol: " << err;
  });

  realm_svc->CreateChild(
      collection, std::move(isolate), std::move(child_args),
      [collection, realm_svc = std::move(realm_svc), connect_func = std::move(connect_func),
       failure_defer = std::move(failure_defer),
       msg = std::move(msg)](fuchsia::component::Realm_CreateChild_Result res) mutable {
        if (res.is_err()) {
          FX_LOGS(WARNING) << "Isolate creation request failed for " << msg;
          return;
        }
        fidl::InterfaceHandle<fuchsia::io::Directory> exposed_dir;

        fuchsia::component::decl::ChildRef child = fuchsia::component::decl::ChildRef{
            .name = msg,
            .collection = collection.name,
        };
        realm_svc->OpenExposedDir(
            child, exposed_dir.NewRequest(),
            [realm_svc = std::move(realm_svc), exposed_dir = std::move(exposed_dir),
             connect_func = std::move(connect_func), failure_defer = std::move(failure_defer)](
                fuchsia::component::Realm_OpenExposedDir_Result res) mutable {
              if (res.is_err()) {
                FX_LOGS(WARNING) << "OpenExposedDir on isolate failed";
                return;
              }

              fuchsia::mediacodec::CodecFactoryPtr factory_delegate;
              auto delegate_req = factory_delegate.NewRequest();
              sys::ServiceDirectory child_services(std::move(exposed_dir));
              zx_status_t connect_res =
                  child_services.Connect(std::move(delegate_req),
                                         // TODO(dustingreen): Might be helpful (for debugging
                                         // maybe) to change this name to distinguish these delegate
                                         // CodecFactory(s) from the main CodecFactory service.
                                         fuchsia::mediacodec::CodecFactory::Name_);
              if (connect_res == ZX_OK) {
                connect_func(std::move(factory_delegate));
                failure_defer.cancel();
              } else {
                FX_LOGS(WARNING) << "Connection to isolate services failed with error code: "
                                 << connect_res;
              }
            });
      });
}
