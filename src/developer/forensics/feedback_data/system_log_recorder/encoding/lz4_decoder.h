// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_SYSTEM_LOG_RECORDER_ENCODING_LZ4_DECODER_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_SYSTEM_LOG_RECORDER_ENCODING_LZ4_DECODER_H_

#include <memory>
#include <optional>
#include <string>

#include <lz4/lz4.h>

#include "src/developer/forensics/feedback_data/system_log_recorder/encoding/decoder.h"
#include "src/developer/forensics/feedback_data/system_log_recorder/encoding/ring_buffer.h"
#include "src/developer/forensics/feedback_data/system_log_recorder/encoding/version.h"

namespace forensics {
namespace feedback_data {
namespace system_log_recorder {

// The Lz4Decoder::Decode() decodes a block previously encoded with the Lz4Encoder. The block is
// processed one chunk at a time as required by LZ4. One chunk is created on every invocation to
// LZ4_compress_fast_continue(). The decoding algorithm further requires that the previous 64KB of
// decoded data remain in memory (unchanged) thus a ring buffer is used for this purpose. The ring
// buffer wraps around when there is not enough data left and we guarantee that there is at least
// 64KB of previous decoded data (it is very likely that the decoded data will be larger than 64KB
// if the encoded block size is 64KB and the compression ratio is greater than 1x). In addition,
// the state for the current block decompression needed by the LZ4 algorithm is kept in the
// “stream” variable.
//
// |output_preallocation_hint| allows callers to suggest how the decoder should preallocate its
// output string when decoding.
class Lz4Decoder : public Decoder {
 public:
  explicit Lz4Decoder(std::optional<size_t> output_preallocation_hint = std::nullopt);

  virtual ~Lz4Decoder() = default;

  virtual EncodingVersion GetEncodingVersion() const { return EncodingVersion::kLz4; }

  // |Decoder|
  virtual std::string Decode(const std::string& block);

 protected:
  // Increases testing flow control.
  //
  // Decoding a block automatically resets the decoder. For testing however it is useful to decode
  // every message. This is because decoding large blocks can spam the test logs with tens of
  // thousands of characters and finding when or how a test fails becomes needlessly onerous.
  // Breaking a large block into smaller blocks also decreases the probability of finding errors
  // since the encoder, the decoder and the buffers get reset on every block.
  std::string DecodeWithoutReset(const std::string& chunk);

  void Reset();

 private:
  RingBuffer ring_;
  std::unique_ptr<LZ4_streamDecode_t, decltype(&LZ4_freeStreamDecode)> stream_;
  std::optional<size_t> output_preallocation_hint_;
};

}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_DATA_SYSTEM_LOG_RECORDER_ENCODING_LZ4_DECODER_H_
