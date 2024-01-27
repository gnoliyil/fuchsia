// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:collection';
import 'dart:convert';
import 'dart:typed_data';

import 'package:fidl/fidl.dart' as fidl;
import 'package:fidl_fuchsia_io/fidl_async.dart';
import 'package:quiver/collection.dart';
import 'package:zircon/zircon.dart';

import 'internal/_flags.dart';
import 'vnode.dart';

// ignore_for_file: import_of_legacy_library_into_null_safe
// ignore_for_file: public_member_api_docs
// ignore_for_file: unnecessary_null_comparison

/// A [PseudoDir] is a directory-like object whose entries are constructed
/// by a program at runtime.  The client can lookup, enumerate, and watch these
/// directory entries but it cannot create, remove, or rename them.
///
/// This class is designed to allow programs to publish a relatively small number
/// of entries (up to a few hundreds) such as services, file-system roots,
/// debugging [PseudoFile].
///
/// This version doesn't support watchers, should support watchers if needed.
class PseudoDir extends Vnode {
  static const _maxObjectNameLength = 256;

  final HashMap<String, _Entry> _entries = HashMap();
  final AvlTreeSet<_Entry> _treeEntries =
      AvlTreeSet(comparator: (v1, v2) => v1.nodeId.compareTo(v2.nodeId));
  int _nextId = 1;
  final List<_DirConnection> _connections = [];
  bool _isClosed = false;

  static bool _isLegalObjectName(String objectName) {
    const forwardSlashUtf16 = 47;
    return objectName.isNotEmpty &&
        objectName.length < _maxObjectNameLength &&
        objectName != '.' &&
        objectName != '..' &&
        objectName.codeUnits
            .every((unit) => (unit != 0 && unit != forwardSlashUtf16));
  }

  /// Adds a directory entry associating the given [name] with [node].
  /// It is ok to add the same Vnode multiple times with different names.
  ///
  /// Returns `ZX.OK` on success.
  /// Returns `ZX.ERR_INVALID_ARGS` if name is illegal object name.
  /// Returns `ZX.ERR_ALREADY_EXISTS` if there is already a node with the
  /// given name.
  int addNode(String name, Vnode node) {
    if (!_isLegalObjectName(name)) {
      return ZX.ERR_INVALID_ARGS;
    }
    if (_entries.containsKey(name)) {
      return ZX.ERR_ALREADY_EXISTS;
    }
    final id = _nextId++;
    final e = _Entry(node, name, id);
    _entries[name] = e;
    _treeEntries.add(e);
    return ZX.OK;
  }

  @override
  void close() {
    _isClosed = true;
    for (final entry in _entries.entries) {
      entry.value.node!.close();
    }
    removeAllNodes();
    // schedule a task because if user closes this as soon as
    // they open a connection, dart fidl binding throws exception due to
    // event(OnOpen) on this fidl.
    scheduleMicrotask(() {
      for (final c in _connections) {
        c.closeBinding();
      }
      _connections.clear();
    });
  }

  /// Connects to this instance of [PseudoDir] and serves
  /// [fidl_fuchsia_io.Directory] over fidl.
  @override
  int connect(OpenFlags flags, int mode, fidl.InterfaceRequest<Node> request,
      [OpenFlags? parentFlags]) {
    if (_isClosed) {
      sendErrorEvent(flags, ZX.ERR_NOT_SUPPORTED, request);
      return ZX.ERR_NOT_SUPPORTED;
    }
    // There should be no modeType* flags set, except for, possibly,
    // modeTypeDirectory when the target is a pseudo dir.
    if ((mode & ~modeProtectionMask) & ~modeTypeDirectory != 0) {
      sendErrorEvent(flags, ZX.ERR_INVALID_ARGS, request);
      return ZX.ERR_INVALID_ARGS;
    }

    var connectFlags = filterForNodeReference(flags);

    parentFlags ??= Flags.fsRightsDefault();
    // Grant POSIX clients additional rights.
    if (Flags.isPosixWritable(connectFlags)) {
      connectFlags |= parentFlags & OpenFlags.rightWritable;
    }
    if (Flags.isPosixExecutable(connectFlags)) {
      connectFlags |= parentFlags & OpenFlags.rightExecutable;
    }
    // Clear any remaining POSIX right expansion flags.
    connectFlags &= ~(OpenFlags.posixWritable | OpenFlags.posixExecutable);

    final status = _validateFlags(connectFlags);
    if (status != ZX.OK) {
      sendErrorEvent(connectFlags, status, request);
      return status;
    }
    final connection = _DirConnection(
        mode, connectFlags, this, fidl.InterfaceRequest(request.passChannel()));
    _connections.add(connection);
    return ZX.OK;
  }

  @override
  int inodeNumber() {
    return inoUnknown;
  }

  /// Checks if directory is empty.
  bool isEmpty() {
    return _entries.isEmpty;
  }

  /// Returns names of the the nodes present in this directory.
  List<String> listNodeNames() {
    return _treeEntries.map((f) => f.name).toList();
  }

  /// Looks up a node for given `name`.
  ///
  /// Returns `null` if no node if found.
  Vnode? lookup(String name) {
    final v = _entries[name];
    if (v != null) {
      return v.node;
    }
    return null;
  }

  @override
  void open(OpenFlags flags, int mode, String path,
      fidl.InterfaceRequest<Node> request,
      [OpenFlags? parentFlags]) {
    parentFlags ??= Flags.fsRightsDefault();
    if (path == '.') {
      connect(flags, mode, request, parentFlags);
      return;
    }
    if (path.startsWith('/')) {
      path = path.substring(1);
    }
    if (path == '') {
      sendErrorEvent(flags, ZX.ERR_BAD_PATH, request);
      return;
    }

    final index = path.indexOf('/');
    final key = index == -1 ? path : path.substring(0, index);
    if (!_isLegalObjectName(key)) {
      sendErrorEvent(flags, ZX.ERR_BAD_PATH, request);
      return;
    }
    final entry = _entries[key];
    if (entry == null) {
      sendErrorEvent(flags, ZX.ERR_NOT_FOUND, request);
      return;
    }
    // node is never null; it is optional only to support `nearest`.
    final node = entry.node!;
    // Final element, open it.
    if (index == -1) {
      node.connect(flags, mode, request, parentFlags);
      return;
    }
    if (index == path.length - 1) {
      // '/' is at end, should be a directory, add flag.
      node.connect(flags | OpenFlags.directory, mode, request, parentFlags);
      return;
    }
    // Forward request to child and let it handle rest of path.
    return node.open(
        flags, mode, path.substring(index + 1), request, parentFlags);
  }

  /// Removes all directory entries.
  void removeAllNodes() {
    _entries.clear();
    _treeEntries.clear();
  }

  /// Removes a directory entry with the given `name`.
  ///
  /// Returns `ZX.OK` on success.
  /// Returns `ZX.RR_NOT_FOUND` if there is no node with the given name.
  int removeNode(String name) {
    final e = _entries.remove(name);
    if (e == null) {
      return ZX.ERR_NOT_FOUND;
    }
    _treeEntries.remove(e);
    return ZX.OK;
  }

  /// Serves this [request] directory over request channel.
  /// Caller may specify the rights granted to the [request] connection.
  /// If `rights` is omitted, it defaults to readable and writable.
  int serve(fidl.InterfaceRequest<Node> request, {OpenFlags? rights}) {
    if (rights != null) {
      assert((rights & ~openRights) == OpenFlags.$none);
    }
    rights ??= OpenFlags.rightReadable | OpenFlags.rightWritable;
    return connect(OpenFlags.directory | rights, 0, request);
  }

  @override
  DirentType type() {
    return DirentType.directory;
  }

  void _onClose(_DirConnection obj) {
    final result = _connections.remove(obj);
    scheduleMicrotask(() {
      obj.closeBinding();
    });
    assert(result);
  }

  int _validateFlags(OpenFlags flags) {
    final allowedFlags = OpenFlags.rightReadable |
        OpenFlags.rightWritable |
        OpenFlags.directory |
        OpenFlags.nodeReference |
        OpenFlags.describe |
        OpenFlags.posixWritable |
        OpenFlags.posixExecutable |
        OpenFlags.cloneSameRights;
    final prohibitedFlags = OpenFlags.create |
        OpenFlags.createIfAbsent |
        OpenFlags.truncate |
        OpenFlags.append;

    // TODO(fxbug.dev/33058) : do not allow OpenFlags.rightWritable.

    if (flags & prohibitedFlags != OpenFlags.$none) {
      return ZX.ERR_INVALID_ARGS;
    }
    if (flags & ~allowedFlags != OpenFlags.$none) {
      return ZX.ERR_NOT_SUPPORTED;
    }
    return ZX.OK;
  }
}

/// Implementation of fuchsia.io.Directory for pseudo directory.
///
/// This class should not be used directly, but by [fuchsia_vfs.PseudoDir].
class _DirConnection extends Directory {
  final DirectoryBinding _binding = DirectoryBinding();
  bool isNodeRef = false;

  // reference to current Directory object;
  final PseudoDir _dir;
  final int _mode;
  final OpenFlags _flags;

  /// Position in directory where [#readDirents] should start searching. If less
  /// than 0, means first entry should be dot('.').
  ///
  /// All the entires in [PseudoDir] are greater then 0.
  /// We will get key after `_seek` and traverse in the TreeMap.
  int _seek = -1;

  bool _isClosed = false;

  /// Constructor
  _DirConnection(this._mode, this._flags, this._dir,
      fidl.InterfaceRequest<Directory> request)
      : assert(_dir != null),
        assert(request != null) {
    _binding.bind(this, request);
    _binding.whenClosed.then((_) {
      return close();
    });
    if (_flags & OpenFlags.nodeReference != OpenFlags.$none) {
      isNodeRef = true;
    }
  }

  @override
  Stream<Directory$OnOpen$Response> get onOpen {
    Directory$OnOpen$Response d;
    if ((_flags & OpenFlags.describe) == OpenFlags.$none) {
      d = Directory$OnOpen$Response(ZX.ERR_NOT_DIR, null);
    } else {
      d = Directory$OnOpen$Response(
          ZX.OK, NodeInfoDeprecated.withDirectory(DirectoryObject()));
    }
    return Stream.fromIterable([d]);
  }

  // TODO(https://fxbug.dev/77623): Switch from onOpen to onRepresentation when
  // clients are ready.
  @override
  Stream<Representation> get onRepresentation async* {}

  @override
  Future<void> advisoryLock(AdvisoryLockRequest request) async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<void> clone(
      OpenFlags flags, fidl.InterfaceRequest<Node> object) async {
    if (!Flags.inputPrecondition(flags)) {
      _dir.sendErrorEvent(flags, ZX.ERR_INVALID_ARGS, object);
      return;
    }
    if (Flags.shouldCloneWithSameRights(flags)) {
      if ((flags & openRights) != OpenFlags.$none) {
        _dir.sendErrorEvent(flags, ZX.ERR_INVALID_ARGS, object);
        return;
      }
    }

    // If SAME_RIGHTS is requested, cloned connection will inherit the same
    // rights as those from the originating connection.
    var newFlags = flags;
    if (Flags.shouldCloneWithSameRights(flags)) {
      newFlags &= (~openRights);
      newFlags |= (_flags & openRights);
      newFlags &= ~OpenFlags.cloneSameRights;
    }

    if (!Flags.stricterOrSameRights(newFlags, _flags)) {
      _dir.sendErrorEvent(flags, ZX.ERR_ACCESS_DENIED, object);
      return;
    }

    _dir.connect(newFlags, _mode, object);
  }

  @override
  Future<void> close() async {
    if (!_isClosed) {
      _isClosed = true;
      scheduleMicrotask(() {
        _dir._onClose(this);
      });
    }
  }

  @override
  Future<Uint8List> query() async {
    return Utf8Encoder().convert(directoryProtocolName);
  }

  void closeBinding() {
    _binding.close();
    _isClosed = true;
  }

  @override
  Future<ConnectionInfo> getConnectionInfo() async {
    return ConnectionInfo();
  }

  @override
  Future<Directory$GetAttr$Response> getAttr() async {
    final n = NodeAttributes(
      mode: modeTypeDirectory | modeProtectionMask,
      id: inoUnknown,
      contentSize: 0,
      storageSize: 0,
      linkCount: 1,
      creationTime: 0,
      modificationTime: 0,
    );
    return Directory$GetAttr$Response(ZX.OK, n);
  }

  @override
  Future<Directory$GetToken$Response> getToken() async {
    return Directory$GetToken$Response(ZX.ERR_NOT_SUPPORTED, null);
  }

  @override
  Future<int> link(String src, Handle dstParentToken, String dst) async {
    return ZX.ERR_NOT_SUPPORTED;
  }

  @override
  Future<void> addInotifyFilter(
      String path, InotifyWatchMask filters, int wd, Socket socket) async {
    return;
  }

  @override
  Future<void> open(OpenFlags flags, int mode, String path,
      fidl.InterfaceRequest<Node> object) async {
    if (!Flags.inputPrecondition(flags)) {
      _dir.sendErrorEvent(flags, ZX.ERR_INVALID_ARGS, object);
      return;
    }
    if (Flags.shouldCloneWithSameRights(flags)) {
      _dir.sendErrorEvent(flags, ZX.ERR_INVALID_ARGS, object);
      return;
    }
    if (Flags.isNodeReference(flags) &&
        ((flags & openRights) == OpenFlags.$none)) {
      _dir.sendErrorEvent(flags, ZX.ERR_INVALID_ARGS, object);
      return;
    }
    if (!Flags.stricterOrSameRights(flags, _flags)) {
      _dir.sendErrorEvent(flags, ZX.ERR_ACCESS_DENIED, object);
      return;
    }
    _dir.open(flags, mode, path, object, _flags);
  }

  @override
  Future<Directory$ReadDirents$Response> readDirents(int maxBytes) async {
    if (isNodeRef) {
      return Directory$ReadDirents$Response(ZX.ERR_BAD_HANDLE, Uint8List(0));
    }
    final buf = Uint8List(maxBytes);
    final bData = ByteData.view(buf.buffer);
    var firstOne = true;
    var index = 0;

    // add dot
    if (_seek < 0) {
      final bytes = _encodeDirent(
          bData, index, maxBytes, inoUnknown, DirentType.directory, '.');
      if (bytes == -1) {
        return Directory$ReadDirents$Response(
            ZX.ERR_BUFFER_TOO_SMALL, Uint8List(0));
      }
      firstOne = false;
      index += bytes;
      _seek = 0;
    }

    var status = ZX.OK;

    // add entries
    var entry = _dir._treeEntries.nearest(_Entry(null, '', _seek),
        nearestOption: TreeSearch.GREATER_THAN);

    if (entry != null) {
      final iterator = _dir._treeEntries.fromIterator(entry);
      while (iterator.moveNext()) {
        entry = iterator.current;
        // we should only send entries > _seek
        if (entry.nodeId <= _seek) {
          continue;
        }
        final bytes = _encodeDirent(bData, index, maxBytes,
            entry.node!.inodeNumber(), entry.node!.type(), entry.name);
        if (bytes == -1) {
          if (firstOne) {
            status = ZX.ERR_BUFFER_TOO_SMALL;
          }
          break;
        }
        firstOne = false;
        index += bytes;
        status = ZX.OK;
        _seek = entry.nodeId;
      }
    }
    return Directory$ReadDirents$Response(
        status, Uint8List.view(buf.buffer, 0, index));
  }

  @override
  Future<void> rename(String src, Handle dstParentToken, String dst) async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<int> rewind() async {
    _seek = -1;
    return ZX.OK;
  }

  @override
  Future<int> setAttr(
      NodeAttributeFlags flags, NodeAttributes attributes) async {
    return ZX.ERR_NOT_SUPPORTED;
  }

  @override
  Future<void> sync() async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<void> unlink(String name, UnlinkOptions options) async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<int> watch(WatchMask mask, int options,
      fidl.InterfaceRequest<DirectoryWatcher> watcher) async {
    return ZX.ERR_NOT_SUPPORTED;
  }

  /// returns number of bytes written
  int _encodeDirent(ByteData buf, int startIndex, int maxBytes, int inodeNumber,
      DirentType type, String name) {
    List<int> charBytes = utf8.encode(name);
    final len = 8 /*ino*/ + 1 /*size*/ + 1 /*type*/ + charBytes.length;
    // cannot fit in buffer
    if (maxBytes - startIndex < len) {
      return -1;
    }
    var index = startIndex;
    buf.setUint64(index, inodeNumber, Endian.little);
    index += 8;
    buf
      ..setUint8(index++, charBytes.length)
      ..setUint8(index++, type.$value);
    for (int i = 0; i < charBytes.length; i++) {
      buf.setUint8(index++, charBytes[i]);
    }
    return len;
  }

  @override
  Future<Directory$GetFlags$Response> getFlags() async {
    return Directory$GetFlags$Response(ZX.ERR_NOT_SUPPORTED, OpenFlags.$none);
  }

  @override
  Future<int> setFlags(OpenFlags flags) async {
    return ZX.ERR_NOT_SUPPORTED;
  }

  @override
  Future<Directory$QueryFilesystem$Response> queryFilesystem() async {
    return Directory$QueryFilesystem$Response(ZX.ERR_NOT_SUPPORTED, null);
  }

  @override
  Future<void> reopen(RightsRequest? rightsRequest,
      fidl.InterfaceRequest<Node> objectRequest) async {
    // TODO(https://fxbug.dev/77623): Close `objectRequest` with epitaph.
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<Directory$GetAttributes$Response> getAttributes(
      NodeAttributesQuery query) async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<void> updateAttributes(MutableNodeAttributes payload) async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<void> open2(
      String path, ConnectionProtocols protocols, Channel objectRequest) async {
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }

  @override
  Future<void> enumerate(DirectoryEnumerateOptions options,
      fidl.InterfaceRequest<DirectoryIterator> iterator) async {
    // TODO(https://fxbug.dev/77623): Close `iterator` with epitaph.
    throw fidl.MethodException(ZX.ERR_NOT_SUPPORTED);
  }
}

/// _Entry class to store in pseudo directory.
class _Entry {
  /// Vnode
  Vnode? node;

  /// node name
  String name;

  /// node id: defines insertion order
  int nodeId;

  /// Constructor
  _Entry(this.node, this.name, this.nodeId);
}
