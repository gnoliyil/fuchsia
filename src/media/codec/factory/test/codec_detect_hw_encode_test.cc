// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/default.h>

#include "../codec_factory_app.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

using CodecFactoryHwDetectTest = ::gtest::RealLoopFixture;

TEST_F(CodecFactoryHwDetectTest, H264EncoderPresent) {
  CodecFactoryApp app(dispatcher(), CodecFactoryApp::ProdOrTest::kTesting);

  // Loop needs to run till hw codec are fully discovered, so run test till that happens.
  RunLoopUntil([&app]() {
    auto factory = app.FindHwCodec(
        [](const fuchsia::mediacodec::CodecDescription& hw_codec_description) -> bool {
          std::string mime_type = "video/h264";
          return (fuchsia::mediacodec::CodecType::ENCODER == hw_codec_description.codec_type) &&
                 (mime_type == hw_codec_description.mime_type);
        });
    return factory != nullptr;
  });
  loop().Shutdown();
}

TEST_F(CodecFactoryHwDetectTest, H265EncoderPresent) {
  CodecFactoryApp app(dispatcher(), CodecFactoryApp::ProdOrTest::kTesting);

  // Loop needs to run till hw codec are fully discovered, so run test till that happens.
  RunLoopUntil([&app]() {
    auto factory = app.FindHwCodec(
        [](const fuchsia::mediacodec::CodecDescription& hw_codec_description) -> bool {
          std::string mime_type = "video/h265";
          return (fuchsia::mediacodec::CodecType::ENCODER == hw_codec_description.codec_type) &&
                 (mime_type == hw_codec_description.mime_type);
        });
    return factory != nullptr;
  });
  loop().Shutdown();
}
