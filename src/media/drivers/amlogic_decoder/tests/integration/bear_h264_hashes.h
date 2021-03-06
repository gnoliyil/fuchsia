// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_TESTS_INTEGRATION_BEAR_H264_HASHES_H_
#define SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_TESTS_INTEGRATION_BEAR_H264_HASHES_H_

#include <cstdint>

#include <openssl/sha.h>

namespace amlogic_decoder {
namespace test {

// SHA256 hashes for decoded contents of each frame of bear.h264. Generated by running:
// uint8_t md[SHA256_DIGEST_LENGTH];
// HashFrame(frame.get(), md);
// printf("%s\n", CppStringingfyHash(md).c_str());
// clang-format off
static uint8_t bear_h264_hashes[][SHA256_DIGEST_LENGTH] = {
{0x6a,0x5c,0xb0,0x32,0x82,0x79,0xa2,0xc8,0x09,0xd3,0xad,0xe7,0xcc,0x46,0x2b,0xa8,0x34,0x2a,0x2d,0x68,0x89,0xb1,0xd8,0x79,0xce,0xa1,0x57,0xc1,0xde,0x18,0xef,0x78,},
{0xc9,0xf6,0x43,0x58,0x00,0xb9,0xa6,0xab,0xa4,0x08,0x92,0x4f,0x38,0x86,0x12,0xf5,0xfa,0xc9,0x35,0xf1,0x7d,0x4d,0x05,0x8d,0x3e,0x0b,0x82,0x0e,0x1b,0x29,0x66,0x7e,},
{0x27,0xb0,0xb1,0xa5,0xd8,0x1b,0x3a,0xfe,0x15,0xa5,0xe1,0x9c,0x21,0x29,0x33,0x99,0x70,0x43,0x51,0x58,0xd7,0xae,0x83,0x9a,0x20,0x60,0x0b,0x05,0xbf,0x1a,0xdb,0x67,},
{0x3e,0x32,0x30,0x56,0x3c,0xeb,0xba,0x75,0xb0,0xd2,0x5e,0x10,0xb0,0x38,0x3b,0x6a,0x4e,0x29,0xeb,0xc3,0x82,0xd3,0x24,0x4b,0xe9,0x17,0x09,0xfc,0x41,0xde,0xaf,0xb0,},
{0x73,0x9e,0xdd,0x43,0xbd,0x78,0x32,0x0d,0x8a,0x6f,0xbb,0xae,0xb2,0xdd,0xb0,0x66,0xa6,0x5f,0xa7,0x57,0x53,0xd2,0xdc,0x8d,0xaf,0x84,0x2a,0xc7,0x78,0x30,0x19,0xdf,},
{0x00,0x02,0x4f,0xa0,0xc7,0xb5,0x93,0xbc,0xbd,0x12,0xaa,0x74,0x34,0x0a,0xe6,0xae,0x72,0x25,0xb9,0xd8,0x38,0x2e,0x06,0xd5,0x91,0x12,0x76,0xf1,0x2e,0x61,0xa6,0xc3,},
{0x2f,0xb6,0xc3,0x5b,0x46,0x08,0xb6,0xc1,0x0c,0x4f,0x35,0x4c,0x12,0x9d,0x09,0x15,0x47,0x32,0x6b,0x7a,0xa2,0x5a,0x8e,0x10,0xa9,0xf8,0x99,0xfb,0x35,0xac,0xbb,0x22,},
{0x29,0xaa,0xe2,0x92,0xb1,0xa5,0xc5,0x5c,0x32,0x0c,0x95,0x01,0x4a,0x38,0xcd,0xbc,0xd7,0xc6,0xe4,0xaa,0x95,0x40,0x76,0x34,0xf0,0xbd,0x12,0x3d,0x53,0xbd,0x7d,0xdc,},
{0xff,0x36,0x20,0x42,0xd5,0xdc,0x80,0xdc,0x2d,0x9a,0x7f,0x8e,0x5f,0x86,0x5c,0xd3,0xc7,0x8a,0x24,0x1b,0xba,0x21,0x6a,0x31,0x70,0x74,0xfc,0xd8,0x22,0x0e,0x31,0x50,},
{0x8d,0xe0,0x22,0xdb,0x7b,0xb8,0xc4,0x60,0x9d,0xd2,0x96,0x4e,0x98,0x05,0x0a,0xd1,0x3a,0xbd,0xc4,0xd5,0x4a,0x56,0xab,0xe7,0x99,0xf9,0x52,0x1d,0xd4,0xc4,0xd2,0xbb,},
{0x81,0x6a,0xc6,0x54,0xa6,0x72,0x29,0x21,0x4b,0xff,0xf3,0xa4,0x47,0xc3,0x13,0xf5,0xa1,0x16,0x19,0xe6,0x8d,0x41,0x69,0x1e,0xb1,0x93,0x8a,0xf3,0xb8,0x63,0x4a,0x60,},
{0x01,0x10,0x30,0x4b,0x32,0xc5,0x5a,0x5b,0xad,0x59,0xf0,0xac,0x77,0x14,0x3d,0xfe,0x53,0x86,0xda,0xe5,0x12,0x2e,0xc6,0xed,0x8f,0x74,0x2d,0x4e,0x0f,0xaa,0x83,0x78,},
{0x71,0xb9,0x1a,0x93,0xc1,0x7a,0x9b,0x1b,0xc3,0x1c,0xd0,0x97,0xc1,0xbd,0xd0,0xcc,0xcd,0x5b,0x70,0x75,0xac,0x3c,0xf0,0x36,0xe4,0xf6,0x26,0xe4,0xaf,0x1f,0x2a,0x02,},
{0xfa,0x8b,0x67,0x46,0xe4,0x84,0x7f,0xc6,0x76,0x39,0xdf,0x56,0x19,0xed,0x7c,0x53,0x4c,0x84,0xac,0xe5,0x70,0xe7,0xa7,0x66,0x47,0x1b,0x00,0x85,0x13,0x91,0x72,0x20,},
{0x37,0x9c,0x70,0x0a,0xad,0xe7,0x28,0xf7,0x3b,0xcd,0xb0,0x7e,0x74,0xd1,0x4d,0x01,0x13,0x51,0x6e,0xda,0xfe,0x77,0xe5,0x78,0x54,0xe1,0x42,0x79,0x7f,0x78,0xcd,0xc7,},
{0xa6,0xe3,0x62,0x41,0x43,0xd4,0xd2,0x6c,0x20,0x0f,0x35,0xdf,0x24,0xa9,0x9a,0x23,0x9d,0xb3,0x8e,0x49,0xd8,0x12,0xba,0x55,0xc0,0x34,0x2d,0x34,0x50,0xc7,0x88,0xb2,},
{0x41,0xe0,0xff,0x96,0xa1,0x61,0x34,0x43,0x8b,0xbe,0xf6,0x79,0xe9,0xec,0x85,0x92,0x3e,0x65,0x37,0x12,0xd4,0x70,0x66,0x3c,0x90,0xf1,0xa3,0x7c,0x59,0x30,0x82,0x2c,},
{0x6c,0xad,0x9d,0x7b,0x1e,0x13,0x70,0x5b,0x62,0x2d,0x50,0x19,0xb6,0x56,0xf8,0x52,0xc1,0x31,0xda,0xf1,0x31,0x48,0x9b,0x75,0x93,0xde,0x1e,0x8b,0x86,0x38,0xc2,0xc9,},
{0xb7,0xfe,0x03,0x67,0xf8,0x8f,0x7f,0x5c,0x3b,0xdf,0x59,0x13,0x28,0x8b,0x45,0x77,0xb4,0xe5,0x79,0xea,0xa9,0xd9,0x00,0x5d,0xfd,0x00,0xcc,0xc6,0xe5,0x93,0x44,0x59,},
{0x10,0x79,0xd3,0x64,0x01,0xd0,0xb5,0xa7,0xc1,0x84,0x7f,0xc8,0x0e,0x63,0xee,0xfa,0xec,0xbf,0x38,0x03,0x15,0x75,0xdd,0x1e,0xf3,0xc2,0x94,0x07,0x49,0xc6,0x6e,0xbc,},
{0x39,0x4c,0xba,0x0c,0x83,0x0a,0x7a,0x74,0xdf,0xb5,0x18,0x19,0x58,0x8e,0x23,0x08,0x1a,0x1b,0x23,0x06,0x82,0x53,0x57,0x2e,0xa9,0x63,0xb4,0x36,0x1f,0xa8,0x57,0xe3,},
{0xce,0x9d,0xdf,0x18,0xf8,0xab,0xa2,0x48,0xa2,0xeb,0xef,0xec,0x20,0x48,0xf3,0xec,0xf8,0x67,0x63,0x2f,0x74,0xb4,0x03,0x8a,0x75,0xd1,0x0b,0x84,0xb8,0x5b,0x9b,0x80,},
{0x72,0x84,0xdc,0x86,0xd8,0xcf,0xaf,0xb3,0xd6,0x4d,0x6d,0x7f,0x55,0x86,0x7a,0xc9,0x41,0x71,0x70,0x42,0xef,0xcd,0x13,0xec,0xf0,0xee,0x4b,0x74,0xef,0xa9,0x98,0x99,},
{0x21,0x7b,0x14,0xd5,0x48,0x08,0xc0,0x0e,0x5c,0x2d,0xc9,0xca,0xdf,0xb2,0xf0,0x4e,0xe6,0xb2,0xc7,0xcc,0xb1,0xa7,0x10,0xa5,0xae,0xc0,0xe2,0xe5,0x8f,0x87,0x94,0x9d,},
{0x07,0x39,0x7a,0x99,0x58,0xa8,0x98,0xba,0xbd,0xff,0x2f,0x81,0x05,0x4f,0x59,0x14,0x3a,0xba,0xba,0x18,0xc9,0x73,0x13,0xac,0x86,0xe1,0x98,0xc0,0x36,0xef,0xb8,0x29,},
{0x1d,0xb5,0xf4,0xea,0x1a,0x24,0x96,0x0e,0x50,0x36,0x4d,0xc3,0x43,0x91,0x91,0xaa,0x64,0x26,0x66,0xd9,0xe1,0xe5,0xb2,0x69,0x4c,0x29,0x66,0x28,0xba,0x2f,0xf6,0x5a,},
{0xb7,0xc8,0x2e,0x0e,0xd4,0x87,0xe9,0x6e,0x4c,0x33,0xee,0xd0,0xa6,0x21,0x65,0xe4,0x81,0x33,0xfb,0xc2,0x13,0xd4,0xa9,0x88,0xdc,0x16,0xec,0x15,0xdd,0x24,0x7a,0x05,},
{0x46,0x28,0xb9,0x08,0x84,0x65,0x08,0x26,0x88,0x7f,0xab,0xe2,0x5d,0xd0,0xc9,0x3f,0x41,0x83,0xdf,0x6c,0x67,0x59,0x80,0xa5,0x12,0xd0,0x3c,0x5f,0xea,0x6e,0xc6,0xdd,},
{0xda,0x94,0xa6,0xa6,0x6f,0xae,0x57,0x0b,0x9a,0x9f,0x2c,0x4c,0xa5,0x6a,0x8d,0x3b,0xe5,0x79,0xb0,0xb4,0xef,0x56,0x83,0x48,0x8e,0xab,0x5f,0x57,0xdb,0x36,0xe1,0x50,},
{0x23,0xbb,0x0e,0x07,0x9a,0x76,0x0e,0xc6,0x87,0x5b,0xea,0xf9,0x1e,0x63,0x3b,0x30,0xa7,0xc7,0xe1,0xff,0x1d,0xe1,0x9b,0x3c,0x86,0x0c,0x8b,0xc9,0xf6,0xa4,0x1d,0x4f,},
};

}
}

#endif  // SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_TESTS_INTEGRATION_BEAR_H264_HASHES_H_
