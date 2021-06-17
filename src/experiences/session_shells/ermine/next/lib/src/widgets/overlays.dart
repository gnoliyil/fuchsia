// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/widgets.dart';
import 'package:flutter_mobx/flutter_mobx.dart';

import 'package:next/src/states/app_state.dart';
import 'package:next/src/widgets/app_bar.dart';
import 'package:next/src/widgets/scrim.dart';
import 'package:next/src/widgets/side_bar.dart';

/// Defines a widget to hold all top-level overlays.
class Overlays extends StatelessWidget {
  final AppState state;

  const Overlays(this.state);

  @override
  Widget build(BuildContext context) {
    return Observer(builder: (_) {
      return Stack(
        children: [
          // Scrim layer.
          Scrim(state),

          // App Bar.
          if (state.appBarVisible.value)
            Positioned(
              top: 0,
              bottom: 0,
              left: 0,
              child: AppBar(state),
            ),

          // Side Bar.
          if (state.sideBarVisible.value)
            Positioned(
              top: 0,
              bottom: 0,
              right: 0,
              child: SideBar(state),
            ),
        ],
      );
    });
  }
}
