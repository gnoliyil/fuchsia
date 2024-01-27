// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert' show utf8;

import 'package:fidl/fidl.dart' hide Service;
import 'package:fidl_fuchsia_io/fidl_async.dart' as io;
import 'package:fuchsia_vfs/vfs.dart';
import 'package:test/test.dart';
import 'package:zircon/zircon.dart';

void main() {
  group('composed pseudo dir: ', () {
    late PseudoDir pseudoDir;
    late io.DirectoryProxy directory;

    setUp(() {
      pseudoDir = PseudoDir();
      directory = io.DirectoryProxy();
      pseudoDir.serve(
          InterfaceRequest<io.Node>(directory.ctrl.request().passChannel()));
    });

    tearDown(() {
      pseudoDir.close();
      directory.ctrl.close();
    });

    test('inherited files are opened', () async {
      pseudoDir.addNode('foo.txt', PseudoFile.readOnlyStr(() => 'hello world'));
      final composedDir =
          ComposedPseudoDir(directory: directory, inheritedNodes: ['foo.txt']);

      // connect to the file through the composed directory
      final fileProxy = io.FileProxy();
      composedDir.open(io.OpenFlags.rightReadable, io.modeTypeFile, 'foo.txt',
          InterfaceRequest<io.Node>(fileProxy.ctrl.request().passChannel()));

      final data = await fileProxy.read(io.maxBuf);
      expect(utf8.decode(data), 'hello world');
    });

    test('inherited services are opened', () async {
      final completer = Completer();
      pseudoDir.addNode('test.foo', Service.withConnector((_) {
        completer.complete();
      }));

      final composedDir =
          ComposedPseudoDir(directory: directory, inheritedNodes: ['test.foo']);

      // connect to the service through the composed directory
      final proxy = AsyncProxy(AsyncProxyController());
      composedDir.open(io.OpenFlags.$none, io.modeTypeService, 'test.foo',
          InterfaceRequest<io.Node>(proxy.ctrl.request().passChannel()));

      expect(() => completer.future, returnsNormally);
    }, timeout: Timeout(Duration(milliseconds: 500)));

    test('file exists in composedDir but not directory can be opened',
        () async {
      final composedDir = ComposedPseudoDir(directory: directory)
        ..addNode('foo.txt', PseudoFile.readOnlyStr(() => 'hello world'));

      // connect to the file through the composed directory
      final fileProxy = io.FileProxy();
      composedDir.open(io.OpenFlags.rightReadable, io.modeTypeFile, 'foo.txt',
          InterfaceRequest<io.Node>(fileProxy.ctrl.request().passChannel()));

      final data = await fileProxy.read(io.maxBuf);
      expect(utf8.decode(data), 'hello world');
    });

    test('adding a node which is already included in inherited nodes fails',
        () {
      final composedDir =
          ComposedPseudoDir(directory: directory, inheritedNodes: ['foo.txt']);
      expect(composedDir.addNode('foo.txt', PseudoFile.readOnlyStr(() => '')),
          ZX.ERR_ALREADY_EXISTS);
    });

    test('Nodes not included in inheritedNodes are not opened', () async {
      pseudoDir.addNode('foo.txt', PseudoFile.readOnlyStr(() => ''));

      final composedDir = ComposedPseudoDir(directory: directory);

      // connect to the file through the composed directory
      final fileProxy = io.FileProxy();
      composedDir.open(io.OpenFlags.rightReadable, io.modeTypeFile, 'foo.txt',
          InterfaceRequest<io.Node>(fileProxy.ctrl.request().passChannel()));

      await expectLater(
          fileProxy.read(io.maxBuf), throwsA(isA<FidlStateException>()));
    });

    test('cannot change list of inherited nodes after creation', () async {
      final inheritedNodes = <String>[];
      pseudoDir.addNode('foo.txt', PseudoFile.readOnlyStr(() => ''));

      final composedDir = ComposedPseudoDir(
          directory: directory, inheritedNodes: inheritedNodes);

      inheritedNodes.add('foo.txt');

      // connect to the file through the composed directory
      final fileProxy = io.FileProxy();
      composedDir.open(io.OpenFlags.rightReadable, io.modeTypeFile, 'foo.txt',
          InterfaceRequest<io.Node>(fileProxy.ctrl.request().passChannel()));

      await expectLater(
          fileProxy.read(io.maxBuf), throwsA(isA<FidlStateException>()));
    });
  });
}
