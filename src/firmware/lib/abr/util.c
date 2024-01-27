/* Copyright 2019 The Fuchsia Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <lib/abr/abr.h>
#include <lib/abr/util.h>
#include <stdarg.h>

/* Returns |in| in big-endian byte order. */
static uint32_t MakeBigEndian(uint32_t in) {
  union {
    uint32_t word;
    uint8_t bytes[4];
  } ret;
  ret.bytes[0] = (in >> 24) & 0xff;
  ret.bytes[1] = (in >> 16) & 0xff;
  ret.bytes[2] = (in >> 8) & 0xff;
  ret.bytes[3] = in & 0xff;
  return ret.word;
}

uint32_t AbrHostToBigEndian(uint32_t in) { return MakeBigEndian(in); }

uint32_t AbrBigEndianToHost(uint32_t in) { return MakeBigEndian(in); }

int AbrSafeMemcmp(const void* s1, const void* s2, size_t n) {
  const unsigned char* us1 = s1;
  const unsigned char* us2 = s2;
  int result = 0;

  if (0 == n) {
    return 0;
  }

  /*
   * Code snippet without data-dependent branch due to Nate Lawson
   * (nate@root.org) of Root Labs.
   */
  while (n--) {
    result |= *us1++ ^ *us2++;
  }

  return result != 0;
}

bool AbrIsOneShotRecoveryBootSet(uint8_t flags) { return flags & kAbrDataOneShotFlagRecoveryBoot; }

bool AbrIsOneShotRecoveryBoot(const AbrData* abr_data) {
  return AbrIsOneShotRecoveryBootSet(abr_data->one_shot_flags);
}

bool AbrIsOneShotBootloaderBootSet(uint8_t flags) {
  return flags & kAbrDataOneShotFlagBootloaderBoot;
}

bool AbrIsOneShotBootloaderBoot(const AbrData* abr_data) {
  return AbrIsOneShotBootloaderBootSet(abr_data->one_shot_flags);
}

void AbrSetOneShotRecoveryBoot(AbrData* abr_data, bool enable) {
  if (enable) {
    abr_data->one_shot_flags |= kAbrDataOneShotFlagRecoveryBoot;
  } else {
    abr_data->one_shot_flags &= ~kAbrDataOneShotFlagRecoveryBoot;
  }
}

void AbrSetOneShotBootloaderBoot(AbrData* abr_data, bool enable) {
  if (enable) {
    abr_data->one_shot_flags |= kAbrDataOneShotFlagBootloaderBoot;
  } else {
    abr_data->one_shot_flags &= ~kAbrDataOneShotFlagBootloaderBoot;
  }
}
