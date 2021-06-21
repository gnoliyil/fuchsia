// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io';

import 'package:intl/intl.dart';
import 'package:mobx/mobx.dart';

import 'package:next/src/services/settings/datetime_service.dart';
import 'package:next/src/services/settings/network_address_service.dart';
import 'package:next/src/services/settings/task_service.dart';
import 'package:next/src/services/settings/timezone_service.dart';
import 'package:next/src/services/shortcuts_service.dart';
import 'package:next/src/states/settings_state.dart';
import 'package:next/src/utils/mobx_disposable.dart';
import 'package:next/src/utils/mobx_extensions.dart';

/// Defines the implementation of [SettingsState].
class SettingsStateImpl with Disposable implements SettingsState, TaskService {
  static const kTimezonesFile = '/pkg/data/tz_ids.txt';

  final settingsPage = SettingsPage.none.asObservable();

  @override
  late final shortcutsPageVisible =
      (() => settingsPage.value == SettingsPage.shortcuts).asComputed();

  @override
  late final allSettingsPageVisible =
      (() => settingsPage.value == SettingsPage.none).asComputed();

  @override
  late final timezonesPageVisible =
      (() => settingsPage.value == SettingsPage.timezone).asComputed();

  @override
  final wifiStrength = Observable<WiFiStrength>(WiFiStrength.good);

  @override
  final batteryCharge = Observable<BatteryCharge>(BatteryCharge.charging);

  @override
  final Map<String, Set<String>> shortcutBindings;

  @override
  final Observable<String> selectedTimezone;

  @override
  final networkAddresses = ObservableList<String>();

  final List<String> _timezones;

  @override
  List<String> get timezones {
    // Move the selected timezone to the top.
    return [selectedTimezone.value]
      ..addAll(_timezones.where((zone) => zone != selectedTimezone.value));
  }

  @override
  late final ObservableValue<String> dateTime = (() =>
      // Ex: Mon, Jun 7 2:25 AM
      DateFormat.MMMEd().add_jm().format(dateTimeNow.value)).asComputed();

  final DateTimeService dateTimeService;
  final TimezoneService timezoneService;
  final NetworkAddressService networkService;

  SettingsStateImpl({
    required ShortcutsService shortcutsService,
    required this.timezoneService,
    required this.dateTimeService,
    required this.networkService,
  })  : shortcutBindings = shortcutsService.keyboardBindings,
        _timezones = _loadTimezones(),
        selectedTimezone = timezoneService.timezone.asObservable() {
    dateTimeService.onChanged = updateDateTime;
    timezoneService.onChanged =
        (timezone) => runInAction(() => selectedTimezone.value = timezone);
    networkService.onChanged = () => NetworkInterface.list().then((interfaces) {
          // Gather all addresses from all interfaces and sort them such that
          // IPv4 addresses come before IPv6.
          final addresses = interfaces
              .expand((interface) => interface.addresses)
              .toList(growable: false)
                ..sort((addr1, addr2) =>
                    addr1.type == InternetAddressType.IPv4 ? -1 : 0);

          runInAction(() => networkAddresses
            ..clear()
            ..addAll(addresses.map((address) => address.address)));
        });
  }

  @override
  Future<void> start() async {
    await Future.wait([
      dateTimeService.start(),
      timezoneService.start(),
      networkService.start(),
    ]);
  }

  @override
  Future<void> stop() async {
    showAllSettings();
    await dateTimeService.stop();
    await timezoneService.stop();
    await networkService.stop();
    _dateTimeNow = null;
  }

  @override
  void dispose() {
    super.dispose();
    dateTimeService.dispose();
    timezoneService.dispose();
  }

  @override
  late final Action updateTimezone = (timezone) {
    selectedTimezone.value = timezone;
    timezoneService.timezone = timezone;
    settingsPage.value = SettingsPage.none;
  }.asAction();

  @override
  late final Action showAllSettings = () {
    settingsPage.value = SettingsPage.none;
  }.asAction();

  @override
  late final Action showShortcutSettings = () {
    settingsPage.value = SettingsPage.shortcuts;
  }.asAction();

  @override
  late final Action showTimezoneSettings = () {
    settingsPage.value = SettingsPage.timezone;
  }.asAction();

  Observable<DateTime>? _dateTimeNow;
  Observable<DateTime> get dateTimeNow =>
      _dateTimeNow ??= DateTime.now().asObservable();

  late final Action updateDateTime = () {
    dateTimeNow.value = DateTime.now();
  }.asAction();

  static List<String> _loadTimezones() {
    return File(kTimezonesFile).readAsLinesSync();
  }
}
