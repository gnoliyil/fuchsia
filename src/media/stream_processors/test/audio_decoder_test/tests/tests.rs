// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use audio_decoder_test_lib::{cvsd::*, sbc::*, test_suite::*};

use fidl::encoding::Decodable;
use fidl_fuchsia_media::*;
use fuchsia_async as fasync;
use std::rc::Rc;
use stream_processor_test::*;

const SBC_TEST_FILE: &str = "/pkg/data/s16le44100mono.sbc";

// INSTRUCTIONS FOR ADDING HASH TESTS
//
// 1. If adding a new encoded test file, check it into the `test_data` directory. It should only be
//    a few thousand PCM frames decoded.
// 2. Set the `output_file` field to write the decoded output into `/tmp/`. You can copy it to the
//    host with `fx scp [$(fx netaddr --fuchsia)]:/tmp/ ... `
// 3. Decode the test encoded stream with another decoder (for sbc, use sbcdec or
//    ffmpeg; for aac use faac)
// 4. Verify the output
//      a. If the codec should produce the exact same bytes for the same settings, `diff` the two
//         files.
//      b. If the codec is permitted to produce different encoded bytes for the same settings, do a
//         similarity check:
//           b1. Decode both the reference and our encoded stream (sbcdec, faad, etc)
//           b2. Import both tracks into Audacity
//           b3. Apply Effect > Invert to one track
//           b4. Select both tracks and Tracks > Mix > Mix and Render to New Track
//           b5. On the resulting track use Effect > Amplify and observe the new peak amplitude
// 5. If all looks good, commit the hash.

#[test]
fn sbc_decode() -> Result<()> {
    let output_format = FormatDetails {
        format_details_version_ordinal: Some(1),
        mime_type: Some("audio/pcm".to_string()),
        domain: Some(DomainFormat::Audio(AudioFormat::Uncompressed(AudioUncompressedFormat::Pcm(
            PcmFormat {
                pcm_mode: AudioPcmMode::Linear,
                bits_per_sample: 16,
                frames_per_second: 44100,
                channel_map: vec![AudioChannelId::Lf],
            },
        )))),
        ..<FormatDetails as Decodable>::new_empty()
    };

    let stream = Rc::new(TimestampedStream {
        source: SbcStream::from_file(
            SBC_TEST_FILE,
            /* codec_info */ &[0x82, 0x00, 0x00, 0x00],
            /* chunk_frames */ 1,
        )?,
        timestamps: 0..,
    });

    let sbc_tests = AudioDecoderTestCase {
        hash_tests: vec![AudioDecoderHashTest {
            output_file: None,
            stream: stream,
            output_packet_count: 23,
            expected_digests: vec![ExpectedDigest::new(
                "Pcm: 44.1kHz/16bit/Mono",
                "03b47b3ec7f7dcb41456c321377c61984966ea308b853d385194683e13f9836b",
            )],
            expected_output_format: output_format,
        }],
    };

    fasync::TestExecutor::new().unwrap().run_singlethreaded(sbc_tests.run())
}

#[test]
fn sbc_decode_large_input_chunk() -> Result<()> {
    let output_format2 = FormatDetails {
        format_details_version_ordinal: Some(1),
        mime_type: Some("audio/pcm".to_string()),
        domain: Some(DomainFormat::Audio(AudioFormat::Uncompressed(AudioUncompressedFormat::Pcm(
            PcmFormat {
                pcm_mode: AudioPcmMode::Linear,
                bits_per_sample: 16,
                frames_per_second: 44100,
                channel_map: vec![AudioChannelId::Lf],
            },
        )))),
        ..<FormatDetails as Decodable>::new_empty()
    };

    let large_input_chunk_stream = Rc::new(TimestampedStream {
        source: SbcStream::from_file(
            SBC_TEST_FILE,
            /* codec_info */ &[0x82, 0x00, 0x00, 0x00],
            /* chunk_frames */ 23,
        )?,
        timestamps: 0..,
    });

    let sbc_tests = AudioDecoderTestCase {
        hash_tests: vec![AudioDecoderHashTest {
            output_file: None,
            stream: large_input_chunk_stream,
            output_packet_count: 23,
            expected_digests: vec![ExpectedDigest::new(
                "Large chunk Pcm: 44.1kHz/16bit/Mono",
                "03b47b3ec7f7dcb41456c321377c61984966ea308b853d385194683e13f9836b",
            )],
            expected_output_format: output_format2,
        }],
    };

    fasync::TestExecutor::new().unwrap().run_singlethreaded(sbc_tests.run())
}

#[test]
fn cvsd_simple_decode() -> Result<()> {
    let output_format = FormatDetails {
        format_details_version_ordinal: Some(1),
        mime_type: Some("audio/pcm".to_string()),
        domain: Some(DomainFormat::Audio(AudioFormat::Uncompressed(AudioUncompressedFormat::Pcm(
            PcmFormat {
                pcm_mode: AudioPcmMode::Linear,
                bits_per_sample: 16,
                frames_per_second: 64000,
                channel_map: vec![AudioChannelId::Lf],
            },
        )))),
        ..<FormatDetails as Decodable>::new_empty()
    };

    let cvsd_tests = AudioDecoderTestCase {
        hash_tests: vec![AudioDecoderHashTest {
            output_file: None,
            stream: Rc::new(TimestampedStream {
                source: CvsdStream::from_data(vec![0b01010101], 1),
                timestamps: 0..,
            }),
            // Total number of expected decoded output bytes is 16.
            // Since the minimum output buffer size is 16 bytes, all the input
            // should have been decoded in 1 output packet.
            output_packet_count: 1,
            expected_digests: vec![ExpectedDigest::new_from_raw(
                "Simple test case",
                // Equivalent to eight int16 elements [10, 0, 9, -1, 9, -1, 9, -1].
                vec![10, 0, 0, 0, 9, 0, 255, 255, 9, 0, 255, 255, 9, 0, 255, 255],
            )],
            expected_output_format: output_format,
        }],
    };

    fasync::TestExecutor::new().unwrap().run_singlethreaded(cvsd_tests.run())
}
