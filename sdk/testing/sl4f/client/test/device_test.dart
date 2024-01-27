// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// @dart=2.12

import 'dart:convert';
import 'dart:io';

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

  test('call GetDeivceName facade with no params', () {
    void handler(HttpRequest req) async {
      expect(req.contentLength, greaterThan(0));
      final body = jsonDecode(await utf8.decoder.bind(req).join());
      expect(body['method'], 'device_facade.GetDeviceName');
      expect(body['params'], null);
      req.response.write(jsonEncode(
          {'id': body['id'], 'result': 'fake-device-name', 'error': null}));
      await req.response.close();
    }

    fakeServer.listen(handler);

    expect(
        Device(sl4f).getDeviceName(), completion(equals('fake-device-name')));
  });
}
