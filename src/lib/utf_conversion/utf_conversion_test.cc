// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/utf_conversion/utf_conversion.h"

#include <lib/stdcompat/bit.h>
#include <stdio.h>

#include <iterator>

#include <fbl/algorithm.h>
#include <zxtest/zxtest.h>

namespace {

constexpr uint32_t HOST_ENDIAN_FLAG = cpp20::endian::native == cpp20::endian::big
                                          ? UTF_CONVERT_FLAG_FORCE_BIG_ENDIAN
                                          : UTF_CONVERT_FLAG_FORCE_LITTLE_ENDIAN;
constexpr uint32_t INVERT_ENDIAN_FLAG = cpp20::endian::native == cpp20::endian::big
                                            ? UTF_CONVERT_FLAG_FORCE_LITTLE_ENDIAN
                                            : UTF_CONVERT_FLAG_FORCE_BIG_ENDIAN;

#define ASSERT_UTF8_EQ(expected, expected_len, actual, actual_bytes, enc_len, msg) \
  do {                                                                             \
    ASSERT_GE(actual_bytes, expected_len, "%s", msg);                              \
    ASSERT_EQ(expected_len, enc_len, "%s", msg);                                   \
    ASSERT_BYTES_EQ(expected, actual, expected_len, "%s", msg);                    \
  } while (false)

constexpr uint16_t ByteSwap(uint16_t x) {
  return static_cast<uint16_t>(((x & 0xff00) >> 8) | ((x & 0x00ff) << 8));
}

TEST(UTF16To8TestCase, BadArgs) {
  uint16_t src;
  uint8_t dst = 0xFE;
  size_t dst_len;
  zx_status_t res;

  // Bad destination buffer with non-zero destination length
  dst_len = 1;
  res = utf16_to_utf8(&src, 1, nullptr, &dst_len);
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, res, "null dst should fail with INVALID_ARGS");
  ASSERT_EQ(1, dst_len, "dst_len modified after conversion with invalid args");

  // Bad dest len pointer
  res = utf16_to_utf8(&src, 1, &dst, nullptr);
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, res, "null dst_len should fail with INVALID_ARGS");
  ASSERT_EQ(0xFE, dst, "dst modified after conversion with invalid args");

  // Bad (undefined) flags
  res = utf16_to_utf8(&src, 1, &dst, &dst_len, 0x80000000);
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, res, "undefined flags should fail with INVALID_ARGS");
  ASSERT_EQ(1, dst_len, "dst_len modified after conversion with invalid args");
  ASSERT_EQ(0xFE, dst, "dst modified after conversion with invalid args");

  // A null dest buffer is allowed if (and only if) the dst_len is zero.
  // Practical use cases include using the converter to determine the length
  // needed to hold a converted string.
  dst_len = 0;
  src = 0xAB;
  res = utf16_to_utf8(&src, 1, nullptr, &dst_len);
  ASSERT_OK(res, "null dst with zero dst_len should succeed");
  ASSERT_EQ(2, dst_len, "encoded size of 0xAB should be 2!");
}

TEST(UTF16To8TestCase, EmptySource) {
  uint16_t src;
  static constexpr uint8_t kExpected[] = {0xA1, 0xB2, 0xC3, 0xD4};
  uint8_t actual[sizeof(kExpected)];
  size_t dst_len;
  zx_status_t res;

  // Check to make sure that attempting to encode a zero length source results
  // in a length of zero and no changes to the destination buffer.
  memcpy(actual, kExpected, sizeof(actual));
  dst_len = sizeof(actual);
  res = utf16_to_utf8(&src, 0, actual, &dst_len);
  ASSERT_OK(res, "zero length string conversion failed");
  ASSERT_EQ(0, dst_len, "dst_len should be zero after zero length string conversion");
  ASSERT_BYTES_EQ(kExpected, actual, sizeof(actual),
                  "dst buffer modified after zero length string conversion");

  dst_len = sizeof(actual);
  res = utf16_to_utf8(nullptr, 1, actual, &dst_len);
  ASSERT_OK(res, "null source string conversion failed");
  ASSERT_EQ(0, dst_len, "dst_len should be zero after null source string conversion");
  ASSERT_BYTES_EQ(kExpected, actual, sizeof(actual),
                  "dst buffer modified after null source string conversion");
}

TEST(UTF16To8TestCase, SimpleCodepoints) {
  static const struct {
    uint16_t src;
    uint8_t expected[3];
    size_t expected_len;
  } TEST_VECTORS[] = {
      // 1 byte UTF-8 codepoints (U+0000, U+007F)
      {0x0000, {0x00}, 1},
      {0x0001, {0x01}, 1},
      {0x007f, {0x7f}, 1},

      // 2 byte UTF-8 codepoints (U+0080, U+07FF)
      {0x0080, {0xC2, 0x80}, 2},
      {0x0456, {0xD1, 0x96}, 2},
      {0x07FF, {0xDF, 0xBF}, 2},

      // 3 byte UTF-8 codepoints (U+0800, U+07FF)
      // Note: we are skipping the (theoretically illegal) unpaired surrogate
      // range (U+D800, U+DFFF) here.  There is a separate test for support of
      // unpaired surrogates.
      {0x0800, {0xE0, 0xA0, 0x80}, 3},
      {0x4567, {0xE4, 0x95, 0xA7}, 3},
      {0xD7FF, {0xED, 0x9F, 0xBF}, 3},
      {0xE000, {0xEE, 0x80, 0x80}, 3},
      {0xE456, {0xEE, 0x91, 0x96}, 3},
      {0xFFFF, {0xEF, 0xBF, 0xBF}, 3},
  };

  uint8_t actual[3];
  for (const auto& v : TEST_VECTORS) {
    char case_id[64];
    size_t encoded_len = sizeof(actual);
    zx_status_t res;

    snprintf(case_id, sizeof(case_id), "case id [0x%04hx]", v.src);
    ::memset(actual, 0xAB, sizeof(actual));

    res = utf16_to_utf8(&v.src, 1, actual, &encoded_len);
    ASSERT_OK(res, "%s", case_id);
    ASSERT_LE(v.expected_len, sizeof(v.expected), "%s", case_id);
    ASSERT_UTF8_EQ(v.expected, v.expected_len, actual, sizeof(actual), encoded_len, case_id);
  }
}

TEST(UTF16To8TestCase, PairedSurrogates) {
  // All paired surrogate encodings are going to be 4 byte UTF-8 codepoints (U+010000, U+10FFFF)
  static const struct {
    uint16_t src[2];
    uint8_t expected[4];
  } TEST_VECTORS[] = {
      {{0xD800, 0xDC00}, {0xF0, 0x90, 0x80, 0x80}},  // U+10000
      {{0xD811, 0xDD67}, {0xF0, 0x94, 0x95, 0xA7}},  // U+14567
      {{0xDA6F, 0xDCDE}, {0xF2, 0xAB, 0xB3, 0x9E}},  // U+ABCDE
      {{0xDBBF, 0xDFFF}, {0xF3, 0xBF, 0xBF, 0xBF}},  // U+FFFFF
      {{0xDBC0, 0xDC00}, {0xF4, 0x80, 0x80, 0x80}},  // U+100000
      {{0xDBD1, 0xDD67}, {0xF4, 0x84, 0x95, 0xA7}},  // U+104567
      {{0xDBFF, 0xDFFF}, {0xF4, 0x8F, 0xBF, 0xBF}},  // U+10FFFF
  };

  uint8_t actual[4];
  for (const auto& v : TEST_VECTORS) {
    char case_id[64];
    size_t encoded_len = sizeof(actual);
    zx_status_t res;

    snprintf(case_id, sizeof(case_id), "case id [0x%04hx : 0x%04hx]", v.src[0], v.src[1]);
    ::memset(actual, 0xAB, sizeof(actual));

    res = utf16_to_utf8(v.src, std::size(v.src), actual, &encoded_len);
    ASSERT_OK(res, "%s", case_id);
    ASSERT_UTF8_EQ(v.expected, sizeof(v.expected), actual, sizeof(actual), encoded_len, case_id);
  }
}

TEST(UTF16To8TestCase, UnpairedSurrogates) {
  static const struct {
    uint16_t src;
    uint8_t expected[3];
  } TEST_VECTORS[] = {
      // All unpaired surrogates are technically supposed to be illegal, but
      // apparently there are systems out there who use them any (Wikipedia
      // claims that Windows allows unpaired surrogates in file names encoded
      // using UTF-16)
      //
      // Unpaired surrogates are 16 bits wide, so they will require a 3-byte
      // UTF-8 encoding.
      {0xD800, {0xED, 0xA0, 0x80}}, {0xD945, {0xED, 0xA5, 0x85}}, {0xDBFF, {0xED, 0xAF, 0xBF}},
      {0xDC00, {0xED, 0xB0, 0x80}}, {0xDD45, {0xED, 0xB5, 0x85}}, {0xDFFF, {0xED, 0xBF, 0xBF}},
  };
  uint8_t replace[3] = {0xEF, 0xBF, 0xBD};
  uint8_t actual[3];
  for (const auto& v : TEST_VECTORS) {
    char case_id[64];
    size_t encoded_len = sizeof(actual);
    zx_status_t res;

    // Attempt to encode the unpaired surrogate, but do not specify that we
    // want to preserve it.  We should end up with the encoded form of the
    // replacement character (U+FFFD) instead.
    snprintf(case_id, sizeof(case_id), "case id [0x%04hx, replace]", v.src);
    ::memset(actual, 0xAB, sizeof(actual));

    encoded_len = sizeof(actual);
    res = utf16_to_utf8(&v.src, 1, actual, &encoded_len);
    ASSERT_OK(res, "%s", case_id);
    ASSERT_UTF8_EQ(replace, sizeof(replace), actual, sizeof(actual), encoded_len, case_id);

    // Do it again, but this time tell the converter to preserve the
    // unpaired surrogate instead.
    snprintf(case_id, sizeof(case_id), "case id [0x%04hx, preserve]", v.src);
    ::memset(actual, 0xAB, sizeof(actual));

    encoded_len = sizeof(actual);
    res = utf16_to_utf8(&v.src, 1, actual, &encoded_len,
                        UTF_CONVERT_FLAG_PRESERVE_UNPAIRED_SURROGATES);
    ASSERT_OK(res, "%s", case_id);
    ASSERT_UTF8_EQ(v.expected, sizeof(v.expected), actual, sizeof(actual), encoded_len, case_id);
  }
}

TEST(UTF16To8TestCase, BufferLengths) {
  const uint16_t src[] = {'T', 'e', 's', 't'};
  const uint8_t expected[] = {'T', 'e', 's', 't'};
  uint8_t actual[16];

  // Perform a conversion, but test multiple cases.
  //
  // 1) The destination buffer size is exactly what is required.
  // 2) The destination buffer size is more than what is required.
  // 3) The destination buffer size is less than what is required.
  // 4) The destination buffer is NULL and buffer size is 0.
  static const size_t DST_LENGTHS[] = {sizeof(expected), sizeof(actual), sizeof(expected) >> 1, 0};
  for (const auto& d : DST_LENGTHS) {
    char case_id[64];
    size_t encoded_len = d;
    zx_status_t res;

    snprintf(case_id, sizeof(case_id), "case id [needed %zu, provided %zu]", sizeof(expected), d);
    ::memset(actual, 0xAB, sizeof(actual));

    ASSERT_LE(encoded_len, sizeof(actual), "%s", case_id);
    uint8_t* dest = (d == 0 ? nullptr : actual);
    res = utf16_to_utf8(src, std::size(src), dest, &encoded_len);

    ASSERT_OK(res, "%s", case_id);
    ASSERT_EQ(sizeof(expected), encoded_len, "%s", case_id);
    static_assert(sizeof(expected) <= sizeof(actual),
                  "'actual' buffer must be large enough to hold 'expected' result");
    ASSERT_BYTES_EQ(expected, actual, d < encoded_len ? d : encoded_len, "%s", case_id);

    if (d < sizeof(actual)) {
      uint8_t pattern[sizeof(actual)];
      ::memset(pattern, 0xAB, sizeof(pattern));
      ASSERT_BYTES_EQ(actual + d, pattern, sizeof(actual) - d, "%s", case_id);
    }
  }
}

TEST(UTF16To8TestCase, EndiannessAndBom) {
  static const struct {
    uint16_t src[5];
    bool host_order;
  } SOURCES[] = {{{0xFEFF, 'T', 'e', 's', 't'}, true},
                 {{
                      ByteSwap(0xFEFF),
                      ByteSwap('T'),
                      ByteSwap('e'),
                      ByteSwap('s'),
                      ByteSwap('t'),
                  },
                  false}};

  const uint8_t bom_removed[] = {'T', 'e', 's', 't'};
  const uint8_t bom_removed_inverted[] = {0xE5, 0x90, 0x80, 0xE6, 0x94, 0x80,
                                          0xE7, 0x8C, 0x80, 0xE7, 0x90, 0x80};
  const uint8_t bom_encoded[] = {0xEF, 0xBB, 0xBF, 'T', 'e', 's', 't'};
  const uint8_t bom_encoded_inverted[] = {0xEF, 0xBF, 0xBE, 0xE5, 0x90, 0x80, 0xE6, 0x94,
                                          0x80, 0xE7, 0x8C, 0x80, 0xE7, 0x90, 0x80};
  uint8_t actual[std::size(bom_encoded_inverted)];

#define EXPECT(e) \
  { e, sizeof(e) }
  static const struct {
    uint32_t flags;
    struct {
      const uint8_t* exp;
      size_t len;
    } host;
    struct {
      const uint8_t* exp;
      size_t len;
    } inv;
  } EXPECTED[]{
      {0, EXPECT(bom_encoded), EXPECT(bom_encoded)},
      {UTF_CONVERT_FLAG_DISCARD_BOM, EXPECT(bom_removed), EXPECT(bom_removed)},
      {HOST_ENDIAN_FLAG, EXPECT(bom_encoded), EXPECT(bom_encoded_inverted)},
      {HOST_ENDIAN_FLAG | UTF_CONVERT_FLAG_DISCARD_BOM, EXPECT(bom_removed),
       EXPECT(bom_removed_inverted)},
      {INVERT_ENDIAN_FLAG, EXPECT(bom_encoded_inverted), EXPECT(bom_encoded)},
      {INVERT_ENDIAN_FLAG | UTF_CONVERT_FLAG_DISCARD_BOM, EXPECT(bom_removed_inverted),
       EXPECT(bom_removed)},
  };
#undef EXPECT

  for (const auto& s : SOURCES) {
    for (const auto& e : EXPECTED) {
      char case_id[64];
      zx_status_t res;
      size_t enc_len = sizeof(actual);

      ::memset(actual, 0xAB, sizeof(actual));
      snprintf(case_id, sizeof(case_id), "case id [%s BOM, %s endian]",
               (e.flags & UTF_CONVERT_FLAG_DISCARD_BOM) ? "discard" : "encode",
               (e.flags & HOST_ENDIAN_FLAG)     ? "host"
               : (e.flags & INVERT_ENDIAN_FLAG) ? "invert"
                                                : "detect");

      res = utf16_to_utf8(s.src, std::size(s.src), actual, &enc_len, e.flags);
      ASSERT_OK(res, "%s", case_id);

      if (s.host_order) {
        ASSERT_UTF8_EQ(e.host.exp, e.host.len, actual, sizeof(actual), enc_len, case_id);
      } else {
        ASSERT_UTF8_EQ(e.inv.exp, e.inv.len, actual, sizeof(actual), enc_len, case_id);
      }
    }
  }
}

TEST(UTF8To16TestCase, SimpleCodepoints) {
  // Only one-byte code points are currently handled.
  constexpr uint8_t kExpected[] = {0x00, 0x01, 0x7f};
  for (const uint8_t expected : kExpected) {
    uint16_t actual[16];
    size_t encoded_len = std::size(actual);
    ASSERT_OK(utf8_to_utf16(&expected, 1, actual, &encoded_len));
    ASSERT_EQ(encoded_len, 1);
    ASSERT_EQ(actual[0], static_cast<uint16_t>(expected));
  }
}

TEST(UTF8To16TestCase, BufferLengths) {
  const uint8_t src[] = {'T', 'e', 's', 't'};
  const uint16_t expected[] = {'T', 'e', 's', 't'};
  uint16_t actual[16];

  // Perform a conversion, but test multiple cases.
  //
  // 1) The destination buffer size is exactly what is required.
  // 2) The destination buffer size is more than what is required.
  // 3) The destination buffer size is less than what is required.
  // 4) The destination buffer is NULL and buffer size is 0.
  constexpr size_t DST_LENGTHS[] = {std::size(expected), std::size(actual),
                                    std::size(expected) >> 1, 0};
  for (const auto& d : DST_LENGTHS) {
    char case_id[64];
    size_t encoded_len = d;
    zx_status_t res;

    snprintf(case_id, sizeof(case_id), "case id [needed %zu, provided %zu]", std::size(expected),
             d);
    ::memset(actual, 0xAB, sizeof(actual));

    ASSERT_LE(encoded_len, sizeof(actual), "%s", case_id);
    uint16_t* dest = (d == 0 ? nullptr : actual);
    res = utf8_to_utf16(src, std::size(src), dest, &encoded_len);

    ASSERT_OK(res, "%s", case_id);
    ASSERT_EQ(std::size(expected), encoded_len, "%s", case_id);
    static_assert(sizeof(expected) <= sizeof(actual),
                  "'actual' buffer must be large enough to hold 'expected' result");
    ASSERT_BYTES_EQ(expected, actual, std::min(d, encoded_len) * sizeof(uint16_t), "%s", case_id);

    if (d < std::size(actual)) {
      uint16_t pattern[sizeof(actual)];
      ::memset(pattern, 0xAB, sizeof(pattern));
      ASSERT_BYTES_EQ(actual + d, pattern, sizeof(actual) - (d * sizeof(uint16_t)), "%s", case_id);
    }
  }
}

}  // namespace
