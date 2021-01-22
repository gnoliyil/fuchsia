// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: deprecated_member_use

import 'package:flutter/material.dart';
import 'package:fuchsia_logger/logger.dart';

void main() {
  setupLogger(name: 'button_flutter');

  // TODO(fxb/68176): Migrate use of FlatButton to TextButton.
  final app = MaterialApp(
    home: Material(
      color: Colors.white,
      child: Container(
        child: FlatButton(
          child: Text('I am a button'),
          onPressed: () {
            log.info('FlatButton.onPressed');
          },
        ),
        alignment: Alignment(0.0, 0.0),
      ),
    ),
  );

  runApp(app);
}
