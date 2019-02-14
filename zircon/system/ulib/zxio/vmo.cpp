// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/io/c/fidl.h>
#include <lib/zxio/inception.h>
#include <lib/zxio/null.h>
#include <lib/zxio/ops.h>
#include <string.h>
#include <sys/stat.h>
#include <zircon/syscalls.h>

static zx_status_t zxio_vmo_release(zxio_t* io, zx_handle_t* out_handle) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);
    zx_handle_t vmo = file->vmo;
    file->vmo = ZX_HANDLE_INVALID;
    *out_handle = vmo;
    return ZX_OK;
}

static zx_status_t zxio_vmo_close(zxio_t* io) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);
    zx_handle_t vmo = file->vmo;
    file->vmo = ZX_HANDLE_INVALID;
    zx_handle_close(vmo);
    return ZX_OK;
}

static zx_status_t zxio_vmo_attr_get(zxio_t* io, zxio_node_attr_t* out_attr) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);
    memset(out_attr, 0, sizeof(*out_attr));
    out_attr->mode = S_IFREG | S_IRUSR;
    out_attr->content_size = file->size;
    return ZX_OK;
}

static zx_status_t zxio_vmo_read(zxio_t* io, void* buffer, size_t capacity,
                                 size_t* out_actual) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);

    mtx_lock(&file->lock);
    if (capacity > (file->size - file->offset)) {
        capacity = file->size - file->offset;
    }
    zx_off_t offset = file->offset;
    file->offset += capacity;
    mtx_unlock(&file->lock);

    zx_status_t status = zx_vmo_read(file->vmo, buffer, offset, capacity);
    if (status == ZX_OK) {
        *out_actual = capacity;
    }
    return status;
}

static zx_status_t zxio_vmo_read_at(zxio_t* io, size_t offset, void* buffer,
                                    size_t capacity, size_t* out_actual) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);

    if (offset > file->size) {
        return ZX_ERR_INVALID_ARGS;
    }

    if (capacity > file->size - offset) {
        capacity = file->size - offset;
    }

    zx_status_t status = zx_vmo_read(file->vmo, buffer, offset, capacity);
    if (status == ZX_OK) {
        *out_actual = capacity;
    }
    return status;
}

zx_status_t zxio_vmo_write(zxio_t* io, const void* buffer, size_t capacity,
                           size_t* out_actual) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);

    mtx_lock(&file->lock);
    if (capacity > (file->size - file->offset)) {
        capacity = file->size - file->offset;
    }
    zx_off_t offset = file->offset;
    file->offset += capacity;
    mtx_unlock(&file->lock);

    zx_status_t status = zx_vmo_write(file->vmo, buffer, offset, capacity);
    if (status == ZX_OK) {
        *out_actual = capacity;
    }
    return status;
}

zx_status_t zxio_vmo_write_at(zxio_t* io, size_t offset, const void* buffer,
                              size_t capacity, size_t* out_actual) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);

    if (offset > file->size) {
        return ZX_ERR_INVALID_ARGS;
    }

    if (capacity > file->size - offset) {
        capacity = file->size - offset;
    }

    zx_status_t status = zx_vmo_write(file->vmo, buffer, offset, capacity);
    if (status == ZX_OK) {
        *out_actual = capacity;
    }
    return status;
}

static zx_status_t zxio_vmo_seek(zxio_t* io, size_t offset,
                                 zxio_seek_origin_t start,
                                 size_t* out_offset) {
    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(io);

    mtx_lock(&file->lock);
    zx_off_t at = 0u;
    switch (start) {
    case fuchsia_io_SeekOrigin_START:
        at = offset;
        break;
    case fuchsia_io_SeekOrigin_CURRENT:
        at = file->offset + offset;
        break;
    case fuchsia_io_SeekOrigin_END:
        at = file->size + offset;
        break;
    default:
        mtx_unlock(&file->lock);
        return ZX_ERR_INVALID_ARGS;
    }
    if (at > file->size) {
        at = ZX_ERR_OUT_OF_RANGE;
    } else {
        file->offset = at;
    }
    mtx_unlock(&file->lock);

    *out_offset = at;
    return ZX_OK;
}

static constexpr zxio_ops_t zxio_vmo_ops = []() {
    zxio_ops_t ops = zxio_default_ops;
    ops.release = zxio_vmo_release;
    ops.close = zxio_vmo_close;
    ops.attr_get = zxio_vmo_attr_get;
    ops.read = zxio_vmo_read;
    ops.read_at = zxio_vmo_read_at;
    ops.write = zxio_vmo_write;
    ops.write_at = zxio_vmo_write_at;
    ops.seek = zxio_vmo_seek;
    return ops;
}();

zx_status_t zxio_vmo_init(zxio_storage_t* storage, zx_handle_t vmo,
                          zx_off_t offset) {
    uint64_t size = 0u;
    zx_status_t status = zx_vmo_get_size(vmo, &size);
    if (status != ZX_OK) {
        zx_handle_close(vmo);
        return status;
    }

    zxio_vmo_t* file = reinterpret_cast<zxio_vmo_t*>(storage);
    zxio_init(&file->io, &zxio_vmo_ops);
    if (offset > size)
        offset = size;
    file->vmo = vmo;
    file->size = size;
    file->offset = offset;
    mtx_init(&file->lock, mtx_plain);
    return ZX_OK;
}
