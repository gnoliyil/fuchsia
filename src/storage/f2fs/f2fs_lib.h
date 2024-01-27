// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_F2FS_F2FS_LIB_H_
#define SRC_STORAGE_F2FS_F2FS_LIB_H_

#include <safemath/checked_math.h>

namespace f2fs {

// Checkpoint
inline bool VerAfter(uint64_t a, uint64_t b) { return a > b; }

// CRC
inline uint32_t F2fsCalCrc32(uint32_t crc, void *buff, uint32_t len) {
  unsigned char *p = static_cast<unsigned char *>(buff);
  while (len-- > 0) {
    crc ^= *p++;
    for (int i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ ((crc & 1) ? kCrcPolyLe : 0);
  }
  return crc;
}

inline uint32_t F2fsCrc32(void *buff, uint32_t len) {
  return F2fsCalCrc32(kF2fsSuperMagic, static_cast<unsigned char *>(buff), len);
}

inline bool F2fsCrcValid(uint32_t blk_crc, void *buff, uint32_t buff_size) {
  return F2fsCrc32(buff, buff_size) == blk_crc;
}

// Bitmap operations
inline void SetBit(uint32_t nr, void *addr) {
  uint8_t *bitmap = static_cast<uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;

  uint32_t iter = nr / size_per_iter;
  uint32_t offset_in_iter = nr % size_per_iter;

  bitmap[iter] |= static_cast<uint8_t>(1U << offset_in_iter);
}

inline void ClearBit(uint32_t nr, void *addr) {
  uint8_t *bitmap = static_cast<uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;

  uint32_t iter = nr / size_per_iter;
  uint32_t offset_in_iter = nr % size_per_iter;

  bitmap[iter] &= ~static_cast<uint8_t>(1U << offset_in_iter);
}

inline bool TestBit(uint32_t nr, const void *addr) {
  const uint8_t *bitmap = static_cast<const uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;

  uint32_t iter = nr / size_per_iter;
  uint32_t offset_in_iter = nr % size_per_iter;

  bool ret = ((bitmap[iter] & static_cast<uint8_t>(1U << offset_in_iter)) != 0);

  return ret;
}

inline uint32_t FindNextZeroBit(const void *addr, uint32_t size, uint32_t offset) {
  const uint8_t *bitmap = static_cast<const uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;

  while (offset < size) {
    uint32_t iter = offset / size_per_iter;
    uint32_t offset_in_iter = offset % size_per_iter;

    uint8_t mask = static_cast<uint8_t>(~0U << offset_in_iter);
    uint8_t res = bitmap[iter] & mask;
    if (res != mask) {  // found
      for (; offset_in_iter < size_per_iter; ++offset_in_iter) {
        if ((bitmap[iter] & static_cast<uint8_t>(1U << offset_in_iter)) == 0) {
          return std::min(iter * size_per_iter + offset_in_iter, size);
        }
      }
    }

    offset = (iter + 1) * size_per_iter;
  }

  return size;
}

inline uint32_t FindNextBit(const void *addr, uint32_t size, uint32_t offset) {
  const uint8_t *bitmap = static_cast<const uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;

  while (offset < size) {
    uint32_t iter = offset / size_per_iter;
    uint32_t offset_in_iter = offset % size_per_iter;

    uint8_t mask = static_cast<uint8_t>(~0U << offset_in_iter);
    uint8_t res = bitmap[iter] & mask;
    if (res != 0) {  // found
      for (; offset_in_iter < size_per_iter; ++offset_in_iter) {
        if ((bitmap[iter] & static_cast<uint8_t>(1U << offset_in_iter)) != 0) {
          return std::min(iter * size_per_iter + offset_in_iter, size);
        }
      }
    }

    offset = (iter + 1) * size_per_iter;
  }

  return size;
}

inline bool TestAndSetBit(uint32_t nr, void *addr) {
  uint8_t *bitmap = static_cast<uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;
  uint32_t iter = nr / size_per_iter;
  uint32_t offset_in_iter = nr % size_per_iter;

  bool ret = ((bitmap[iter] & static_cast<uint8_t>(1U << offset_in_iter)) != 0);

  bitmap[iter] |= static_cast<uint8_t>(1U << offset_in_iter);

  return ret;
}

inline bool TestAndClearBit(uint32_t nr, void *addr) {
  uint8_t *bitmap = static_cast<uint8_t *>(addr);
  uint32_t size_per_iter = kBitsPerByte;

  uint32_t iter = nr / size_per_iter;
  uint32_t offset_in_iter = nr % size_per_iter;

  bool ret = ((bitmap[iter] & static_cast<uint8_t>(1U << offset_in_iter)) != 0);

  bitmap[iter] &= ~static_cast<uint8_t>((1U << offset_in_iter));

  return ret;
}

inline uint32_t CountBits(const void *addr, uint32_t offset, uint32_t len) {
  uint32_t end = offset + len, sum = 0;

  for (; offset < end; ++offset) {
    if (TestBit(offset, addr)) {
      ++sum;
    }
  }
  return sum;
}

inline bool IsDotOrDotDot(std::string_view name) { return (name == "." || name == ".."); }

template <typename T>
inline T CheckedDivRoundUp(const T n, const T d) {
  return safemath::CheckDiv<T>(fbl::round_up(n, d), d).ValueOrDie();
}

inline size_t BitsToLongs(size_t nr) {
  return CheckedDivRoundUp<size_t>(nr, kBitsPerByte * sizeof(long));
}

constexpr uint32_t kBlockSize = 4096;  // F2fs block size in byte
template <typename T = uint8_t>
class FsBlock {
 public:
  FsBlock() { memset(data_, 0, kBlockSize); }
  FsBlock(uint8_t (&block)[kBlockSize]) { memcpy(data_, block, kBlockSize); }
  FsBlock(const FsBlock &block) = delete;
  FsBlock &operator=(const FsBlock &block) = delete;
  FsBlock &operator=(const uint8_t (&block)[kBlockSize]) {
    memcpy(data_, block, kBlockSize);
    return *this;
  }
  template <typename U = void>
  U *get() {
    return reinterpret_cast<U *>(data_);
  }
  T *operator->() { return get<T>(); }
  T *operator&() { return get<T>(); }
  T &operator*() { return *get<T>(); }

 private:
  uint64_t data_[kBlockSize / sizeof(uint64_t)];
};

}  // namespace f2fs

#endif  // SRC_STORAGE_F2FS_F2FS_LIB_H_
