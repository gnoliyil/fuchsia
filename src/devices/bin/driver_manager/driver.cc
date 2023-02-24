// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver.h"

#include <elf.h>
#include <fcntl.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/ddk/binding.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <new>
#include <string>

#include <fbl/algorithm.h>
#include <fbl/string_printf.h>

#include "fdio.h"
#include "src/devices/lib/log/log.h"

namespace {

namespace fio = fuchsia_io;

using NoteFunc = fit::function<zx_status_t(void* note, size_t size)>;

using elfhdr = Elf64_Ehdr;
using elfphdr = Elf64_Phdr;
using notehdr = Elf64_Nhdr;

zx_status_t FindNote(uint8_t* data, size_t size, NoteFunc& func) {
  const std::string kNoteName = ZIRCON_NOTE_NAME;
  constexpr uint32_t kType = ZIRCON_NOTE_DRIVER;
  while (size >= sizeof(notehdr)) {
    const auto* header = reinterpret_cast<const notehdr*>(data);
    uint32_t name_size = fbl::round_up(header->n_namesz, 4u);
    if (name_size > (size - sizeof(notehdr))) {
      return ZX_ERR_INTERNAL;
    }
    size_t header_size = sizeof(notehdr) + name_size;
    data += header_size;
    size -= header_size;

    uint32_t desc_size = fbl::round_up(header->n_descsz, 4u);
    if (desc_size > size) {
      return ZX_ERR_INTERNAL;
    }

    if ((header->n_type == kType) && (header->n_namesz - 1 == kNoteName.size()) &&
        (kNoteName ==
         std::string_view(reinterpret_cast<const char*>(header + 1), header->n_namesz - 1))) {
      return func(data - header_size, header->n_descsz + header_size);
    }

    data += desc_size;
    size -= desc_size;
  }
  return ZX_ERR_NOT_FOUND;
}

zx_status_t ForEachNote(zx::unowned_vmo dso, uint8_t* data, size_t dsize, NoteFunc func) {
  elfphdr ph[64];
  elfhdr eh;
  if (dso->read(&eh, 0, sizeof(eh)) != ZX_OK) {
    LOGF(ERROR, "zx_vmo_read(eh) failed");
    return ZX_ERR_IO;
  }
  if (memcmp(&eh, ELFMAG, 4) || (eh.e_ehsize != sizeof(elfhdr)) ||
      (eh.e_phentsize != sizeof(elfphdr))) {
    LOGF(ERROR, "bad elf magic");
    return ZX_ERR_INTERNAL;
  }
  size_t size = sizeof(elfphdr) * eh.e_phnum;
  if (size > sizeof(ph)) {
    LOGF(ERROR, "too many phdrs");
    return ZX_ERR_INTERNAL;
  }
  if (dso->read(ph, eh.e_phoff, size) != ZX_OK) {
    LOGF(ERROR, "zx_vmo_read(eh.e_phoff, size) failed");
    return ZX_ERR_IO;
  }
  for (int i = 0; i < eh.e_phnum; i++) {
    if ((ph[i].p_type != PT_NOTE) || (ph[i].p_filesz > dsize)) {
      continue;
    }
    if (dso->read(data, ph[i].p_offset, ph[i].p_filesz) != ZX_OK) {
      LOGF(ERROR, "zx_vmo_read(ph[i]) failed");
      return ZX_ERR_IO;
    }
    int r = FindNote(data, ph[i].p_filesz, func);
    if (r == ZX_OK) {
      return r;
    }
  }
  return ZX_ERR_NOT_FOUND;
}

using InfoFunc = fit::function<void(zircon_driver_note_payload_t* note)>;

zx_status_t ReadDriverInfo(zx::unowned_vmo dso, InfoFunc func) {
  constexpr size_t kBufferSize = 4096;
  uint8_t data[kBufferSize];
  return ForEachNote(std::move(dso), data, sizeof(data), [&](void* note, size_t size) {
    if (size < sizeof(zircon_driver_note_t)) {
      return ZX_ERR_INTERNAL;
    }
    func(&reinterpret_cast<zircon_driver_note_t*>(note)->payload);
    return ZX_OK;
  });
}

bool is_driver_disabled(fidl::WireSyncClient<fuchsia_boot::Arguments>* boot_args,
                        const char* name) {
  if (!boot_args) {
    return false;
  }
  // driver.<driver_name>.disable
  auto option = fbl::StringPrintf("driver.%s.disable", name);
  auto disabled = (*boot_args)->GetBool(fidl::StringView::FromExternal(option), false);
  return disabled.ok() && disabled.value().value;
}

bool is_driver_eager_fallback(fidl::WireSyncClient<fuchsia_boot::Arguments>* boot_args,
                              const char* name) {
  if (!boot_args) {
    return false;
  }
  std::vector<fbl::String> eager_fallback_drivers;
  auto drivers = (*boot_args)->GetString("devmgr.bind-eager");
  if (drivers.ok() && !drivers.value().value.is_null() && !drivers.value().value.empty()) {
    std::string list(drivers.value().value.data(), drivers.value().value.size());
    size_t pos;
    while ((pos = list.find(',')) != std::string::npos) {
      eager_fallback_drivers.emplace_back(list.substr(0, pos));
      list.erase(0, pos + 1);
    }
    eager_fallback_drivers.emplace_back(std::move(list));
  }

  for (auto& driver : eager_fallback_drivers) {
    if (driver.compare(name) == 0) {
      return true;
    }
  }
  return false;
}

void found_driver(zircon_driver_note_payload_t* note,
                  fidl::WireSyncClient<fuchsia_boot::Arguments>* boot_args,
                  std::string_view libname, zx::vmo& driver_vmo, DriverLoadCallback& func) {
  // ensure strings are terminated
  note->name[sizeof(note->name) - 1] = 0;
  note->vendor[sizeof(note->vendor) - 1] = 0;
  note->version[sizeof(note->version) - 1] = 0;

  if (is_driver_disabled(boot_args, note->name)) {
    return;
  }

  auto drv = std::make_unique<Driver>();
  if (drv == nullptr) {
    return;
  }

  drv->flags = note->flags;
  drv->libname = libname;
  drv->name = note->name;
  if (note->version[0] == '*') {
    drv->fallback = true;
    // TODO(fxbug.dev/44586): remove this once a better solution for driver prioritization is
    // implemented.
    if (is_driver_eager_fallback(boot_args, drv->name.c_str())) {
      LOGF(INFO, "Marking fallback driver '%s' as eager.", drv->name.c_str());
      drv->fallback = false;
    }
  }

  drv->dso_vmo = std::move(driver_vmo);

  VLOGF(2, "Found driver: %s", libname.data());
  VLOGF(2, "        name: %s", note->name);
  VLOGF(2, "      vendor: %s", note->vendor);
  VLOGF(2, "     version: %s", note->version);
  VLOGF(2, "       flags: %#x", note->flags);

  func(drv.release(), note->version);
}

}  // namespace

zx_status_t load_driver_vmo(fidl::WireSyncClient<fuchsia_boot::Arguments>* boot_args,
                            std::string_view libname_view, zx::vmo driver_vmo,
                            DriverLoadCallback func) {
  std::string libname(libname_view);
  zx::unowned_vmo dso = driver_vmo.borrow();

  zx_status_t status = ReadDriverInfo(std::move(dso), [&](zircon_driver_note_payload_t* note) {
    found_driver(note, boot_args, libname, driver_vmo, func);
  });

  if (status == ZX_ERR_NOT_FOUND) {
    LOGF(INFO, "Missing info from driver '%s'", libname.c_str());
  } else if (status != ZX_OK) {
    LOGF(ERROR, "Failed to read info from driver '%s': %s", libname.c_str(),
         zx_status_get_string(status));
  }
  return status;
}

zx::result<zx::vmo> load_vmo(std::string_view libname_view) {
  std::string libname(libname_view);
  fbl::unique_fd fd;
  constexpr uint32_t file_rights = static_cast<uint32_t>(fio::wire::OpenFlags::kRightReadable |
                                                         fio::wire::OpenFlags::kRightExecutable);
  if (zx_status_t status = fdio_open_fd(libname.c_str(), file_rights, fd.reset_and_get_address());
      status != ZX_OK) {
    LOGF(ERROR, "Cannot open driver '%s': %d", libname.c_str(), status);
    return zx::error(ZX_ERR_IO);
  }

  zx::vmo vmo;
  if (zx_status_t status = fdio_get_vmo_exec(fd.get(), vmo.reset_and_get_address());
      status != ZX_OK) {
    LOGF(ERROR, "Cannot get driver VMO '%s'", libname.c_str());
    return zx::error(status);
  }

  // Libname is either a URL or a filesystem path.
  // Set the vmo_name to the final part of the path, i.e the characters after the last slash '/'.
  const char* vmo_name = libname.c_str();
  size_t last_slash = libname.rfind('/');
  if (last_slash != std::string::npos) {
    vmo_name = &libname[last_slash + 1];
  }

  if (zx_status_t status = vmo.set_property(ZX_PROP_NAME, vmo_name, strlen(vmo_name));
      status != ZX_OK) {
    LOGF(ERROR, "Cannot set name on driver VMO to '%s'", libname.c_str());
    return zx::error(status);
  }
  return zx::ok(std::move(vmo));
}
