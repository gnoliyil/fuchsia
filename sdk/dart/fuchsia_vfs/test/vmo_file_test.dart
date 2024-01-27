// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:typed_data';
import 'dart:convert' show utf8;

import 'package:fidl/fidl.dart';
import 'package:fidl_fuchsia_io/fidl_async.dart';
import 'package:fuchsia_vfs/vfs.dart';
import 'package:test/test.dart';
import 'package:zircon/zircon.dart';

void main() {
  InterfaceRequest<Node> _getNodeInterfaceRequest(FileProxy proxy) {
    return InterfaceRequest<Node>(proxy.ctrl.request().passChannel());
  }

  _ReadOnlyFile _createVmoFile(String str, openRights,
      [VmoSharingMode mode = VmoSharingMode.shareDuplicate]) {
    final SizedVmo sizedVmo =
        SizedVmo.fromUint8List(Uint8List.fromList(str.codeUnits));

    _ReadOnlyFile file = _ReadOnlyFile()
      ..vmoFile = VmoFile.readOnly(Vmo(sizedVmo.handle), mode)
      ..proxy = FileProxy();
    expect(
        file.vmoFile.connect(
            openRights, ModeType.$none, _getNodeInterfaceRequest(file.proxy)),
        ZX.OK);
    return file;
  }

  Future<void> _assertRead(FileProxy proxy, int bufSize, String expectedStr,
      {expectedStatus = ZX.OK}) async {
    if (expectedStatus == ZX.OK) {
      final data = await proxy.read(bufSize);
      expect(String.fromCharCodes(data), expectedStr);
    } else {
      await expectLater(
          proxy.read(bufSize),
          throwsA(isA<MethodException>()
              .having((e) => e.value, 'value', equals(expectedStatus))));
    }
  }

  Future<void> _assertDescribeFile(FileProxy proxy) async {
    var response = await proxy.query();
    expect(utf8.decode(response), fileProtocolName);
  }

  Future<void> _assertDescribeVmo(FileProxy proxy, String expectedStr) async {
    await _assertDescribeFile(proxy);

    final vmo = await proxy.getBackingMemory(VmoFlags.$none);
    expect(vmo.isValid, isTrue);
    final Uint8List vmoData = vmo.map();
    expect(String.fromCharCodes(vmoData.sublist(0, expectedStr.length)),
        expectedStr);
  }

  group('vmo file:', () {
    test('onOpen event on success', () async {
      var file = _createVmoFile(
          'test_str', OpenFlags.rightReadable | OpenFlags.describe);

      await file.proxy.onOpen.first.then((response) {
        expect(response.s, ZX.OK);
        expect(response.info, isNotNull);
      }).catchError((err) async {
        fail(err.toString());
      });
    });

    test('onOpen with describe flag', () async {
      var file = _createVmoFile(
          'test_str', OpenFlags.rightReadable | OpenFlags.describe);

      await file.proxy.onOpen.first.then((response) {
        expect(response.s, ZX.OK);
        NodeInfoDeprecated? nodeInfo = response.info;
        if (nodeInfo != null) {
          expect(nodeInfo.file, isNotNull);
        }
      }).catchError((err) async {
        fail(err.toString());
      });
    });

    test('read file', () async {
      var str = 'test_str';
      var file = _createVmoFile(str, OpenFlags.rightReadable);
      await _assertRead(file.proxy, str.length, str);
    });

    test('describe duplicate', () async {
      var str = 'test_str';
      var file = _createVmoFile(str, OpenFlags.rightReadable);
      await _assertDescribeVmo(file.proxy, str);
    });

    test('describe no sharing', () async {
      var str = 'test_str';
      var file = _createVmoFile(
          str, OpenFlags.rightReadable, VmoSharingMode.noSharing);
      await _assertDescribeFile(file.proxy);
    });
  });
}

class _ReadOnlyFile {
  late VmoFile vmoFile;
  late FileProxy proxy;
}
