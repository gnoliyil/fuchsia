// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// @dart=2.12

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:sl4f/sl4f.dart';
import 'package:test/test.dart';

void main(List<String> args) {
  late HttpServer fakeServer;
  late Sl4f sl4f;

  setUp(() async {
    fakeServer = await HttpServer.bind('127.0.0.1', 18080);
    sl4f = Sl4f('127.0.0.1', null, 18080);
  });

  tearDown(() async {
    await fakeServer.close();
  });

  group(FactoryStore, () {
    test('listFiles makes request and receives file list', () async {
      const expectedAlphaFiles = [
        'alpha_file1',
        'some/dir/alpha_file2',
        'alpha_file3',
        'another/dir/alpha_file4'
      ];
      const expectedCastFiles = ['cast_file1', 'some/dir/cast_file2'];
      const expectedMiscFiles = ['misc_file1', 'some/dir/misc_file2'];
      const expectedPlayreadyFiles = [
        'playready_file1',
        'some/dir/playready_file2'
      ];
      const expectedWeaveFiles = [
        'weave_file1',
        'some/dir/weave_file2',
        'weave_file3',
        'another/dir/weave_file4'
      ];
      const expectedWidevineFiles = [
        'widevine_file1',
        'some/dir/widevine_file2'
      ];

      final returnedFiles = {
        'alpha': expectedAlphaFiles,
        'cast': expectedCastFiles,
        'misc': expectedMiscFiles,
        'playready': expectedPlayreadyFiles,
        'weave': expectedWeaveFiles,
        'widevine': expectedWidevineFiles
      };

      void handler(HttpRequest req) async {
        final body = jsonDecode(await utf8.decoder.bind(req).join());
        expect(body['method'], 'factory_store_facade.ListFiles');
        final params = body['params'];
        final files = returnedFiles[params['provider']];
        req.response.write(
            jsonEncode({'id': body['id'], 'result': files, 'error': null}));
        await req.response.close();
      }

      fakeServer.listen(handler);

      final sl4fFactory = FactoryStore(sl4f);
      final List<String> alphaFiles =
          await sl4fFactory.listFiles(FactoryStoreProvider.alpha);
      final List<String> castFiles =
          await sl4fFactory.listFiles(FactoryStoreProvider.cast);
      final List<String> miscFiles =
          await sl4fFactory.listFiles(FactoryStoreProvider.misc);
      final List<String> playreadyFiles =
          await sl4fFactory.listFiles(FactoryStoreProvider.playready);
      final List<String> weaveFiles =
          await sl4fFactory.listFiles(FactoryStoreProvider.weave);
      final List<String> widevineFiles =
          await sl4fFactory.listFiles(FactoryStoreProvider.widevine);

      expect(alphaFiles, containsAllInOrder(expectedAlphaFiles));
      expect(castFiles, containsAllInOrder(expectedCastFiles));
      expect(miscFiles, containsAllInOrder(expectedMiscFiles));
      expect(playreadyFiles, containsAllInOrder(expectedPlayreadyFiles));
      expect(weaveFiles, containsAllInOrder(expectedWeaveFiles));
      expect(widevineFiles, containsAllInOrder(expectedWidevineFiles));
    });

    test('readFile makes request and receives file contents', () async {
      const alphaFilename = 'alpha_file1';
      final expectedAlphaFileContents =
          base64Encode(utf8.encode('alpha_file1'));
      const castFilename = 'cast_file1';
      final expectedCastFileContents = base64Encode(utf8.encode('cast_file1'));
      const miscFilename = 'misc_file1';
      final expectedMiscFileContents = base64Encode(utf8.encode('misc_file1'));
      const playreadyFilename = 'playready_file1';
      final expectedPlayreadyFileContents =
          base64Encode(utf8.encode('playready_file1'));
      const weaveFilename = 'weave_file1';
      final expectedWeaveFileContents =
          base64Encode(utf8.encode('weave_file1'));
      const widevineFilename = 'widevine_file1';
      final expectedWidevineFileContents =
          base64Encode(utf8.encode('widevine_file1'));

      final returnedFileContents = {
        'alpha': {alphaFilename: expectedAlphaFileContents},
        'cast': {castFilename: expectedCastFileContents},
        'misc': {miscFilename: expectedMiscFileContents},
        'playready': {playreadyFilename: expectedPlayreadyFileContents},
        'weave': {weaveFilename: expectedWeaveFileContents},
        'widevine': {widevineFilename: expectedWidevineFileContents}
      };

      void handler(HttpRequest req) async {
        final body = jsonDecode(await utf8.decoder.bind(req).join());
        expect(body['method'], 'factory_store_facade.ReadFile');

        final params = body['params'];
        final provider = params['provider'];
        final filename = params['filename'];

        final fileContents = returnedFileContents[provider]![filename];
        req.response.write(jsonEncode(
            {'id': body['id'], 'result': fileContents, 'error': null}));
        await req.response.close();
      }

      fakeServer.listen(handler);

      final sl4fFactory = FactoryStore(sl4f);
      final Uint8List alphaFiles =
          await sl4fFactory.readFile(FactoryStoreProvider.alpha, alphaFilename);
      final Uint8List castFiles =
          await sl4fFactory.readFile(FactoryStoreProvider.cast, castFilename);
      final Uint8List miscFiles =
          await sl4fFactory.readFile(FactoryStoreProvider.misc, miscFilename);
      final Uint8List playreadyFiles = await sl4fFactory.readFile(
          FactoryStoreProvider.playready, playreadyFilename);
      final Uint8List weaveFiles =
          await sl4fFactory.readFile(FactoryStoreProvider.weave, weaveFilename);
      final Uint8List widevineFiles = await sl4fFactory.readFile(
          FactoryStoreProvider.widevine, widevineFilename);

      expect(alphaFiles, base64Decode(expectedAlphaFileContents));
      expect(castFiles, base64Decode(expectedCastFileContents));
      expect(miscFiles, base64Decode(expectedMiscFileContents));
      expect(playreadyFiles, base64Decode(expectedPlayreadyFileContents));
      expect(weaveFiles, base64Decode(expectedWeaveFileContents));
      expect(widevineFiles, base64Decode(expectedWidevineFileContents));
    });
  });
}
