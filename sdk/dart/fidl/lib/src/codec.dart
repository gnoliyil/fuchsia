// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:math';
import 'dart:typed_data';

import 'package:zircon/zircon.dart';

import 'error.dart';
import 'message.dart';
import 'wire_format.dart';

// ignore_for_file: always_specify_types
// ignore_for_file: avoid_positional_boolean_parameters
// ignore_for_file: public_member_api_docs

const int _kAlignment = 8;
const int _kAlignmentMask = _kAlignment - 1;

const int _maxOutOfLineDepth = 32;

int align(int size) => (size + _kAlignmentMask) & ~_kAlignmentMask;

void _checkRange(int value, int min, int max) {
  if (value < min || value > max) {
    throw FidlRangeCheckError(value, min, max);
  }
}

const int _kInitialBufferSize = 512;
const int _kMinBufferSizeIncreaseFactor = 2;

class Encoder {
  Encoder(this.wireFormat);

  OutgoingMessage get message {
    encodeUint8(kWireFormatV2FlagMask, kMessageFlagOffset);
    final ByteData trimmed = ByteData.view(data.buffer, 0, _extent);
    return OutgoingMessage(trimmed, _handleDispositions);
  }

  ByteData data = ByteData(_kInitialBufferSize);
  final List<HandleDisposition> _handleDispositions = <HandleDisposition>[];
  int _extent = 0;
  WireFormat wireFormat;

  void _grow(int newSize) {
    final Uint8List newList = Uint8List(newSize)
      ..setRange(0, data.lengthInBytes, data.buffer.asUint8List());
    data = newList.buffer.asByteData();
  }

  void _claimBytes(int claimSize) {
    _extent += claimSize;
    if (_extent > data.lengthInBytes) {
      int newSize =
          max(_extent, _kMinBufferSizeIncreaseFactor * data.lengthInBytes);
      _grow(newSize);
    }
  }

  int alloc(int size, int nextOutOfLineDepth) {
    if (nextOutOfLineDepth > _maxOutOfLineDepth) {
      throw FidlError('Exceeded maxOutOfLineDepth',
          FidlErrorCode.fidlExceededMaxOutOfLineDepth);
    }
    int offset = _extent;
    _claimBytes(align(size));
    return offset;
  }

  int nextOffset() {
    return _extent;
  }

  int countHandles() {
    return _handleDispositions.length;
  }

  void addHandleDisposition(HandleDisposition value) {
    _handleDispositions.add(value);
  }

  void encodeMessageHeader(int ordinal, int txid, CallStrictness strictness) {
    alloc(kMessageHeaderSize, 0);
    encodeUint32(txid, kMessageTxidOffset);
    encodeUint8(kWireFormatV2FlagMask, kMessageFlagOffset);
    encodeUint8(0, kMessageFlagOffset + 1);
    encodeUint8(strictnessToFlags(strictness), kMessageDynamicFlagOffset);
    encodeUint8(kMagicNumberInitial, kMessageMagicOffset);
    encodeUint64(ordinal, kMessageOrdinalOffset);
  }

  /// Produces a response for an UnknownMethod which was called with the given
  /// ordinal value for the method and the given transaction ID. This produces
  /// both a message header and an encoded body. The header will have the
  /// provided ordinal and txid and CallStrictness.flexible (this should never
  /// be used with a strict method). The body will contain a union with ordinal
  /// 3 and a value of type zx_status with the NOT_SUPPORTED error code.
  void encodeUnknownMethodResponse(int methodOrdinal, int txid) {
    encodeMessageHeader(methodOrdinal, txid, CallStrictness.flexible);

    const int kUnknownMethodInlineSize = 16;
    const int kEnvelopeOffset = kMessageHeaderSize + 8;
    const int kTransportErrOrdinal = 3;
    alloc(kUnknownMethodInlineSize, 0);
    // Union header.
    // transport_err value for the union's ordinal.
    encodeUint64(kTransportErrOrdinal, kMessageHeaderSize);
    // Inline value of the zx_status.
    encodeInt32(ZX.ERR_NOT_SUPPORTED, kEnvelopeOffset);
    // Number of handles in the envelope.
    encodeUint16(0, kEnvelopeOffset + 4);
    // Flags field, with tag indicating the value is stored in-line (in what
    // would otherwise be the size field).
    encodeUint16(kEnvelopeInlineMarker, kEnvelopeOffset + 6);
  }

  void encodeBool(bool value, int offset) {
    data.setInt8(offset, value ? 1 : 0);
  }

  void encodeInt8(int value, int offset) {
    _checkRange(value, -128, 127);
    data.setInt8(offset, value);
  }

  void encodeUint8(int value, int offset) {
    _checkRange(value, 0, 255);
    data.setUint8(offset, value);
  }

  void encodeInt16(int value, int offset) {
    _checkRange(value, -32768, 32767);
    data.setInt16(offset, value, Endian.little);
  }

  void encodeUint16(int value, int offset) {
    _checkRange(value, 0, 65535);
    data.setUint16(offset, value, Endian.little);
  }

  void encodeInt32(int value, int offset) {
    _checkRange(value, -2147483648, 2147483647);
    data.setInt32(offset, value, Endian.little);
  }

  void encodeUint32(int value, int offset) {
    _checkRange(value, 0, 4294967295);
    data.setUint32(offset, value, Endian.little);
  }

  void encodeInt64(int value, int offset) {
    data.setInt64(offset, value, Endian.little);
  }

  void encodeUint64(int value, int offset) {
    data.setUint64(offset, value, Endian.little);
  }

  void encodeFloat32(double value, int offset) {
    data.setFloat32(offset, value, Endian.little);
  }

  void encodeFloat64(double value, int offset) {
    data.setFloat64(offset, value, Endian.little);
  }
}

class Decoder {
  Decoder(IncomingMessage message)
      : data = message.data,
        handleInfos = message.handleInfos,
        wireFormat = message.parseWireFormat();

  Decoder.fromRawArgs(this.data, this.handleInfos, this.wireFormat);

  ByteData data;
  List<HandleInfo> handleInfos;

  int _nextOffset = 0;
  int _nextHandle = 0;

  WireFormat wireFormat;

  int nextOffset() {
    return _nextOffset;
  }

  int claimBytes(int size, int nextOutOfLineDepth) {
    if (nextOutOfLineDepth > _maxOutOfLineDepth) {
      throw FidlError('Exceeded maxOutOfLineDepth',
          FidlErrorCode.fidlExceededMaxOutOfLineDepth);
    }
    final int result = _nextOffset;
    _nextOffset += align(size);
    if (_nextOffset > data.lengthInBytes) {
      throw FidlError(
          'Cannot access out of range memory', FidlErrorCode.fidlTooFewBytes);
    }
    return result;
  }

  int countUnclaimedBytes() {
    return data.lengthInBytes - _nextOffset;
  }

  int countClaimedHandles() {
    return _nextHandle;
  }

  int countUnclaimedHandles() {
    return handleInfos.length - _nextHandle;
  }

  HandleInfo claimHandle() {
    if (_nextHandle >= handleInfos.length) {
      throw FidlError(
          'Cannot access out of range handle', FidlErrorCode.fidlTooFewHandles);
    }
    return handleInfos[_nextHandle++];
  }

  bool decodeBool(int offset) {
    switch (data.getUint8(offset)) {
      case 0:
        return false;
      case 1:
        return true;
      default:
        throw FidlError('Invalid boolean', FidlErrorCode.fidlInvalidBoolean);
    }
  }

  int decodeInt8(int offset) => data.getInt8(offset);

  int decodeUint8(int offset) => data.getUint8(offset);

  int decodeInt16(int offset) => data.getInt16(offset, Endian.little);

  int decodeUint16(int offset) => data.getUint16(offset, Endian.little);

  int decodeInt32(int offset) => data.getInt32(offset, Endian.little);

  int decodeUint32(int offset) => data.getUint32(offset, Endian.little);

  int decodeInt64(int offset) => data.getInt64(offset, Endian.little);

  int decodeUint64(int offset) => data.getUint64(offset, Endian.little);

  double decodeFloat32(int offset) => data.getFloat32(offset, Endian.little);

  double decodeFloat64(int offset) => data.getFloat64(offset, Endian.little);

  void checkPadding(int offset, int padding) {
    for (int readAt = offset; readAt < offset + padding; readAt++) {
      if (data.getUint8(readAt) != 0) {
        throw FidlError('Non-zero padding at: $readAt',
            FidlErrorCode.fidlInvalidPaddingByte);
      }
    }
  }
}
