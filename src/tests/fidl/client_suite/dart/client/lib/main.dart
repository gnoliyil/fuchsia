// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(https://fxbug.dev/84961): Fix null safety and remove this language version.
// @dart=2.9

import 'dart:async';
import 'dart:core';
import 'dart:typed_data';

import 'package:fidl/fidl.dart' hide UnknownEvent;
import 'package:fuchsia_logger/logger.dart';
import 'package:fidl_fidl_clientsuite/fidl_async.dart';
import 'package:fuchsia_services/services.dart';
import 'package:zircon/zircon.dart';
import 'package:fidl_zx/fidl_async.dart' show Rights;

FidlErrorKind classifyError(var error) {
  print('classifying error $error');
  if (error is FidlError) {
    print('FidlError.code: ${error.code}');
    switch (error.code) {
      case FidlErrorCode.fidlUnknownMethod:
        return FidlErrorKind.unknownMethod;
      case FidlErrorCode.fidlExceededMaxOutOfLineDepth:
      case FidlErrorCode.fidlInvalidBoolean:
      case FidlErrorCode.fidlInvalidPresenceIndicator:
      case FidlErrorCode.fidlInvalidNumBytesInEnvelope:
      case FidlErrorCode.fidlInvalidNumHandlesInEnvelope:
      case FidlErrorCode.fidlInvalidInlineMarkerInEnvelope:
      case FidlErrorCode.fidlTooFewBytes:
      case FidlErrorCode.fidlTooManyBytes:
      case FidlErrorCode.fidlTooFewHandles:
      case FidlErrorCode.fidlTooManyHandles:
      case FidlErrorCode.fidlStringTooLong:
      case FidlErrorCode.fidlNonNullableTypeWithNullValue:
      case FidlErrorCode.fidlStrictUnionUnknownField:
      case FidlErrorCode.fidlUnknownMagic:
      case FidlErrorCode.fidlInvalidBit:
      case FidlErrorCode.fidlInvalidEnumValue:
      case FidlErrorCode.fidlIntOutOfRange:
      case FidlErrorCode.fidlNonEmptyStringWithNullBody:
      case FidlErrorCode.fidlNonEmptyVectorWithNullBody:
      case FidlErrorCode.fidlNonResourceHandle:
      case FidlErrorCode.fidlMissingRequiredHandleRights:
      case FidlErrorCode.fidlIncorrectHandleType:
      case FidlErrorCode.fidlInvalidInlineBitInEnvelope:
      case FidlErrorCode.fidlCountExceedsLimit:
      case FidlErrorCode.fidlInvalidPaddingByte:
      case FidlErrorCode.fidlUnsupportedWireFormat:
        return FidlErrorKind.decodingError;
    }
  }
  return FidlErrorKind.otherError;
}

class RunnerImpl extends Runner {
  Future<bool> isTestEnabled(Test test) async {
    switch (test) {
      // fxbug.dev/111496: Dart doesn't allow us to differentiate peer closed in
      // FidlErrorCode, so we can't correctly tell the cause of the error in this
      // case.
      case Test.gracefulFailureDuringCallAfterPeerClose:
      // Dart bindings do close the channel in these cases, however they don't
      // provide a way to observe this as an error separately from just the
      // channel being closed, so there's no way to differentiate between peer
      // disconnected an peer sent a bad message when reporting the channel being
      // closed to the reporter.
      case Test.unknownStrictEventClosedProtocol:
      case Test.unknownFlexibleEventClosedProtocol:
      case Test.unknownStrictEventAjarProtocol:
      case Test.unknownStrictEventOpenProtocol:
      case Test.unknownStrictServerInitiatedTwoWay:
      case Test.unknownFlexibleServerInitiatedTwoWay:
        return false;
      default:
        return true;
    }
  }

  Future<void> checkAlive() async {}

  Future<EmptyResultClassification> callTwoWayNoPayload(
      InterfaceHandle<ClosedTarget> targetHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.twoWayNoPayload();
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<NonEmptyResultClassification> callTwoWayStructPayload(
      InterfaceHandle<ClosedTarget> targetHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      int result = await target.twoWayStructPayload();
      return NonEmptyResultClassification.withSuccess(
          NonEmptyPayload(someField: result));
    } catch (e) {
      return NonEmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<TableResultClassification> callTwoWayTablePayload(
      InterfaceHandle<ClosedTarget> targetHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      TablePayload result = await target.twoWayTablePayload();
      return TableResultClassification.withSuccess(result);
    } catch (e) {
      return TableResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<UnionResultClassification> callTwoWayUnionPayload(
      InterfaceHandle<ClosedTarget> targetHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      UnionPayload result = await target.twoWayUnionPayload();
      return UnionResultClassification.withSuccess(result);
    } catch (e) {
      return UnionResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<NonEmptyResultWithErrorClassification> callTwoWayStructPayloadErr(
      InterfaceHandle<ClosedTarget> targetHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      int result = await target.twoWayStructPayloadErr();
      return NonEmptyResultWithErrorClassification.withSuccess(
          NonEmptyPayload(someField: result));
    } on MethodException<int> catch (e) {
      return NonEmptyResultWithErrorClassification.withApplicationError(
          e.value);
    } catch (e) {
      return NonEmptyResultWithErrorClassification.withFidlError(
          classifyError(e));
    }
  }

  Future<EmptyResultClassification> callTwoWayStructRequest(
      InterfaceHandle<ClosedTarget> targetHandle,
      NonEmptyPayload request) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.twoWayStructRequest(request.someField);
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callTwoWayTableRequest(
      InterfaceHandle<ClosedTarget> targetHandle, TablePayload request) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.twoWayTableRequest(request);
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callTwoWayUnionRequest(
      InterfaceHandle<ClosedTarget> targetHandle, UnionPayload request) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.twoWayUnionRequest(request);
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callOneWayNoRequest(
      InterfaceHandle<ClosedTarget> targetHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.oneWayNoRequest();
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callOneWayStructRequest(
      InterfaceHandle<ClosedTarget> targetHandle,
      NonEmptyPayload request) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.oneWayStructRequest(request.someField);
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callOneWayTableRequest(
      InterfaceHandle<ClosedTarget> targetHandle, TablePayload request) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.oneWayTableRequest(request);
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callOneWayUnionRequest(
      InterfaceHandle<ClosedTarget> targetHandle, UnionPayload request) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.oneWayUnionRequest(request);
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callStrictOneWay(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.strictOneWay();
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callFlexibleOneWay(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.flexibleOneWay();
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultClassification> callStrictTwoWay(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.strictTwoWay();
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<NonEmptyResultClassification> callStrictTwoWayFields(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      int result = await target.strictTwoWayFields();
      return NonEmptyResultClassification.withSuccess(
          NonEmptyPayload(someField: result));
    } catch (e) {
      return NonEmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultWithErrorClassification> callStrictTwoWayErr(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.strictTwoWayErr();
      return const EmptyResultWithErrorClassification.withSuccess(Empty());
    } on MethodException<int> catch (e) {
      return EmptyResultWithErrorClassification.withApplicationError(e.value);
    } catch (e) {
      return EmptyResultWithErrorClassification.withFidlError(classifyError(e));
    }
  }

  Future<NonEmptyResultWithErrorClassification> callStrictTwoWayFieldsErr(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      int result = await target.strictTwoWayFieldsErr();
      return NonEmptyResultWithErrorClassification.withSuccess(
          NonEmptyPayload(someField: result));
    } on MethodException<int> catch (e) {
      return NonEmptyResultWithErrorClassification.withApplicationError(
          e.value);
    } catch (e) {
      return NonEmptyResultWithErrorClassification.withFidlError(
          classifyError(e));
    }
  }

  Future<EmptyResultClassification> callFlexibleTwoWay(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.flexibleTwoWay();
      return const EmptyResultClassification.withSuccess(Empty());
    } catch (e) {
      return EmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<NonEmptyResultClassification> callFlexibleTwoWayFields(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      int result = await target.flexibleTwoWayFields();
      return NonEmptyResultClassification.withSuccess(
          NonEmptyPayload(someField: result));
    } catch (e) {
      return NonEmptyResultClassification.withFidlError(classifyError(e));
    }
  }

  Future<EmptyResultWithErrorClassification> callFlexibleTwoWayErr(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      await target.flexibleTwoWayErr();
      return const EmptyResultWithErrorClassification.withSuccess(Empty());
    } on MethodException<int> catch (e) {
      return EmptyResultWithErrorClassification.withApplicationError(e.value);
    } catch (e) {
      return EmptyResultWithErrorClassification.withFidlError(classifyError(e));
    }
  }

  Future<NonEmptyResultWithErrorClassification> callFlexibleTwoWayFieldsErr(
      InterfaceHandle<OpenTarget> targetHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    try {
      int result = await target.flexibleTwoWayFieldsErr();
      return NonEmptyResultWithErrorClassification.withSuccess(
          NonEmptyPayload(someField: result));
    } on MethodException<int> catch (e) {
      return NonEmptyResultWithErrorClassification.withApplicationError(
          e.value);
    } catch (e) {
      return NonEmptyResultWithErrorClassification.withFidlError(
          classifyError(e));
    }
  }

  Future<void> receiveClosedEvents(InterfaceHandle<ClosedTarget> targetHandle,
      InterfaceHandle<ClosedTargetEventReporter> reporterHandle) async {
    var target = ClosedTargetProxy();
    target.ctrl.bind(targetHandle);
    var reporter = ClosedTargetEventReporterProxy();
    reporter.ctrl.bind(reporterHandle);

    target.onEventNoPayload.forEach((_) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(
            const ClosedTargetEventReport.withOnEventNoPayload(Empty()));
      }
    });

    target.onEventStructPayload.forEach((someField) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(ClosedTargetEventReport.withOnEventStructPayload(
            NonEmptyPayload(someField: someField)));
      }
    });

    target.onEventTablePayload.forEach((payload) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(
            ClosedTargetEventReport.withOnEventTablePayload(payload));
      }
    });

    target.onEventUnionPayload.forEach((payload) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(
            ClosedTargetEventReport.withOnEventUnionPayload(payload));
      }
    });

    target.ctrl.whenClosed.then((_) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(const ClosedTargetEventReport.withFidlError(
            FidlErrorKind.channelPeerClosed));
      }
    });

    reporter.ctrl.whenClosed.then((_) {
      target.ctrl.close();
    });
  }

  Future<void> receiveAjarEvents(InterfaceHandle<AjarTarget> targetHandle,
      InterfaceHandle<AjarTargetEventReporter> reporterHandle) async {
    var target = AjarTargetProxy();
    target.ctrl.bind(targetHandle);
    var reporter = AjarTargetEventReporterProxy();
    reporter.ctrl.bind(reporterHandle);

    target.$unknownEvents.forEach((unknownEvent) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(AjarTargetEventReport.withUnknownEvent(
            UnknownEvent(ordinal: unknownEvent.ordinal)));
      }
    });

    target.ctrl.whenClosed.then((_) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(const AjarTargetEventReport.withFidlError(
            FidlErrorKind.channelPeerClosed));
      }
    });

    reporter.ctrl.whenClosed.then((_) {
      target.ctrl.close();
    });
  }

  Future<void> receiveOpenEvents(InterfaceHandle<OpenTarget> targetHandle,
      InterfaceHandle<OpenTargetEventReporter> reporterHandle) async {
    var target = OpenTargetProxy();
    target.ctrl.bind(targetHandle);
    var reporter = OpenTargetEventReporterProxy();
    reporter.ctrl.bind(reporterHandle);

    target.strictEvent.forEach((_) {
      if (reporter.ctrl.isBound) {
        reporter
            .reportEvent(const OpenTargetEventReport.withStrictEvent(Empty()));
      }
    });

    target.flexibleEvent.forEach((_) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(
            const OpenTargetEventReport.withFlexibleEvent(Empty()));
      }
    });

    target.$unknownEvents.forEach((unknownEvent) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(OpenTargetEventReport.withUnknownEvent(
            UnknownEvent(ordinal: unknownEvent.ordinal)));
      }
    });

    target.ctrl.whenClosed.then((_) {
      if (reporter.ctrl.isBound) {
        reporter.reportEvent(const OpenTargetEventReport.withFidlError(
            FidlErrorKind.channelPeerClosed));
      }
    });

    reporter.ctrl.whenClosed.then((_) {
      target.ctrl.close();
    });
  }
}

ComponentContext _context;

void main(List<String> args) {
  setupLogger(name: 'fidl-dynsuite-dart-client');
  print('Dart client: main');
  _context = ComponentContext.create();

  _context.outgoing
    ..addPublicService<Runner>((request) {
      RunnerBinding().bind(RunnerImpl(), request);
    }, Runner.$serviceName)
    ..serveFromStartupInfo();
}
