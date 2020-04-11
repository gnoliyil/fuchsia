// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';

import '../../models/app_model.dart';
import '../../utils/elevations.dart';
import '../../utils/styles.dart';
import '../support/animation_driver.dart';
import 'status.dart';

/// Defines a widget to manage the visibility of the [Status] widget.
class StatusContainer extends StatelessWidget {
  /// The model that holds the state for this widget.
  final AppModel model;

  /// Constructor.
  const StatusContainer({@required this.model});

  @override
  Widget build(BuildContext context) {
    double bottom() => !model.isFullscreen || model.peekNotifier.value
        ? model.topbarModel.statusButtonRect.bottom
        : MediaQuery.of(context).size.height -
            ErmineStyle.kTopBarHeight -
            ErmineStyle.kStoryTitleHeight;
    final status = Material(
      key: model.statusModel.key,
      color: ErmineStyle.kBackgroundColor,
      elevation: Elevations.systemOverlayElevation,
      child: Container(
        width: 450,
        height: 300,
        padding: EdgeInsets.all(12),
        decoration: BoxDecoration(
          border: Border.all(color: ErmineStyle.kOverlayBorderColor),
        ),
        child: Status(model: model.statusModel),
      ),
    );
    return RepaintBoundary(
      child: Stack(
        children: <Widget>[
          AnimatedBuilder(
            animation: model.statusVisibility,
            builder: (context, child) => model.statusVisibility.value
                ? Positioned(
                    bottom: bottom(),
                    right: model.topbarModel.statusButtonRect.right,
                    child: AnimationDriver(
                      tween:
                          Tween<Offset>(begin: Offset(0, 0), end: Offset(0, 1)),
                      builder: (context, animation) => FractionalTranslation(
                        translation: animation.value,
                        child: status,
                      ),
                    ),
                  )
                : Offstage(),
          ),
        ],
      ),
    );
  }
}
