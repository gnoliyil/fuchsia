// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//ignore_for_file: type_annotate_public_apis

import 'package:fidl/fidl.dart';
import 'package:fidl_fuchsia_io/fidl_async.dart';
import 'package:zircon/zircon.dart';

import 'vnode.dart';

/// A [RemoteDir] is a directory-like object which holds a channel to a remotely
/// hosted directory to which requests are delegated when opened.
///
/// This class is designed to allow programs to publish remote filesystems
/// as directories without requiring a separate "mount" step.
class RemoteDir extends Vnode {
  final _proxy = DirectoryProxy();
  bool _isClosed = false;

  /// Constructs the [RemoteDir] with a given [channel] to the remote.
  RemoteDir(Channel channel) {
    _proxy.ctrl.bind(InterfaceHandle<Directory>(channel));
  }

  @override
  void close() {
    if (!_isClosed) {
      _proxy.ctrl.close();
      _isClosed = true;
    }
  }

  @override
  int connect(OpenFlags flags, ModeType mode, request,
      [OpenFlags? parentFlags]) {
    // Called when a PseudoDir needs to open this directory
    open(flags, mode, '.', request);
    return ZX.OK;
  }

  @override
  void open(OpenFlags flags, ModeType mode, String path,
      InterfaceRequest<Node> request,
      [OpenFlags? parentFlags]) {
    if (_isClosed) {
      sendErrorEvent(flags, ZX.ERR_NOT_SUPPORTED, request);
      return;
    }

    final status = _validateFlags(flags);
    if (status != ZX.OK) {
      sendErrorEvent(flags, status, request);
      return;
    }

    // Forward the reqeuest on to the remote directory
    _proxy.open(flags, mode, path, request);
  }

  int _validateFlags(OpenFlags flags) {
    return ZX.OK;
  }

  @override
  int inodeNumber() {
    return inoUnknown;
  }

  @override
  DirentType type() {
    return DirentType.directory;
  }
}
