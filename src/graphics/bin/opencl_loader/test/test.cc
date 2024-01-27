// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/gpu/magma/cpp/fidl.h>
#include <fuchsia/io/cpp/fidl.h>
#include <fuchsia/opencl/loader/cpp/fidl.h>
#include <fuchsia/sys2/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/namespace.h>
#include <lib/fit/defer.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <filesystem>

#include <gtest/gtest.h>

// This is the first and only ICD loaded, so it should have a "0-" prepended.
const char* kIcdFilename = "0-libopencl_fake.so";

zx_status_t ForceWaitForIdle(fuchsia::opencl::loader::LoaderSyncPtr& loader) {
  zx::vmo vmo;
  // manifest.json remaps this to bin/opencl-server.
  return loader->Get(kIcdFilename, &vmo);
}

TEST(OpenclLoader, ManifestLoad) {
  fuchsia::opencl::loader::LoaderSyncPtr loader;
  EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.opencl.loader.Loader",
                                        loader.NewRequest().TakeChannel().release()));

  zx::vmo vmo_out;
  // manifest.json remaps this to bin/opencl-server.
  EXPECT_EQ(ZX_OK, loader->Get(kIcdFilename, &vmo_out));
  EXPECT_TRUE(vmo_out.is_valid());
  zx_info_handle_basic_t handle_info;
  EXPECT_EQ(ZX_OK, vmo_out.get_info(ZX_INFO_HANDLE_BASIC, &handle_info, sizeof(handle_info),
                                    nullptr, nullptr));
  EXPECT_TRUE(handle_info.rights & ZX_RIGHT_EXECUTE);
  EXPECT_FALSE(handle_info.rights & ZX_RIGHT_WRITE);
  EXPECT_EQ(ZX_OK, loader->Get("not-present", &vmo_out));
  EXPECT_FALSE(vmo_out.is_valid());
}

// Check that writes to one VMO returned by the server will not modify a separate VMO returned by
// the service.
TEST(OpenclLoader, VmosIndependent) {
  fuchsia::opencl::loader::LoaderSyncPtr loader;
  EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.opencl.loader.Loader",
                                        loader.NewRequest().TakeChannel().release()));

  zx::vmo vmo_out;
  // manifest.json remaps this to bin/opencl-server.
  EXPECT_EQ(ZX_OK, loader->Get(kIcdFilename, &vmo_out));
  ASSERT_TRUE(vmo_out.is_valid());

  fzl::VmoMapper mapper;
  EXPECT_EQ(ZX_OK,
            mapper.Map(vmo_out, 0, 0, ZX_VM_PERM_EXECUTE | ZX_VM_PERM_READ | ZX_VM_ALLOW_FAULTS));
  uint8_t original_value = *static_cast<uint8_t*>(mapper.start());
  uint8_t byte_to_write = original_value + 1;
  size_t actual;
  // zx_process_write_memory can write to memory mapped without ZX_VM_PERM_WRITE. If that ever
  // changes, this test can probably be removed.
  zx_status_t status = zx::process::self()->write_memory(
      reinterpret_cast<uint64_t>(mapper.start()), &byte_to_write, sizeof(byte_to_write), &actual);

  // zx_process_write_memory may be disabled using a kernel command-line flag.
  if (status == ZX_ERR_NOT_SUPPORTED) {
    EXPECT_EQ(original_value, *static_cast<uint8_t*>(mapper.start()));
  } else {
    EXPECT_EQ(ZX_OK, status);

    EXPECT_EQ(byte_to_write, *static_cast<uint8_t*>(mapper.start()));
  }

  // Ensure that the new clone is unaffected.
  zx::vmo vmo2;
  EXPECT_EQ(ZX_OK, loader->Get(kIcdFilename, &vmo2));
  EXPECT_TRUE(vmo2.is_valid());

  fzl::VmoMapper mapper2;
  EXPECT_EQ(ZX_OK,
            mapper2.Map(vmo2, 0, 0, ZX_VM_PERM_EXECUTE | ZX_VM_PERM_READ | ZX_VM_ALLOW_FAULTS));
  EXPECT_EQ(original_value, *static_cast<uint8_t*>(mapper2.start()));
}

TEST(OpenclLoader, DeviceFs) {
  fuchsia::opencl::loader::LoaderSyncPtr loader;
  EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.opencl.loader.Loader",
                                        loader.NewRequest().TakeChannel().release()));

  fidl::InterfaceHandle<fuchsia::io::Directory> dir;
  EXPECT_EQ(ZX_OK, loader->ConnectToDeviceFs(dir.NewRequest().TakeChannel()));

  ForceWaitForIdle(loader);

  fuchsia::gpu::magma::DeviceSyncPtr device_ptr;
  EXPECT_EQ(ZX_OK, fdio_service_connect_at(dir.channel().get(), "class/gpu/000",
                                           device_ptr.NewRequest().TakeChannel().release()));
  fuchsia::gpu::magma::Device_Query_Result query_result;
  ASSERT_EQ(ZX_OK, device_ptr->Query(fuchsia::gpu::magma::QueryId(0), &query_result));
  ASSERT_TRUE(query_result.is_response()) << zx_status_get_string(query_result.err());
  ASSERT_TRUE(query_result.response().is_simple_result());
  EXPECT_EQ(5u, query_result.response().simple_result());
}

TEST(OpenclLoader, Features) {
  fuchsia::opencl::loader::LoaderSyncPtr loader;
  EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.opencl.loader.Loader",
                                        loader.NewRequest().TakeChannel().release()));

  fuchsia::opencl::loader::Features features;
  EXPECT_EQ(ZX_OK, loader->GetSupportedFeatures(&features));
  constexpr fuchsia::opencl::loader::Features kExpectedFeatures =
      fuchsia::opencl::loader::Features::CONNECT_TO_DEVICE_FS |
      fuchsia::opencl::loader::Features::GET;
  EXPECT_EQ(kExpectedFeatures, features);
}

TEST(OpenclLoader, ManifestFs) {
  fuchsia::opencl::loader::LoaderSyncPtr loader;
  EXPECT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.opencl.loader.Loader",
                                        loader.NewRequest().TakeChannel().release()));

  fidl::InterfaceHandle<fuchsia::io::Directory> dir;
  EXPECT_EQ(ZX_OK, loader->ConnectToManifestFs(
                       fuchsia::opencl::loader::ConnectToManifestOptions::WAIT_FOR_IDLE,
                       dir.NewRequest().TakeChannel()));

  int dir_fd;
  EXPECT_EQ(ZX_OK, fdio_fd_create(dir.TakeChannel().release(), &dir_fd));

  int manifest_fd = openat(dir_fd, (std::string(kIcdFilename) + ".json").c_str(), O_RDONLY);

  EXPECT_LE(0, manifest_fd);

  constexpr int kManifestFileSize = 135;
  char manifest_data[kManifestFileSize + 1];
  ssize_t read_size = read(manifest_fd, manifest_data, sizeof(manifest_data) - 1);
  EXPECT_EQ(kManifestFileSize, read_size);

  close(manifest_fd);
  close(dir_fd);
}

TEST(OpenclLoader, DebugFilesystems) {
  fuchsia::opencl::loader::LoaderSyncPtr loader;
  ASSERT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.opencl.loader.Loader",
                                        loader.NewRequest().TakeChannel().release()));
  ForceWaitForIdle(loader);

  fuchsia::sys2::RealmQuerySyncPtr query;
  ASSERT_EQ(ZX_OK, fdio_service_connect("/svc/fuchsia.sys2.RealmQuery",
                                        query.NewRequest().TakeChannel().release()));

  fuchsia::sys2::RealmQuery_Open_Result result;
  fidl::InterfaceHandle<fuchsia::io::Node> dir;
  EXPECT_EQ(ZX_OK, query->Open("./opencl_loader", fuchsia::sys2::OpenDirType::OUTGOING_DIR,
                               fuchsia::io::OpenFlags::RIGHT_READABLE, fuchsia::io::ModeType(), ".",
                               dir.NewRequest(), &result));
  ASSERT_TRUE(result.is_response()) << result.err();

  fdio_ns_t* ns;
  EXPECT_EQ(ZX_OK, fdio_ns_get_installed(&ns));
  EXPECT_EQ(ZX_OK, fdio_ns_bind(ns, "/loader_out", dir.TakeChannel().release()));
  auto cleanup_binding = fit::defer([&]() { fdio_ns_unbind(ns, "/loader_out"); });

  const std::string debug_path("/loader_out/debug/");

  EXPECT_TRUE(std::filesystem::exists(debug_path + "device-fs/class/gpu/000"));
}
