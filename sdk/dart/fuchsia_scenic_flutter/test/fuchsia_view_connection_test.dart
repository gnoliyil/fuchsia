// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: avoid_as, dead_code, null_check_always_fails

import 'dart:async';
import 'dart:ui' as ui;

import 'package:fidl_fuchsia_ui_views/fidl_async.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:fuchsia_scenic_flutter/fuchsia_view.dart';
// ignore: implementation_imports
import 'package:fuchsia_scenic_flutter/src/pointer_injector.dart';
import 'package:mockito/mockito.dart';
import 'package:pedantic/pedantic.dart';
import 'package:zircon/zircon.dart';

void main() {
  const testViewId = 42;
  const testHandle = 10;

  Future<void> sendViewConnectedEvent(int viewId) {
    return FuchsiaViewsService.instance.platformViewChannel.binaryMessenger
        .handlePlatformMessage(
            FuchsiaViewsService.instance.platformViewChannel.name,
            FuchsiaViewsService.instance.platformViewChannel.codec
                .encodeMethodCall(MethodCall(
                    'View.viewConnected', <String, dynamic>{'viewId': viewId})),
            null);
  }

  Future<void> sendViewDisconnectedEvent(int viewId) {
    return FuchsiaViewsService.instance.platformViewChannel.binaryMessenger
        .handlePlatformMessage(
            FuchsiaViewsService.instance.platformViewChannel.name,
            FuchsiaViewsService.instance.platformViewChannel.codec
                .encodeMethodCall(MethodCall('View.viewDisconnected',
                    <String, dynamic>{'viewId': viewId})),
            null);
  }

  void expectPlatformViewChannelCall(
      WidgetTester tester, String methodName, Map<String, dynamic> methodArgs,
      {dynamic Function()? handler}) {
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        FuchsiaViewsService.instance.platformViewChannel, (call) async {
      expect(call.method, methodName);
      expect(call.arguments, methodArgs);
      if (handler != null) {
        return handler.call();
      } else {
        return null;
      }
    });
  }

  testWidgets('FuchsiaViewConnection', (WidgetTester tester) async {
    bool connectedCalled = false;
    bool disconnectedCalled = false;
    final connection = TestFuchsiaViewConnection(
      _mockViewHolderToken(testViewId),
      onViewConnected: (_) => connectedCalled = true,
      onViewDisconnected: (_) => disconnectedCalled = true,
    );

    expectPlatformViewChannelCall(tester, 'View.create', <String, dynamic>{
      'viewId': testViewId,
      'hitTestable': true,
      'focusable': true,
      'viewOcclusionHintLTRB': <double>[0, 0, 0, 0],
    });
    await connection.connect();
    expect(connection.connected, false);
    expect(connectedCalled, false);
    expect(disconnectedCalled, false);

    // signal that the platform view has connected successfully.
    await sendViewConnectedEvent(testViewId);
    expect(connection.connected, true);
    expect(connectedCalled, true);

    // signal that the platform view has disconnected.
    await sendViewDisconnectedEvent(testViewId);
    expect(connection.connected, false);
    expect(disconnectedCalled, true);
  });

  testWidgets('FuchsiaViewConnection.usePointerInjection',
      (WidgetTester tester) async {
    bool connectedCalled = false;
    bool disconnectedCalled = false;
    final connection = TestFuchsiaViewConnection(
      _mockViewHolderToken(testViewId),
      viewRef: _mockViewRef(),
      onViewConnected: (_) => connectedCalled = true,
      onViewDisconnected: (_) => disconnectedCalled = true,
      usePointerInjection: true,
    );

    expectPlatformViewChannelCall(tester, 'View.create', <String, dynamic>{
      'viewId': testViewId,
      'hitTestable': true,
      'focusable': true,
      'viewOcclusionHintLTRB': <double>[0, 0, 0, 0],
    });
    await connection.connect();

    // Invoke all View.view* callbacks; disconnect() should dispose the pointer injector.
    await sendViewConnectedEvent(testViewId);
    await sendViewDisconnectedEvent(testViewId);
    expect(connectedCalled, true);
    expect(disconnectedCalled, true);
    verify(connection.pointerInjector.dispose());

    // Test pointer dispatch works.
    when(connection.pointerInjector.registered).thenReturn(false);
    await connection.dispatchPointerEvent(PointerDownEvent());
    verify((connection.pointerInjector as MockPointerInjector).register(
      hostViewRef: anyNamed('hostViewRef'),
      viewRef: anyNamed('viewRef'),
      viewport: anyNamed('viewport'),
    ));

    when(connection.pointerInjector.registered).thenReturn(true);
    await connection.dispatchPointerEvent(PointerDownEvent());
    verify((connection.pointerInjector as MockPointerInjector).dispatchEvent(
        pointer: anyNamed('pointer'), viewport: anyNamed('viewport')));
  });

  testWidgets('Recreate pointer injection on error',
      (WidgetTester tester) async {
    final connection = TestFuchsiaViewConnection(
      _mockViewHolderToken(testViewId),
      viewRef: _mockViewRef(),
      usePointerInjection: true,
    );

    // Error handler for pointer injection should dispose current instance of
    // PointerInjector.
    final injector = connection.pointerInjector;
    connection.onPointerInjectionError();
    verify(injector.dispose());

    // A new instance of PointerInjector should be created as part of register()
    // during dispatchPointerEvent call.
    connection._pointerInjector = MockPointerInjector();

    final downEvent = PointerDownEvent();
    when(connection.pointerInjector.registered).thenReturn(false);
    await connection.dispatchPointerEvent(downEvent);

    verify((connection.pointerInjector as MockPointerInjector).register(
      hostViewRef: anyNamed('hostViewRef'),
      viewRef: anyNamed('viewRef'),
      viewport: anyNamed('viewport'),
    ));

    // A new instance of PointerInjector should be used during register().
    final newInjector = connection.pointerInjector;
    expect(newInjector != injector, isTrue);
  });

  test('Multiple pointer events only register injector once', () async {
    final FuchsiaViewConnection connection = TestFuchsiaViewConnection(
      _mockViewHolderToken(testViewId),
      viewRef: _mockViewRef(),
      usePointerInjection: true,
    );

    // Inject multiple events which should trigger registration.
    // Don't `await` the resulting `Future`s, since the real caller
    // (_PlatformViewGestureRecognizer) doesn't do so.
    //
    // Switch the `registered` getter to return `true` after the
    // first call to `dispatchPointerEvent()`, to emulate the
    // case where the PointerInjector immediately updates
    // the registration state.
    final downEvent = PointerDownEvent();
    when(connection.pointerInjector.registered).thenReturn(false);
    unawaited(connection.dispatchPointerEvent(downEvent));
    when(connection.pointerInjector.registered).thenReturn(true);
    unawaited(connection.dispatchPointerEvent(downEvent));

    // Because `registered` was true at the time of the second pointer event,
    // FuchsiaViewConnection should not have called `register()` for the
    // second pointer event.
    verify((connection.pointerInjector as MockPointerInjector).register(
      hostViewRef: anyNamed('hostViewRef'),
      viewRef: anyNamed('viewRef'),
      viewport: anyNamed('viewport'),
    )).called(1);
  });

  test('Multiple pointer events preserve event order', () async {
    // Create a FuchsiaViewConnection with a PointerInjector whose
    // register() method never completes. This emulates the case
    // where the FIDL call to the PointerInjector Registry doesn't
    // complete immediately.
    final FuchsiaViewConnection connection = TestFuchsiaViewConnection(
      _mockViewHolderToken(testViewId),
      viewRef: _mockViewRef(),
      usePointerInjection: true,
    ).._pointerInjector = _hangingPointerInjector();

    // Inject a pointer event in the unregistered state.
    // Don't `await` the resulting `Future`s, since the real caller
    // (_PlatformViewGestureRecognizer) doesn't do so.
    final firstDownEvent = PointerDownEvent(position: Offset(1.0, 1.0));
    when(connection.pointerInjector.registered).thenReturn(false);
    unawaited(connection.dispatchPointerEvent(firstDownEvent));

    // Update PointerInjector to indicate that the injector is
    // registered. This emulates the case where PointerInjector
    // optimistically updates registration state before the FIDL
    // call completes.
    when(connection.pointerInjector.registered).thenReturn(true);

    // Inject a pointer event in the registered state.
    // Don't `await` the resulting `Future`s, since the real caller
    // (_PlatformViewGestureRecognizer) doesn't do so.
    final secondDownEvent = PointerDownEvent(position: Offset(2.0, 2.0));
    unawaited(connection.dispatchPointerEvent(secondDownEvent));

    // Verify that the events were not reordered.
    expect(
        verify((connection.pointerInjector as MockPointerInjector)
                .dispatchEvent(
                    pointer: captureAnyNamed('pointer'),
                    viewport: anyNamed('viewport')))
            .captured,
        anyOf(equals(null), equals(firstDownEvent),
            equals([firstDownEvent, secondDownEvent])));
  });

  testWidgets('usePointerInjection2 generates correct platform message',
      (WidgetTester tester) async {
    final pointerX = 10.0,
        pointerY = 10.0,
        device = 1,
        traceFlowId = 1,
        timestamp = Duration(microseconds: 1);
    final pointerEvent = PointerDownEvent(
        position: Offset(pointerX, pointerY),
        device: device,
        pointer: traceFlowId,
        timeStamp: timestamp);

    ui.Size window = ui.window.physicalSize / ui.window.devicePixelRatio;

    var args = <String, dynamic>{
      'viewId': testViewId,
      'x': pointerX,
      'y': pointerY,
      'phase': 1, // EventPhase.add
      'pointerId': device,
      'traceFlowId': traceFlowId,
      'logicalWidth': window.width,
      'logicalHeight': window.height,
      'timestamp': timestamp.inMicroseconds * 1000, // nanosecs
      'viewRef': testHandle
    };

    // Correct pointer injector platform message is generated for GFX views.
    {
      final connection = TestFuchsiaViewConnection(
        _mockViewHolderToken(testViewId),
        viewRef: _mockViewRef(handleValue: testHandle),
        usePointerInjection2: true,
      );

      expectPlatformViewChannelCall(tester, 'View.create', <String, dynamic>{
        'viewId': testViewId,
        'hitTestable': true,
        'focusable': true,
        'viewOcclusionHintLTRB': <double>[0, 0, 0, 0],
      });

      await connection.connect();

      expectPlatformViewChannelCall(
          tester, 'View.pointerinjector.inject', args);

      await connection.dispatchPointerEvent(pointerEvent);
    }

    // Correct pointer injector platform message is generated for Flatland views.
    {
      final connection = TestFuchsiaViewConnection.flatland(
        _mockViewportCreationToken(testViewId),
        usePointerInjection2: true,
      );

      expectPlatformViewChannelCall(tester, 'View.create', <String, dynamic>{
        'viewId': testViewId,
        'hitTestable': true,
        'focusable': true,
        'viewOcclusionHintLTRB': <double>[0, 0, 0, 0],
      });

      await connection.connect();

      // viewRef is not present in the platform message for flatland views as
      // the flutter engine holds the viewRef and does not provide it to the
      // dart code.
      args.remove('viewRef');

      expectPlatformViewChannelCall(
          tester, 'View.pointerinjector.inject', args);

      await connection.dispatchPointerEvent(pointerEvent);
    }
  });
}

ViewRef _mockViewRef({int handleValue = 0}) {
  final handle = MockHandle();
  when(handle.isValid).thenReturn(true);
  when(handle.handle).thenReturn(handleValue);
  when(handle.duplicate(any)).thenReturn(handle);
  final eventPair = MockEventPair();
  when(eventPair.handle).thenReturn(handle);
  when(eventPair.isValid).thenReturn(true);
  final viewRef = MockViewRef();
  when(viewRef.reference).thenReturn(eventPair);

  return viewRef;
}

ViewHolderToken _mockViewHolderToken(int handleValue) {
  final handle = MockHandle();
  when(handle.handle).thenReturn(handleValue);
  when(handle.isValid).thenReturn(true);
  final eventPair = MockEventPair();
  when(eventPair.handle).thenReturn(handle);
  when(eventPair.isValid).thenReturn(true);
  final viewHolderToken = MockViewHolderToken();
  when(viewHolderToken.value).thenReturn(eventPair);

  return viewHolderToken;
}

ViewportCreationToken _mockViewportCreationToken(int handleValue) {
  final handle = MockHandle();
  when(handle.handle).thenReturn(handleValue);
  when(handle.isValid).thenReturn(true);
  final channel = MockChannel();
  when(channel.handle).thenReturn(handle);
  when(channel.isValid).thenReturn(true);
  final viewportCreationToken = MockViewportCreationToken();
  when(viewportCreationToken.value).thenReturn(channel);

  return viewportCreationToken;
}

class TestFuchsiaViewConnection extends FuchsiaViewConnection {
  // ignore: prefer_final_fields
  var _pointerInjector = MockPointerInjector();

  TestFuchsiaViewConnection(
    ViewHolderToken viewHolderToken, {
    ViewRef? viewRef,
    FuchsiaViewConnectionCallback? onViewConnected,
    FuchsiaViewConnectionCallback? onViewDisconnected,
    FuchsiaViewConnectionStateCallback? onViewStateChanged,
    bool usePointerInjection = false,
    bool usePointerInjection2 = false,
  }) : super(
          viewHolderToken,
          viewRef: viewRef,
          onViewConnected: onViewConnected,
          onViewDisconnected: onViewDisconnected,
          onViewStateChanged: onViewStateChanged,
          usePointerInjection: usePointerInjection,
          usePointerInjection2: usePointerInjection2,
        );

  TestFuchsiaViewConnection.flatland(
    ViewportCreationToken viewportCreationToken, {
    FuchsiaViewConnectionCallback? onViewConnected,
    FuchsiaViewConnectionCallback? onViewDisconnected,
    FuchsiaViewConnectionStateCallback? onViewStateChanged,
    bool usePointerInjection = false,
    bool usePointerInjection2 = false,
  }) : super.flatland(
          viewportCreationToken,
          onViewConnected: onViewConnected,
          onViewDisconnected: onViewDisconnected,
          onViewStateChanged: onViewStateChanged,
          usePointerInjection2: usePointerInjection2,
        );

  @override
  PointerInjector get pointerInjector => _pointerInjector;

  @override
  ViewRef get hostViewRef => _mockViewRef();
}

class MockViewHolderToken extends Mock implements ViewHolderToken {
  @override
  int get hashCode => super.noSuchMethod(Invocation.method(#hashCode, []));

  @override
  bool operator ==(Object other) =>
      super.noSuchMethod(Invocation.method(#==, [other]));
}

class MockViewportCreationToken extends Mock implements ViewportCreationToken {
  @override
  int get hashCode => super.noSuchMethod(Invocation.method(#hashCode, []));

  @override
  bool operator ==(Object other) =>
      super.noSuchMethod(Invocation.method(#==, [other]));
}

class MockViewRef extends Mock implements ViewRef {
  @override
  int get hashCode => super.noSuchMethod(Invocation.method(#hashCode, []));

  @override
  bool operator ==(Object other) =>
      super.noSuchMethod(Invocation.method(#==, [other]));
}

class MockEventPair extends Mock implements EventPair {}

class MockChannel extends Mock implements Channel {}

class MockHandle extends Mock implements Handle {
  @override
  int get hashCode => super.noSuchMethod(Invocation.method(#hashCode, []));

  @override
  bool operator ==(Object other) =>
      super.noSuchMethod(Invocation.method(#==, [other]));

  @override
  Handle duplicate(int? rights) =>
      super.noSuchMethod(Invocation.method(#==, [rights]));
}

class MockPointerInjector extends Mock implements PointerInjector {
  @override
  Future<void> register({
    ViewRef? hostViewRef,
    ViewRef? viewRef,
    Rect? viewport,
  }) async =>
      super.noSuchMethod(Invocation.method(#register, [], {
        #hostViewRef: hostViewRef,
        #viewRef: viewRef,
        #viewport: viewport,
      }));

  @override
  Future<void> dispatchEvent({
    PointerEvent? pointer,
    Rect? viewport,
  }) async =>
      super.noSuchMethod(Invocation.method(#dispatchEvent, [], {
        #pointer: pointer,
        #viewport: viewport,
      }));
}

// Returns a MockPointerInjector whose `register()` method returns a `Future`
// which never completes.
MockPointerInjector _hangingPointerInjector() {
  final injector = MockPointerInjector();
  when(injector.register(
    hostViewRef: anyNamed('hostViewRef'),
    viewRef: anyNamed('viewRef'),
    viewport: anyNamed('viewport'),
  )).thenAnswer((_) => Completer().future);
  return injector;
}
