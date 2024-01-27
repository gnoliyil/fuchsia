// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// @dart=2.12

import 'package:logging/logging.dart';

import 'sl4f_client.dart';

enum MicState { muted, available }

enum NetworkOption { wifi, ethernet, unknown }

final _micStateToJson = {
  MicState.muted: 'muted',
  MicState.available: 'available',
};

final _networkToJson = {
  NetworkOption.wifi: 'wifi',
  NetworkOption.ethernet: 'ethernet',
  NetworkOption.unknown: 'unknown',
};

final _log = Logger('setui_sl4f');

class SetUiSl4fException implements Exception {
  final String message;

  SetUiSl4fException(this.message);

  @override
  String toString() => 'SetUiSl4fException: $message';
}

/// Converts data from json to dart data type.
///
/// IntlInfo models the IntlInfo struct defined in
/// /src/testing/sl4f/src/setui/types.rs
class IntlInfo {
  final String temperatureUnit;
  final String timeZoneId;
  final String hourCycle;
  final List<String> locales;

  IntlInfo(
      {required this.temperatureUnit,
      required this.timeZoneId,
      required this.hourCycle,
      required this.locales});

  factory IntlInfo.fromJson(Map<String, dynamic>? json) {
    if (json == null ||
        json['locales'] == null ||
        json['temperature_unit'] == null ||
        json['time_zone_id'] == null ||
        json['hour_cycle'] == null) {
      throw SetUiSl4fException(
          'Invalid json while parsing IntlInfo, received $json');
    }

    List<String> locales = <String>[];
    for (final locale in json['locales']) {
      locales.add(locale['id']);
    }
    return IntlInfo(
        temperatureUnit: json['temperature_unit'],
        timeZoneId: json['time_zone_id'],
        hourCycle: json['hour_cycle'],
        locales: locales);
  }
}

/// Configures various aspects of the system.
class SetUi {
  final Sl4f _sl4f;

  SetUi(this._sl4f);

  Future<void> setDevNetworkOption(NetworkOption option) async {
    _log.info('Setting Dev network option to ${option.toString()}');
    if (option == NetworkOption.unknown) {
      throw SetUiSl4fException(
          'Invalid Dev Options input. Accepted are wifi and ethernet.');
    }
    await _sl4f.request('setui_facade.SetNetwork', _networkToJson[option]);
  }

  Future<NetworkOption> getDevNetworkOption() async {
    final result = await _sl4f.request('setui_facade.GetNetwork', null);
    switch (result) {
      case 'wifi':
        return NetworkOption.wifi;
      case 'ethernet':
        return NetworkOption.ethernet;
      default:
        return NetworkOption.unknown;
    }
  }

  /// [locale] accepts Unicode BCP-47 Locale Identifier.
  ///
  /// Reference LocaleId definition in
  /// https://fuchsia.googlesource.com/fuchsia/+/HEAD/sdk/fidl/fuchsia.intl/intl.fidl
  Future<void> setLocale(String locale) async {
    _log.info('Setting Locale to $locale}');
    await _sl4f.request('setui_facade.SetIntl', {
      'locales': [
        {'id': locale}
      ]
    });
  }

  Future<void> setTimeZone(String id) async {
    _log.info('Setting Time Zone to $id}');
    await _sl4f.request('setui_facade.SetIntl', {'time_zone_id': id});
  }

  Future<IntlInfo> getIntl() async {
    final result = await _sl4f.request('setui_facade.GetIntl');
    return IntlInfo.fromJson(result);
  }

  Future<bool> isMicMuted() async {
    final muted = await _sl4f.request('setui_facade.IsMicMuted');
    return muted;
  }

  Future<void> setMicMute(MicState state) async {
    _log.info('setMicMute - Setting mic state to ${state.toString()}');

    try {
      await _sl4f.request('setui_facade.SetMicMute', _micStateToJson[state]);
    } on Exception catch (e) {
      throw SetUiSl4fException(
          'setMicMute - Setting mic state to ${state.toString()} returned with error: $e');
    }
  }
}
