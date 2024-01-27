// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fshost/fxfs.h"

#include <fidl/fuchsia.component/cpp/wire.h>
#include <fidl/fuchsia.fxfs/cpp/wire.h>
#include <fidl/fuchsia.fxfs/cpp/wire_types.h>
#include <lib/component/incoming/cpp/clone.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fidl/cpp/wire/vector_view.h>
#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/result.h>

#include <explicit-memory/bytes.h>

#include "src/lib/files/directory.h"
#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/storage/fs_management/cpp/options.h"
#include "src/lib/storage/key-bag/c/key_bag.h"
#include "src/security/lib/fcrypto/bytes.h"
#include "src/security/lib/kms-stateless/kms-stateless.h"
#include "src/security/lib/zxcrypt/client.h"
#include "src/storage/fshost/crypt_policy.h"
#include "src/storage/fshost/fshost_config.h"

namespace fshost {
namespace {

constexpr const char* KeySourceString(KeySource source) {
  switch (source) {
    case KeySource::kNullSource:
      return "null";
    case KeySource::kTeeSource:
      return "tee";
  }
}

constexpr unsigned char kLegacyCryptDataKey[32] = {
    0x0,  0x1,  0x2,  0x3,  0x4,  0x5,  0x6,  0x7,  0x8,  0x9,  0xa,  0xb,  0xc,  0xd,  0xe,  0xf,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

constexpr unsigned char kLegacyCryptMetadataKey[32] = {
    0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8, 0xf7, 0xf6, 0xf5, 0xf4, 0xf3, 0xf2, 0xf1, 0xf0,
    0xef, 0xee, 0xed, 0xec, 0xeb, 0xea, 0xe9, 0xe8, 0xe7, 0xe6, 0xe5, 0xe4, 0xe3, 0xe2, 0xe1, 0xe0,
};

constexpr char kFxfsUnencryptedVolumeName[] = "unencrypted";
constexpr char kFxfsDataVolumeName[] = "data";

// For legacy reasons, the key name is "zxcrypt"; this is so old recovery images will correctly wipe
// the data key when performing a factory reset.
// zxcrypt is the legacy crypto mechanism for minfs, which doesn't have its own encryption.
constexpr char kFxfsDataVolumeKeyName[] = "zxcrypt";

zx::result<crypto::Bytes> GetKeyFromKms(std::string_view key_name) {
  // Zero-pad the key name.  key_info_buf does not need to be null-terminated.
  if (key_name.length() > kms_stateless::kExpectedKeyInfoSize) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  uint8_t key_info[kms_stateless::kExpectedKeyInfoSize]{0};
  strncpy(reinterpret_cast<char*>(key_info), key_name.data(), key_name.length());
  crypto::Bytes key;
  zx_status_t status = kms_stateless::GetHardwareDerivedKey(
      [&key](std::unique_ptr<uint8_t[]> cb_key_buffer, size_t cb_key_size) {
        return key.Copy(cb_key_buffer.get(), cb_key_size);
      },
      key_info);

  if (status != ZX_OK) {
    return zx::error(status);
  }
  ZX_ASSERT_MSG(key.len() == key_bag::AES128_KEY_SIZE, "Expected a 128-bit key from kms");
  return zx::ok(std::move(key));
}

// Generates a key deterministically from |key_name|.
zx::result<crypto::Bytes> GenerateInsecureKey(std::string_view key_name) {
  if (key_name.length() == 0 || key_name.length() > key_bag::AES128_KEY_SIZE) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  crypto::Bytes key;
  if (zx_status_t status = key.Resize(key_bag::AES128_KEY_SIZE); status != ZX_OK)
    return zx::error(status);
  if (zx_status_t status =
          key.Copy(reinterpret_cast<const uint8_t*>(key_name.data()), key_name.length());
      status != ZX_OK)
    return zx::error(status);
  return zx::ok(std::move(key));
}

// This will spawn a new crypt client component. Nothing exists to destroy the components so it's
// possible to end up with orphaned crypt components.  The Rust port of fshost does not have this
// problem and the C++ version of fshost is not long for this world, so it's not something to worry
// about.
zx::result<fidl::ClientEnd<fuchsia_io::Directory>> InitCryptClient(crypto::Bytes data,
                                                                   crypto::Bytes metadata) {
  auto realm_client_end = component::Connect<fuchsia_component::Realm>();
  if (realm_client_end.is_error())
    return realm_client_end.take_error();
  fidl::WireSyncClient realm{std::move(*realm_client_end)};
  fidl::ClientEnd<fuchsia_io::Directory> client_end;

  static std::atomic_int instance(1);
  std::string component_name = "fxfs-crypt." + std::to_string(instance.fetch_add(1));
  constexpr std::string_view kCollectionName = "fxfs-crypt";

  fidl::Arena allocator;
  fuchsia_component_decl::wire::CollectionRef collection_ref{
      .name = fidl::StringView::FromExternal(kCollectionName)};
  auto child_decl = fuchsia_component_decl::wire::Child::Builder(allocator)
                        .name(component_name)
                        .url("#meta/fxfs-crypt.cm")
                        .startup(fuchsia_component_decl::wire::StartupMode::kLazy)
                        .Build();
  fuchsia_component::wire::CreateChildArgs child_args;
  auto create_res = realm->CreateChild(collection_ref, child_decl, child_args);
  if (!create_res.ok())
    return zx::error(create_res.status());
  if (create_res->is_error())
    return zx::error(ZX_ERR_INVALID_ARGS);

  fuchsia_component_decl::wire::ChildRef child_ref{
      .name = fidl::StringView::FromExternal(component_name),
      .collection = fidl::StringView::FromExternal(kCollectionName)};
  auto exposed_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (exposed_endpoints.is_error())
    return exposed_endpoints.take_error();
  auto open_exposed_res = realm->OpenExposedDir(child_ref, std::move(exposed_endpoints->server));
  if (!open_exposed_res.ok()) {
    return zx::error(open_exposed_res.status());
  }
  if (open_exposed_res->is_error())
    return zx::error(ZX_ERR_INVALID_ARGS);

  auto crypt = component::ConnectAt<fuchsia_fxfs::CryptManagement>(exposed_endpoints->client);
  if (crypt.is_error()) {
    FX_PLOGS(ERROR, crypt.error_value()) << "Failed to connect to CryptManagement service.";
    return crypt.take_error();
  }

  if (auto result = fidl::WireCall(*crypt)->AddWrappingKey(
          0, fidl::VectorView<unsigned char>::FromExternal(data.get(), data.len()));
      !result.ok()) {
    FX_LOGS(ERROR) << "Failed to add data key: " << zx_status_get_string(result.status());
    return zx::error(result.status());
  }
  if (auto result = fidl::WireCall(*crypt)->AddWrappingKey(
          1, fidl::VectorView<unsigned char>::FromExternal(metadata.get(), metadata.len()));
      !result.ok()) {
    FX_LOGS(ERROR) << "Failed to add metadata key: " << zx_status_get_string(result.status());
    return zx::error(result.status());
  }
  if (auto result = fidl::WireCall(*crypt)->SetActiveKey(fuchsia_fxfs::wire::KeyPurpose::kData, 0);
      !result.ok()) {
    FX_LOGS(ERROR) << "Failed to set active data key: " << zx_status_get_string(result.status());
    return zx::error(result.status());
  }
  if (auto result =
          fidl::WireCall(*crypt)->SetActiveKey(fuchsia_fxfs::wire::KeyPurpose::kMetadata, 1);
      !result.ok()) {
    FX_LOGS(ERROR) << "Failed to set active metadata key: "
                   << zx_status_get_string(result.status());
    return zx::error(result.status());
  }
  return zx::ok(std::move(exposed_endpoints->client));
}

zx::result<fs_management::MountedVolume*> UnwrapOrInitDataVolume(
    fs_management::StartedMultiVolumeFilesystem& fs, const fshost_config::Config& config,
    const bool create) {
  auto volume_fn = [&](const char* name, zx::channel crypt) {
    return create ? fs.CreateVolume(name, std::move(crypt)) : fs.OpenVolume(name, std::move(crypt));
  };
  const auto op = create ? "create" : "unwrap";

  // We might find a different layout than we expect.  Respect this, since otherwise the paving
  // workflow won't work if the paver and the paved version have different values of
  // use_native_fxfs_crypto (the paver will write SSH keys to one layout and the system will attempt
  // to open up the other layout).
  bool use_native_fxfs_crypto = config.use_native_fxfs_crypto();
  bool has_native_layout = !fs.HasVolume("default");
  if (!create && (has_native_layout != use_native_fxfs_crypto)) {
    FX_LOGS(WARNING) << "Overriding use_native_fxfs_crypto due to detected different layout";
    use_native_fxfs_crypto = !use_native_fxfs_crypto;
  }

  if (!use_native_fxfs_crypto) {
    FX_LOGS(INFO) << "Using legacy crypto configuration for Fxfs";
    crypto::Bytes data_key, metadata_key;
    data_key.Copy(kLegacyCryptDataKey, sizeof(kLegacyCryptDataKey));
    metadata_key.Copy(kLegacyCryptMetadataKey, sizeof(kLegacyCryptMetadataKey));

    auto exposed_dir = InitCryptClient(std::move(data_key), std::move(metadata_key));
    if (exposed_dir.is_error())
      return exposed_dir.take_error();

    if (!create && config.check_filesystems()) {
      FX_LOGS(INFO) << "Checking default volume integrity...";
      auto crypt = component::ConnectAt<fuchsia_fxfs::Crypt>(*exposed_dir);
      if (crypt.is_error()) {
        FX_PLOGS(ERROR, crypt.error_value()) << "Failed to connect to Crypt service.";
        return crypt.take_error();
      }
      auto status = fs.CheckVolume("default", std::move(crypt)->TakeChannel());
      if (status.is_error()) {
        FX_PLOGS(ERROR, status.error_value()) << "Volume is corrupt!";
        return status.take_error();
      }
    }
    auto crypt = component::ConnectAt<fuchsia_fxfs::Crypt>(*exposed_dir);
    if (crypt.is_error()) {
      FX_PLOGS(ERROR, crypt.error_value()) << "Failed to connect to Crypt service.";
      return crypt.take_error();
    }
    return volume_fn("default", std::move(crypt)->TakeChannel());
  }

  // Open up the unencrypted volume so that we can access the key-bag for data.
  if (!create && config.check_filesystems()) {
    FX_LOGS(INFO) << "Checking " << kFxfsUnencryptedVolumeName << " volume integrity...";
    auto status = fs.CheckVolume(kFxfsUnencryptedVolumeName, {});
    if (status.is_error()) {
      FX_PLOGS(ERROR, status.error_value()) << "Volume is corrupt!";
      return status.take_error();
    }
  }
  auto root_volume = volume_fn(kFxfsUnencryptedVolumeName, {});
  if (root_volume.is_error())
    return root_volume.take_error();
  auto data_root = (*root_volume)->DataRoot();
  if (data_root.is_error()) {
    FX_PLOGS(ERROR, data_root.status_value()) << "Failed to " << (create ? "create" : "open")
                                              << " data root in " << kFxfsUnencryptedVolumeName;
    return data_root.take_error();
  }

  const std::string mount_path("/unencrypted_volume");
  const std::string keybag_dir_path = mount_path + "/keys";
  const std::string keybag_path = keybag_dir_path + "/fxfs-data";
  auto binding = fs_management::NamespaceBinding::Create(mount_path.c_str(), std::move(*data_root));
  if (binding.is_error()) {
    FX_PLOGS(ERROR, binding.status_value()) << "Failed to bind data root";
    return binding.take_error();
  }
  if (create) {
    files::CreateDirectory(keybag_dir_path);
  }

  key_bag::KeyBagManager* kb;
  zx_status_t status;
  if (status = key_bag::keybag_open(keybag_path.c_str(), &kb); status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to open keybag";
    return zx::error(status);
  }
  auto cleanup = fit::defer([&]() { key_bag::keybag_close(kb); });

  auto ksp = SelectKeySourcePolicy();
  if (ksp.is_error()) {
    FX_PLOGS(ERROR, ksp.status_value()) << "Failed to load key source policy";
    return zx::error(ZX_ERR_BAD_STATE);
  }
  key_bag::Aes256Key data_unwrapped, metadata_unwrapped;
  auto cleanup2 = fit::defer([&]() {
    mandatory_memset(&data_unwrapped, 0, sizeof(data_unwrapped));
    mandatory_memset(&metadata_unwrapped, 0, sizeof(metadata_unwrapped));
  });
  bool keys_initialized = false;
  // Try each supported key source in order, which supports soft transitions.
  auto key_sources =
      create ? ComputeEffectiveCreatePolicy(*ksp) : ComputeEffectiveUnsealPolicy(*ksp);
  for (const auto& key_source : key_sources) {
    FX_LOGS(INFO) << "Trying key policy " << KeySourceString(key_source);
    std::function<zx::result<crypto::Bytes>(const char*)> key_fn;
    zx::result<crypto::Bytes> unwrap_key_bytes;
    switch (key_source) {
      case KeySource::kTeeSource: {
        unwrap_key_bytes = GetKeyFromKms(kFxfsDataVolumeKeyName);
        break;
      }
      case KeySource::kNullSource: {
        FX_LOGS(WARNING) << "Using static keys for fxfs; this is INSECURE on production builds.";
        unwrap_key_bytes = GenerateInsecureKey(kFxfsDataVolumeKeyName);
        break;
      }
    }
    if (!unwrap_key_bytes.is_ok()) {
      continue;
    }
    ZX_ASSERT(unwrap_key_bytes->len() == key_bag::AES128_KEY_SIZE);
    key_bag::WrappingKey unwrap;
    auto cleanup3 = fit::defer([&]() { mandatory_memset(&unwrap, 0, sizeof(unwrap)); });
    if (zx_status_t status = key_bag::keybag_create_aes128_wrapping_key(
            unwrap_key_bytes->get(), unwrap_key_bytes->len(), &unwrap);
        status != ZX_OK) {
      return zx::error(status);
    }
    auto unwrap_fn = create ? key_bag::keybag_new_key : key_bag::keybag_unwrap_key;
    if (status = unwrap_fn(kb, 0, &unwrap, &data_unwrapped); status != ZX_OK) {
      if (status == ZX_ERR_ACCESS_DENIED) {
        continue;
      }
      FX_PLOGS(ERROR, status) << "Failed to " << op << " data key";
      return zx::error(status);
    }
    if (status = unwrap_fn(kb, 1, &unwrap, &metadata_unwrapped); status != ZX_OK) {
      if (status == ZX_ERR_ACCESS_DENIED) {
        continue;
      }
      FX_PLOGS(ERROR, status) << "Failed to " << op << " metadata key";
      return zx::error(status);
    }
    keys_initialized = true;
    break;
  }
  if (!keys_initialized) {
    FX_LOGS(ERROR) << "Failed to " << op << " keys using all possible key sources.";
    return zx::error(ZX_ERR_ACCESS_DENIED);
  }

  crypto::Bytes data_key, metadata_key;
  if (status = data_key.Copy(data_unwrapped._0, key_bag::AES256_KEY_SIZE); status != ZX_OK)
    return zx::error(status);
  if (status = metadata_key.Copy(metadata_unwrapped._0, key_bag::AES256_KEY_SIZE); status != ZX_OK)
    return zx::error(status);
  auto exposed_dir = InitCryptClient(std::move(data_key), std::move(metadata_key));
  if (exposed_dir.is_error())
    return exposed_dir.take_error();

  // OK, crypt is seeded with the stored keys, so we can finally open the data volume.
  if (!create && config.check_filesystems()) {
    FX_LOGS(INFO) << "Checking " << kFxfsDataVolumeName << " volume integrity...";
    auto crypt = component::ConnectAt<fuchsia_fxfs::Crypt>(*exposed_dir);
    if (crypt.is_error()) {
      FX_PLOGS(ERROR, crypt.error_value()) << "Failed to connect to Crypt service.";
      return crypt.take_error();
    }
    auto status = fs.CheckVolume(kFxfsDataVolumeName, std::move(crypt)->TakeChannel());
    if (status.is_error()) {
      FX_PLOGS(ERROR, status.error_value()) << "Volume is corrupt!";
      return status.take_error();
    }
  }
  auto crypt = component::ConnectAt<fuchsia_fxfs::Crypt>(*exposed_dir);
  if (crypt.is_error()) {
    FX_PLOGS(ERROR, crypt.error_value()) << "Failed to connect to Crypt service.";
    return crypt.take_error();
  }
  return volume_fn(kFxfsDataVolumeName, std::move(crypt)->TakeChannel());
}

}  // namespace

zx::result<std::pair<fs_management::StartedMultiVolumeFilesystem, fs_management::MountedVolume*>>
FormatFxfsAndInitDataVolume(fidl::ClientEnd<fuchsia_hardware_block::Block> block_device,
                            const fshost_config::Config& config) {
  {
    constexpr char kStartupServicePath[] = "/fxfs/svc/fuchsia.fs.startup.Startup";
    auto startup_client_end = component::Connect<fuchsia_fs_startup::Startup>(kStartupServicePath);
    if (startup_client_end.is_error()) {
      FX_PLOGS(ERROR, startup_client_end.error_value())
          << "Failed to connect to startup service at " << kStartupServicePath;
      return startup_client_end.take_error();
    }
    fidl::WireSyncClient startup_client{std::move(*startup_client_end)};
    const fs_management::MkfsOptions options;

    zx::result cloned = component::Clone(block_device, component::AssumeProtocolComposesNode);
    if (cloned.is_error()) {
      FX_PLOGS(ERROR, cloned.error_value()) << "Failed to clone block channel";
      return cloned.take_error();
    }

    auto res = startup_client->Format(std::move(cloned.value()), options.as_format_options());
    if (!res.ok()) {
      FX_PLOGS(ERROR, res.status()) << "Failed to format (FIDL error)";
      return zx::error(res.status());
    }
    if (res.value().is_error()) {
      FX_PLOGS(ERROR, res.value().error_value()) << "Format failed";
      return zx::error(res.value().error_value());
    }
  }

  fs_management::MountOptions mount_options;
  mount_options.component_child_name =
      fs_management::DiskFormatString(fs_management::kDiskFormatFxfs);
  zx::result fs =
      fs_management::MountMultiVolume(std::move(block_device), fs_management::kDiskFormatFxfs,
                                      mount_options, fs_management::LaunchLogsAsync);
  if (fs.is_error()) {
    FX_PLOGS(ERROR, fs.error_value()) << "MountMultiVolume failed";
    return fs.take_error();
  }
  zx::result volume = InitDataVolume(*fs, config);
  if (volume.is_error()) {
    FX_PLOGS(ERROR, volume.error_value()) << "InitDataVolume failed";
    return volume.take_error();
  }
  return zx::ok(std::make_pair(std::move(*fs), *volume));
}

zx::result<fs_management::MountedVolume*> UnwrapDataVolume(
    fs_management::StartedMultiVolumeFilesystem& fs, const fshost_config::Config& config) {
  return UnwrapOrInitDataVolume(fs, config, false);
}

// Initializes the data volume in |fs|, which should be freshly reformatted.
zx::result<fs_management::MountedVolume*> InitDataVolume(
    fs_management::StartedMultiVolumeFilesystem& fs, const fshost_config::Config& config) {
  return UnwrapOrInitDataVolume(fs, config, true);
}

}  // namespace fshost
