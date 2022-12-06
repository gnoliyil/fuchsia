// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';
import 'package:internationalization/strings.dart';
import 'package:shell_settings/src/states/settings_state.dart';
import 'package:shell_settings/src/widgets/setting_details.dart';

/// Defines a widget to list all keyboard mappings in [SettingDetails] widget.
class KeyboardSettings extends StatelessWidget {
  final SettingsState state;
  final ValueChanged<String> onChange;

  const KeyboardSettings({required this.state, required this.onChange});

  @override
  Widget build(BuildContext context) {
    final keymapIds = state.supportedKeymaps;
    return SettingDetails(
      title: Strings.keyboard,
      onBack: state.showAllSettings,
      child: ListView.builder(
          itemCount: keymapIds.length,
          itemBuilder: (context, index) {
            final keymapId = keymapIds[index];
            return ListTile(
              title: Text(keymapId),
              subtitle: state.currentKeymap == keymapId
                  ? Text(Strings.selected)
                  : null,
              onTap: () => onChange(keymapIds[index]),
            );
          }),
    );
  }
}
