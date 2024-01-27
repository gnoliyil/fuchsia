// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zbi/zbi.h>
#include <stdbool.h>
#include <string.h>

// At most, a ZBI container can be the header + 32 bits of length.
#define MAX_CONTAINER_SIZE (sizeof(zbi_header_t) + 0xFFFFFFFFull)

static bool is_zbi_container(const zbi_header_t* hdr) {
  return (hdr->type == ZBI_TYPE_CONTAINER) && (hdr->magic == ZBI_ITEM_MAGIC) &&
         (hdr->extra == ZBI_CONTAINER_MAGIC);
}

zbi_result_t zbi_init(void* buffer, const size_t length) {
  if (!buffer) {
    return ZBI_RESULT_ERROR;
  }

  if (length < sizeof(zbi_header_t)) {
    return ZBI_RESULT_TOO_BIG;
  }

  if ((uintptr_t)buffer % ZBI_ALIGNMENT != 0) {
    return ZBI_RESULT_BAD_ALIGNMENT;
  }

  zbi_header_t* hdr = (zbi_header_t*)buffer;
  hdr->type = ZBI_TYPE_CONTAINER;
  hdr->length = 0;
  hdr->extra = ZBI_CONTAINER_MAGIC;
  hdr->flags = ZBI_FLAGS_VERSION;
  hdr->reserved0 = 0;
  hdr->reserved1 = 0;
  hdr->magic = ZBI_ITEM_MAGIC;
  hdr->crc32 = ZBI_ITEM_NO_CRC32;

  return ZBI_RESULT_OK;
}

static zbi_result_t for_each_check_entry(zbi_header_t* hdr, void* payload, void* cookie) {
  zbi_header_t** err = cookie;
  zbi_result_t result = ZBI_RESULT_OK;

  if (hdr->magic != ZBI_ITEM_MAGIC) {
    result = ZBI_RESULT_BAD_MAGIC;
  } else if ((hdr->flags & ZBI_FLAGS_VERSION) == 0) {
    result = ZBI_RESULT_BAD_VERSION;
  } else if ((hdr->flags & ZBI_FLAGS_CRC32) == 0 && hdr->crc32 != ZBI_ITEM_NO_CRC32) {
    result = ZBI_RESULT_BAD_CRC;
  }

  // If we found a problem, try to report it back up to the caller.
  if (err != NULL && result != ZBI_RESULT_OK) {
    *err = hdr;
  }

  return result;
}

static zbi_result_t zbi_check_internal(const void* base, uint32_t check_bootable,
                                       zbi_header_t** err) {
  if (!base) {
    return ZBI_RESULT_ERROR;
  }

  zbi_result_t res = ZBI_RESULT_OK;
  const zbi_header_t* header = base;

  if (header->type != ZBI_TYPE_CONTAINER) {
    res = ZBI_RESULT_BAD_TYPE;
  } else if (header->extra != ZBI_CONTAINER_MAGIC || header->magic != ZBI_ITEM_MAGIC) {
    res = ZBI_RESULT_BAD_MAGIC;
  } else if ((header->flags & ZBI_FLAGS_VERSION) == 0) {
    res = ZBI_RESULT_BAD_VERSION;
  } else if ((header->flags & ZBI_FLAGS_CRC32) == 0 && header->crc32 != ZBI_ITEM_NO_CRC32) {
    res = ZBI_RESULT_BAD_CRC;
  }

  // Something was wrong with the container.  Don't even attempt to process
  // the rest of the image.  Return diagnostic information back to the caller
  // if they requested it.
  if (res != ZBI_RESULT_OK) {
    if (err) {
      *err = (zbi_header_t*)header;
    }
    return res;
  }

  res = zbi_for_each(base, for_each_check_entry, err);

  if (res == ZBI_RESULT_OK && check_bootable != 0) {
    if (header->length == 0) {
      res = ZBI_RESULT_ERR_TRUNCATED;
    } else if (header[1].type != check_bootable) {
      res = ZBI_RESULT_INCOMPLETE_KERNEL;
      if (err) {
        *err = (zbi_header_t*)(header + 1);
      }
    }
  }

  if (err && res == ZBI_RESULT_ERR_TRUNCATED) {
    // A truncated image perhaps indicates a problem with the container?
    *err = (zbi_header_t*)header;
  }

  return res;
}

zbi_result_t zbi_check(const void* base, zbi_header_t** err) {
  return zbi_check_internal(base, 0, err);
}

zbi_result_t zbi_check_bootable(const void* base, zbi_header_t** err) {
  return zbi_check_internal(base,
#ifdef __aarch64__
                            ZBI_TYPE_KERNEL_ARM64,
#elif defined(__x86_64__) || defined(__i386__)
                            ZBI_TYPE_KERNEL_X64,
#elif defined(__riscv)
                            ZBI_TYPE_KERNEL_RISCV64,
#else
#error "what architecture?"
#endif
                            err);
}

zbi_result_t zbi_for_each(const void* base, const zbi_foreach_cb_t callback, void* cookie) {
  if (!base || !callback) {
    return ZBI_RESULT_ERROR;
  }

  zbi_header_t* header = (zbi_header_t*)(base);

  // Skip container header.
  const uint32_t totalSize = (uint32_t)sizeof(zbi_header_t) + header->length;
  uint32_t offset = sizeof(zbi_header_t);
  while (offset < totalSize) {
    zbi_header_t* entryHeader = (zbi_header_t*)(base + offset);

    zbi_result_t result = callback(entryHeader, entryHeader + 1, cookie);

    if (result != ZBI_RESULT_OK) {
      return result;
    }

    if ((offset + entryHeader->length + sizeof(zbi_header_t)) > totalSize) {
      return ZBI_RESULT_ERR_TRUNCATED;
    }

    offset = ZBI_ALIGN(offset + entryHeader->length + sizeof(zbi_header_t));
  }

  return ZBI_RESULT_OK;
}

zbi_result_t zbi_create_entry_with_payload(void* base, const size_t capacity, uint32_t type,
                                           uint32_t extra, uint32_t flags, const void* payload,
                                           size_t payload_length) {
  if (!base || !payload) {
    return ZBI_RESULT_ERROR;
  }

  uint8_t* new_section = NULL;
  zbi_result_t result =
      zbi_create_entry(base, capacity, type, extra, flags, payload_length, (void**)&new_section);

  if (result != ZBI_RESULT_OK) {
    return result;
  }

  // Copy in the payload.
  memcpy(new_section, payload, payload_length);
  return ZBI_RESULT_OK;
}

zbi_result_t zbi_create_entry(void* base, size_t capacity, uint32_t type, uint32_t extra,
                              uint32_t flags, size_t payload_length, void** payload) {
  if (!base) {
    return ZBI_RESULT_ERROR;
  }

  // We don't support CRC computation (yet?)
  if (flags & ZBI_FLAGS_CRC32) {
    return ZBI_RESULT_ERROR;
  }

  if (payload_length > 0xFFFFFFFFU) {
    return ZBI_RESULT_TOO_BIG;
  }

  zbi_header_t* hdr = (zbi_header_t*)base;

  // Find the payload entry point and make sure we have enough room.
  void* next_payload = NULL;
  uint32_t max_payload_length = 0;
  zbi_result_t result =
      zbi_get_next_entry_payload(base, capacity, &next_payload, &max_payload_length);
  if (result != ZBI_RESULT_OK) {
    return result;
  }

  if (max_payload_length < payload_length) {
    return ZBI_RESULT_TOO_BIG;
  }

  // Fill in the new section header.
  zbi_header_t* new_header = (void*)((uint8_t*)(hdr + 1) + hdr->length);
  *new_header = (zbi_header_t){
      .type = type,
      .length = (uint32_t)payload_length,
      .extra = extra,
      .flags = flags | ZBI_FLAGS_VERSION,
      .magic = ZBI_ITEM_MAGIC,
      .crc32 = ZBI_ITEM_NO_CRC32,
  };

  // Tell the caller where to fill in the payload. They may not need this if
  // the payload has already been loaded or the length is 0.
  if (payload) {
    *payload = next_payload;
  }

  // Update the container header, always keeping the length aligned.
  hdr->length += (uint32_t)sizeof(*new_header) + new_header->length;
  if (hdr->length % ZBI_ALIGNMENT != 0) {
    // It was already verified that the capacity can fit the aligned length.
    uint32_t aligned_length = ZBI_ALIGN(hdr->length);
    memset((uint8_t*)(hdr + 1) + hdr->length, 0, aligned_length - hdr->length);
    hdr->length = aligned_length;
  }

  return ZBI_RESULT_OK;
}

zbi_result_t zbi_get_next_entry_payload(void* base, size_t capacity, void** payload,
                                        uint32_t* max_payload_length) {
  if (!base || !payload || !max_payload_length) {
    return ZBI_RESULT_ERROR;
  }

  zbi_header_t* hdr = (zbi_header_t*)base;

  // Make sure we were actually passed a bootdata container.
  if (!is_zbi_container(hdr)) {
    return ZBI_RESULT_BAD_TYPE;
  }

  // Determine the maximum payload size available.
  // It's OK if the caller has more capacity available than the ZBI can actually
  // take, e.g. a bootloader may just supply whatever leftover memory it has.
  // Just truncate to the max ZBI size and ignore any extra capacity.
  if (capacity > MAX_CONTAINER_SIZE) {
    // On systems with a 32-bit size_t, this cast is necessary to avoid an error.
    // Note that this code will never run on those systems.
    capacity = (size_t)MAX_CONTAINER_SIZE;
  }
  if (capacity < sizeof(*hdr) || capacity - sizeof(*hdr) < hdr->length) {
    return ZBI_RESULT_TOO_BIG;
  }
  const uint32_t available = (capacity - sizeof(*hdr) - hdr->length) & ~(ZBI_ALIGNMENT - 1);
  if (available < sizeof(*hdr)) {
    return ZBI_RESULT_TOO_BIG;
  }

  // The next payload starts after the current container + the next header.
  *payload = (uint8_t*)(hdr + 2) + hdr->length;
  *max_payload_length = (uint32_t)(available - sizeof(*hdr));
  return ZBI_RESULT_OK;
}

zbi_result_t zbi_extend(void* dst_buffer, size_t capacity, const void* src_buffer) {
  if (!dst_buffer || !src_buffer) {
    return ZBI_RESULT_ERROR;
  }

  zbi_header_t* dst = (zbi_header_t*)dst_buffer;
  zbi_header_t* src = (zbi_header_t*)src_buffer;

  // Extend only works against two zbi containers, if you want to append a zbi
  // section to the end of a container, use zbi_append_section instead.
  if (!is_zbi_container(dst) || !is_zbi_container(src)) {
    return ZBI_RESULT_BAD_TYPE;
  }

  // Make sure there's enough space in the destination buffer to contain the
  // source.
  const uint32_t dst_size = ZBI_ALIGN(dst->length + sizeof(*dst));

  // This captures the situation where there's not even enough space to have
  // padding between this section and the next.
  if (dst_size > capacity) {
    return ZBI_RESULT_TOO_BIG;
  }

  // This makes sure that there's enough space to perform the copy after
  const size_t remaining_buffer = capacity - dst_size;
  if (remaining_buffer < src->length) {
    return ZBI_RESULT_TOO_BIG;
  }

  // Okay everything looks good, let's do the copy.
  memcpy(dst_buffer + dst_size, src_buffer + sizeof(*src), src->length);

  // And patch up the length on the destination buffer's header.
  dst->length += src->length;

  return ZBI_RESULT_OK;
}
