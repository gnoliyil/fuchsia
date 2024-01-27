// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/lib/guest_config/guest_config.h"

#include <lib/syslog/cpp/macros.h>
#include <libgen.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <unordered_map>
#include <utility>

#include <rapidjson/document.h>

namespace guest_config {

namespace {

using fuchsia::virtualization::GuestConfig;

zx_status_t parse(const OpenAt& open_at, const std::string& value,
                  fuchsia::virtualization::BlockSpec* out) {
  std::string path;
  std::istringstream token_stream(value);
  std::string token;
  while (std::getline(token_stream, token, ',')) {
    if (token == "rw") {
      out->mode = fuchsia::virtualization::BlockMode::READ_WRITE;
    } else if (token == "ro") {
      out->mode = fuchsia::virtualization::BlockMode::READ_ONLY;
    } else if (token == "volatile") {
      out->mode = fuchsia::virtualization::BlockMode::VOLATILE_WRITE;
    } else if (token == "file") {
      if (!out->format.has_invalid_tag()) {
        return ZX_ERR_INVALID_ARGS;
      }
      fidl::InterfaceHandle<fuchsia::io::File> file;
      if (zx_status_t status = open_at(path, file.NewRequest()); status != ZX_OK) {
        return status;
      }
      out->format.set_file(std::move(file));
    } else if (token == "qcow") {
      if (!out->format.has_invalid_tag()) {
        return ZX_ERR_INVALID_ARGS;
      }
      fidl::InterfaceHandle<fuchsia::io::File> file;
      if (zx_status_t status = open_at(path, file.NewRequest()); status != ZX_OK) {
        return status;
      }
      out->format.set_qcow(file.TakeChannel());
    } else if (token == "block") {
      if (!out->format.has_invalid_tag()) {
        return ZX_ERR_INVALID_ARGS;
      }
      fidl::InterfaceHandle<fuchsia::hardware::block::Block> block;
      if (zx_status_t status = open_at(
              path, fidl::InterfaceRequest<fuchsia::io::File>(block.NewRequest().TakeChannel()));
          status != ZX_OK) {
        return status;
      }
      out->format.set_block(std::move(block));
    } else {
      // Set the last MAX_BLOCK_DEVICE_ID characters of token as the ID.
      size_t pos = token.size() > fuchsia::virtualization::MAX_BLOCK_DEVICE_ID
                       ? token.size() - fuchsia::virtualization::MAX_BLOCK_DEVICE_ID
                       : 0;
      out->id = token.substr(pos, fuchsia::virtualization::MAX_BLOCK_DEVICE_ID);
      path = std::move(token);
    }
  }
  if (path.empty()) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (out->format.has_invalid_tag()) {
    fidl::InterfaceHandle<fuchsia::io::File> file;
    if (zx_status_t status = open_at(path, file.NewRequest()); status != ZX_OK) {
      return status;
    }
    out->format.set_file(std::move(file));
  }
  return ZX_OK;
}

template <typename T>
zx_status_t parse(const rapidjson::Value& value, T* result) {
  *result = value.Get<T>();
  return ZX_OK;
}
template <>
zx_status_t parse<uint8_t>(const rapidjson::Value& value, uint8_t* result) {
  unsigned int v = value.GetUint();
  if (v > std::numeric_limits<uint8_t>::max()) {
    FX_LOGS(ERROR) << "Value too large: " << v;
    return ZX_ERR_INVALID_ARGS;
  }
  *result = static_cast<uint8_t>(value.GetUint());
  return ZX_OK;
}

zx_status_t parse_mem(const rapidjson::Value& value, uint64_t* result) {
  char modifier = 'b';
  uint64_t size;
  if (!value.IsString()) {
    FX_LOGS(ERROR) << "Value is not a string";
    return ZX_ERR_INVALID_ARGS;
  }

  int ret = sscanf(value.GetString(), "%" SCNu64 "%c", &size, &modifier);
  if (ret < 1) {
    FX_LOGS(ERROR) << "Value is not a size string: " << value.GetString();
    return ZX_ERR_INVALID_ARGS;
  }

  switch (modifier) {
    case 'b':
      break;
    case 'k':
      size *= (1 << 10);
      break;
    case 'M':
      size *= (1 << 20);
      break;
    case 'G':
      size *= (1 << 30);
      break;
    default:
      FX_LOGS(ERROR) << "Invalid size modifier " << modifier;
      return ZX_ERR_INVALID_ARGS;
  }

  *result = size;
  return ZX_OK;
}

class OptionHandler {
 public:
  OptionHandler() = default;

  virtual zx_status_t Set(GuestConfig* cfg, const std::string& name,
                          const rapidjson::Value& value) = 0;

  virtual ~OptionHandler() = default;
};

template <typename T>
class SimpleOptionHandler : public OptionHandler {
 public:
  SimpleOptionHandler(std::function<T*(GuestConfig*)> mutable_field)
      : OptionHandler(), mutable_field_{std::move(mutable_field)} {}

 protected:
  void FillMutableField(const T& value, GuestConfig* cfg) { *(mutable_field_(cfg)) = value; }

  zx_status_t Set(GuestConfig* cfg, const std::string& name,
                  const rapidjson::Value& value) override {
    T result;

    zx_status_t status = parse(value, &result);
    if (status == ZX_OK) {
      FillMutableField(result, cfg);
    }
    return status;
  }

 private:
  std::function<T*(GuestConfig*)> mutable_field_;
};

class MemorySizeOptionHandler : public SimpleOptionHandler<uint64_t> {
 public:
  MemorySizeOptionHandler(std::function<uint64_t*(GuestConfig*)> mutable_field)
      : SimpleOptionHandler<uint64_t>(std::move(mutable_field)) {}

  zx_status_t Set(GuestConfig* cfg, const std::string& name,
                  const rapidjson::Value& value) override {
    uint64_t result;
    auto status = parse_mem(value, &result);
    if (status == ZX_OK) {
      FillMutableField(result, cfg);
    }
    return status;
  }
};

using NumCpusOptionHandler = SimpleOptionHandler<uint8_t>;
using BoolOptionHandler = SimpleOptionHandler<bool>;
using StringOptionHandler = SimpleOptionHandler<std::string>;

class FileOptionHandler : public OptionHandler {
 public:
  FileOptionHandler(OpenAt open_at,
                    std::function<fuchsia::io::FileHandle*(GuestConfig*)> mutable_field)
      : OptionHandler(), open_at_{std::move(open_at)}, mutable_field_{std::move(mutable_field)} {}

 protected:
  zx_status_t Set(GuestConfig* cfg, const std::string& name,
                  const rapidjson::Value& value) override {
    if (value.GetStringLength() == 0) {
      FX_LOGS(ERROR) << "Option: '" << name << "' expects a value (--" << name << "=<value>)";
      return ZX_ERR_INVALID_ARGS;
    }
    fuchsia::io::FileHandle result;
    zx_status_t status = open_at_(value.GetString(), result.NewRequest());
    if (status == ZX_OK) {
      *(mutable_field_(cfg)) = std::move(result);
    }
    return status;
  }

 private:
  OpenAt open_at_;
  std::function<fuchsia::io::FileHandle*(GuestConfig*)> mutable_field_;
};

class KernelOptionHandler : public FileOptionHandler {
 public:
  KernelOptionHandler(
      OpenAt open_at, std::function<fuchsia::io::FileHandle*(GuestConfig*)> mutable_field,
      std::function<fuchsia::virtualization::KernelType*(GuestConfig*)> mutable_type_fn,
      fuchsia::virtualization::KernelType type)
      : FileOptionHandler{std::move(open_at), std::move(mutable_field)},
        mutable_type_fn_{std::move(mutable_type_fn)},
        type_{type} {}

 private:
  zx_status_t Set(GuestConfig* cfg, const std::string& name,
                  const rapidjson::Value& value) override {
    auto status = FileOptionHandler::Set(cfg, name, value);
    if (status == ZX_OK) {
      *(mutable_type_fn_(cfg)) = type_;
    }
    return status;
  }

  std::function<fuchsia::virtualization::KernelType*(GuestConfig*)> mutable_type_fn_;
  fuchsia::virtualization::KernelType type_;
};

template <typename T>
class RepeatedOptionHandler : public OptionHandler {
 public:
  RepeatedOptionHandler(std::function<std::vector<T>*(GuestConfig*)> mutable_field)
      : mutable_field_{std::move(mutable_field)} {}

 protected:
  zx_status_t Set(GuestConfig* cfg, const std::string& name,
                  const rapidjson::Value& value) override {
    if (value.GetStringLength() == 0) {
      FX_LOGS(ERROR) << "Option: '" << name << "' expects a value (--" << name << "=<value>)";
      return ZX_ERR_INVALID_ARGS;
    }
    T result{};
    auto status = parse(value, &result);
    if (status == ZX_OK) {
      mutable_field_(cfg)->emplace_back(result);
    }
    return status;
  }

 private:
  std::function<std::vector<T>*(GuestConfig*)> mutable_field_;
};

template <>
class RepeatedOptionHandler<fuchsia::virtualization::BlockSpec> : public OptionHandler {
 public:
  RepeatedOptionHandler(
      OpenAt open_at,
      std::function<std::vector<fuchsia::virtualization::BlockSpec>*(GuestConfig*)> mutable_field)
      : open_at_(std::move(open_at)), mutable_field_{std::move(mutable_field)} {}

 protected:
  zx_status_t Set(GuestConfig* cfg, const std::string& name,
                  const rapidjson::Value& value) override {
    if (value.GetStringLength() == 0) {
      FX_LOGS(ERROR) << "Option: '" << name << "' expects a value (--" << name << "=<value>)";
      return ZX_ERR_INVALID_ARGS;
    }
    fuchsia::virtualization::BlockSpec result;
    auto status = parse(open_at_, value.GetString(), &result);
    if (status == ZX_OK) {
      mutable_field_(cfg)->emplace_back(std::move(result));
    }
    return status;
  }

 private:
  OpenAt open_at_;
  std::function<std::vector<fuchsia::virtualization::BlockSpec>*(GuestConfig*)> mutable_field_;
};

std::unordered_map<std::string, std::unique_ptr<OptionHandler>> GetAllOptionHandlers(
    OpenAt open_at) {
  std::unordered_map<std::string, std::unique_ptr<OptionHandler>> handlers;
  handlers.emplace("block",
                   std::make_unique<RepeatedOptionHandler<fuchsia::virtualization::BlockSpec>>(
                       open_at.share(), &GuestConfig::mutable_block_devices));
  handlers.emplace("cmdline", std::make_unique<StringOptionHandler>(&GuestConfig::mutable_cmdline));
  handlers.emplace("dtb-overlay", std::make_unique<FileOptionHandler>(
                                      open_at.share(), &GuestConfig::mutable_dtb_overlay));
  handlers.emplace(
      "linux", std::make_unique<KernelOptionHandler>(open_at.share(), &GuestConfig::mutable_kernel,
                                                     &GuestConfig::mutable_kernel_type,
                                                     fuchsia::virtualization::KernelType::LINUX));
  handlers.emplace("ramdisk", std::make_unique<FileOptionHandler>(open_at.share(),
                                                                  &GuestConfig::mutable_ramdisk));
  handlers.emplace(
      "zircon", std::make_unique<KernelOptionHandler>(open_at.share(), &GuestConfig::mutable_kernel,
                                                      &GuestConfig::mutable_kernel_type,
                                                      fuchsia::virtualization::KernelType::ZIRCON));
  handlers.emplace("cmdline-add", std::make_unique<RepeatedOptionHandler<std::string>>(
                                      &GuestConfig::mutable_cmdline_add));
  handlers.emplace("memory",
                   std::make_unique<MemorySizeOptionHandler>(&GuestConfig::mutable_guest_memory));
  handlers.emplace("cpus", std::make_unique<NumCpusOptionHandler>(&GuestConfig::mutable_cpus));
  handlers.emplace("default-net",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_default_net));
  handlers.emplace("virtio-balloon",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_balloon));
  handlers.emplace("virtio-console",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_console));
  handlers.emplace("virtio-gpu",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_gpu));
  handlers.emplace("virtio-rng",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_rng));
  handlers.emplace("virtio-sound",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_sound));
  handlers.emplace("virtio-sound-input",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_sound_input));
  handlers.emplace("virtio-vsock",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_vsock));
  handlers.emplace("virtio-mem",
                   std::make_unique<BoolOptionHandler>(&GuestConfig::mutable_virtio_mem));
  handlers.emplace("virtio-mem-block-size", std::make_unique<MemorySizeOptionHandler>(
                                                &GuestConfig::mutable_virtio_mem_block_size));
  handlers.emplace("virtio-mem-region-size", std::make_unique<MemorySizeOptionHandler>(
                                                 &GuestConfig::mutable_virtio_mem_region_size));
  handlers.emplace(
      "virtio-mem-region-alignment",
      std::make_unique<MemorySizeOptionHandler>(&GuestConfig::mutable_virtio_mem_region_alignment));
  return handlers;
}

}  // namespace

zx::result<GuestConfig> ParseConfig(const std::string& data, OpenAt open_at) {
  rapidjson::Document document;
  document.Parse(data);
  if (!document.IsObject()) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  GuestConfig cfg;
  auto opts = GetAllOptionHandlers(std::move(open_at));
  for (auto& member : document.GetObject()) {
    auto entry = opts.find(member.name.GetString());
    if (entry == opts.end()) {
      FX_LOGS(ERROR) << "Unknown field in configuration object: " << member.name.GetString();
      return zx::error(ZX_ERR_INVALID_ARGS);
    }
    zx_status_t status = ZX_OK;
    if (member.value.IsArray()) {
      // for array members, invoke the handler on each value in the array.
      for (auto& array_member : member.value.GetArray()) {
        status = entry->second->Set(&cfg, member.name.GetString(), array_member);
      }
    } else {
      // For atomic members, invoke the handler directly on the value.
      status = entry->second->Set(&cfg, member.name.GetString(), member.value);
    }

    if (status != ZX_OK) {
      return zx::error(ZX_ERR_INVALID_ARGS);
    }
  }

  return zx::ok(std::move(cfg));
}

fuchsia::virtualization::GuestConfig MergeConfigs(fuchsia::virtualization::GuestConfig base,
                                                  fuchsia::virtualization::GuestConfig overrides) {
#define COPY_GUEST_CONFIG_FIELD(field_name)                                \
  do {                                                                     \
    if (overrides.has_##field_name()) {                                    \
      base.set_##field_name(std::move(*overrides.mutable_##field_name())); \
    }                                                                      \
  } while (0)
#define APPEND_GUEST_CONFIG_FIELD(field_name)                                 \
  do {                                                                        \
    if (overrides.has_##field_name()) {                                       \
      base.mutable_##field_name()->insert(                                    \
          base.mutable_##field_name()->end(),                                 \
          std::make_move_iterator(overrides.mutable_##field_name()->begin()), \
          std::make_move_iterator(overrides.mutable_##field_name()->end()));  \
    }                                                                         \
  } while (0)

  COPY_GUEST_CONFIG_FIELD(kernel_type);
  COPY_GUEST_CONFIG_FIELD(kernel);
  COPY_GUEST_CONFIG_FIELD(ramdisk);
  COPY_GUEST_CONFIG_FIELD(dtb_overlay);
  COPY_GUEST_CONFIG_FIELD(cmdline);
  APPEND_GUEST_CONFIG_FIELD(cmdline_add);
  COPY_GUEST_CONFIG_FIELD(cpus);
  COPY_GUEST_CONFIG_FIELD(guest_memory);
  APPEND_GUEST_CONFIG_FIELD(block_devices);
  APPEND_GUEST_CONFIG_FIELD(net_devices);
  COPY_GUEST_CONFIG_FIELD(wayland_device);
  COPY_GUEST_CONFIG_FIELD(magma_device);
  COPY_GUEST_CONFIG_FIELD(default_net);
  COPY_GUEST_CONFIG_FIELD(virtio_mem);
  COPY_GUEST_CONFIG_FIELD(virtio_mem_block_size);
  COPY_GUEST_CONFIG_FIELD(virtio_mem_region_size);
  COPY_GUEST_CONFIG_FIELD(virtio_mem_region_alignment);
  COPY_GUEST_CONFIG_FIELD(virtio_balloon);
  COPY_GUEST_CONFIG_FIELD(virtio_console);
  COPY_GUEST_CONFIG_FIELD(virtio_gpu);
  COPY_GUEST_CONFIG_FIELD(virtio_rng);
  COPY_GUEST_CONFIG_FIELD(virtio_vsock);
  COPY_GUEST_CONFIG_FIELD(virtio_sound);
  COPY_GUEST_CONFIG_FIELD(virtio_sound_input);
  APPEND_GUEST_CONFIG_FIELD(vsock_listeners);

#undef COPY_GUEST_CONFIG_FIELD
#undef APPEND_GUEST_CONFIG_FIELD

  return base;
}

}  // namespace guest_config
