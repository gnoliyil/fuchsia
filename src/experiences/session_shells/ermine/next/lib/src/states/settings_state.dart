// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:mobx/mobx.dart';

import 'package:next/src/services/settings/datetime_service.dart';
import 'package:next/src/services/settings/task_service.dart';
import 'package:next/src/services/settings/timezone_service.dart';
import 'package:next/src/services/shortcuts_service.dart';
import 'package:next/src/states/settings_state_impl.dart';
import 'package:next/src/widgets/quick_settings.dart';
import 'package:next/src/widgets/settings/setting_details.dart';

enum WiFiStrength { off, searching, weak, good, strong, error }

enum BatteryCharge { missing, charging, discharging, error }

/// Defines the pages that have a [SettingDetails] widget.
enum SettingsPage {
  none,
  wifi,
  bluetooth,
  channel,
  timezone,
  shortcuts,
  feedback,
  opensource,
}

/// Defines the state of the [QuickSettings] overlay.
abstract class SettingsState with Store implements TaskService {
  ObservableValue<bool> get allSettingsPageVisible;
  ObservableValue<bool> get shortcutsPageVisible;
  ObservableValue<bool> get timezonesPageVisible;
  ObservableValue<WiFiStrength> get wifiStrength;
  ObservableValue<BatteryCharge> get batteryCharge;
  ObservableValue<String> get dateTime;
  ObservableValue<String> get selectedTimezone;
  Map<String, Set<String>> get shortcutBindings;
  List<String> get timezones;

  factory SettingsState.from({required ShortcutsService shortcutsService}) {
    return SettingsStateImpl(
      shortcutsService: shortcutsService,
      timezoneService: TimezoneService(),
      dateTimeService: DateTimeService(),
    );
  }

  Action get updateTimezone;
  Action get showAllSettings;
  Action get showShortcutSettings;
  Action get showTimezoneSettings;
}
