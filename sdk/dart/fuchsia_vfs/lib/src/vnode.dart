// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:fidl/fidl.dart';
import 'package:fidl_fuchsia_io/fidl_async.dart';
import 'package:zircon/zircon.dart';

import 'internal/_error_node.dart';
import 'internal/_flags.dart';

/// This interface declares a abstract Vnode class with
/// common operations that may be overwritten.
///
/// These nodes can be added to [PseudoDir] and can be accessed from filesystem.
abstract class Vnode {
  final List<ErrorNodeForSendingEvent> _errorNodes = [];

  /// Close this node and all of its bindings and children.
  void close();

  /// Connect to this vnode.
  /// All flags and modes are defined in
  /// https://fuchsia.googlesource.com/fuchsia/+/HEAD/sdk/fidl/fuchsia.io/
  ///
  /// By default param [#parentFlags] is all rights, so that open will allow
  /// all rights requested on the incoming [request].
  /// This param is used by clone to restrict cloning.
  int connect(OpenFlags flags, ModeType mode, InterfaceRequest<Node> request,
      [OpenFlags? parentFlags]);

  /// Filter flags when [OpenFlags.nodeReference] is passed.
  /// This will maintain compatibility with c++ layer.
  OpenFlags filterForNodeReference(OpenFlags flags) {
    if (Flags.isNodeReference(flags)) {
      return flags &
          (OpenFlags.nodeReference | OpenFlags.directory | OpenFlags.describe);
    }
    return flags;
  }

  /// Inode number as defined in fuchsia.io.
  int inodeNumber();

  /// This function is called from [fidl_fuchsia_io.Directory#open].
  /// This function parses path and opens correct node.
  ///
  /// Vnode provides a simplified implementation for non-directory types.
  /// Behavior:
  /// For directory types, it will throw UnimplementedError error.
  /// For non empty path it will fail with [ERR_NOT_DIR].
  void open(OpenFlags flags, ModeType mode, String path,
      InterfaceRequest<Node> request,
      [OpenFlags? parentFlags]) {
    if (type() == DirentType.directory) {
      // dir types should implement this function
      throw UnimplementedError();
    }
    sendErrorEvent(flags, ZX.ERR_NOT_DIR, request);
  }

  /// Create a error node to send onOpen event with failure status.
  void sendErrorEvent(
      OpenFlags flags, int status, InterfaceRequest<Node> request) {
    if ((flags & OpenFlags.describe) != OpenFlags.$none) {
      final e = ErrorNodeForSendingEvent(status, _removeErrorNode, request);
      _errorNodes.add(e);
    } else {
      request.close();
    }
  }

  DirentType type();

  void _removeErrorNode(ErrorNodeForSendingEvent e) {
    _errorNodes.remove(e);
  }
}
