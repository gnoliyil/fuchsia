// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/bringup/bin/netsvc/args.h"

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <lib/service/llcpp/service.h>

#include <cstring>

namespace {
int ParseCommonArgs(int argc, char** argv, const char** error, std::string* interface) {
  while (argc > 1) {
    if (!strncmp(argv[1], "--interface", 11)) {
      if (argc < 3) {
        *error = "netsvc: missing argument to --interface";
        return -1;
      }
      *interface = argv[2];
      argv++;
      argc--;
    }
    argv++;
    argc--;
  }
  return 0;
}
}  // namespace

int ParseArgs(int argc, char** argv, fidl::UnownedClientEnd<fuchsia_io::Directory> svc_dir,
              const char** error, NetsvcArgs* out) {
  // Reset the args.
  *out = NetsvcArgs();

  // First parse from kernel args, then use use cmdline args as overrides.
  zx::status client_end = service::ConnectAt<fuchsia_boot::Arguments>(svc_dir);
  if (client_end.is_error()) {
    *error = "netsvc: unable to connect to fuchsia.boot.Arguments";
    return -1;
  }

  fidl::WireSyncClient client = fidl::BindSyncClient(std::move(client_end.value()));
  fidl::WireResult string_resp = client->GetString(fidl::StringView{"netsvc.interface"});
  if (string_resp.ok()) {
    auto& value = string_resp->value;
    out->interface = std::string{value.data(), value.size()};
  }

  fuchsia_boot::wire::BoolPair bool_keys[]{
      {fidl::StringView{"netsvc.disable"}, true},
      {fidl::StringView{"netsvc.netboot"}, false},
      {fidl::StringView{"netsvc.advertise"}, true},
      {fidl::StringView{"netsvc.all-features"}, false},
  };

  fidl::WireResult bool_resp =
      client->GetBools(fidl::VectorView<fuchsia_boot::wire::BoolPair>::FromExternal(bool_keys));
  if (bool_resp.ok()) {
    out->disable = bool_resp->values[0];
    out->netboot = bool_resp->values[1];
    out->advertise = bool_resp->values[2];
    out->all_features = bool_resp->values[3];
  }

  int err = ParseCommonArgs(argc, argv, error, &out->interface);
  if (err) {
    return err;
  }
  while (argc > 1) {
    if (!strncmp(argv[1], "--netboot", 9)) {
      out->netboot = true;
    } else if (!strncmp(argv[1], "--nodename", 10)) {
      out->print_nodename_and_exit = true;
    } else if (!strncmp(argv[1], "--advertise", 11)) {
      out->advertise = true;
    } else if (!strncmp(argv[1], "--all-features", 14)) {
      out->all_features = true;
    }
    argv++;
    argc--;
  }
  return 0;
}
