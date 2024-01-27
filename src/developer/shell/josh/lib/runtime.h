// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_SHELL_JOSH_LIB_RUNTIME_H_
#define SRC_DEVELOPER_SHELL_JOSH_LIB_RUNTIME_H_

#include <string>

#include "third_party/quickjs/quickjs-libc.h"
#include "third_party/quickjs/quickjs.h"

#define DEFAULT_SEQUENCE_JSON_FILENAME "startup.json"

namespace shell {

// A C++ wrapper for the JSRuntime type.  Creates a JSRuntime and manages its lifetime.
class Runtime {
 public:
  Runtime();

  JSRuntime* Get() const { return rt_; }

  void HandlePromiseRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason);

  // Check if there is any unhandled error in the current JSRuntime
  bool HasUnhandledError() const { return has_error_; }

  ~Runtime();

 private:
  JSRuntime* rt_;
  bool is_valid_;
  bool has_error_;
};

// A C++ wrapper for the JSContext type.  You can have multiple JSContexts for a given JSRuntime.
// Creates a JSContext and manages its lifetime.
class Context {
 public:
  Context(const Runtime* rt);
  ~Context();

  JSContext* Get() const { return ctx_; }
  void DumpError() { js_std_dump_error(ctx_); }

  // Initializes standard libc functions and makes them available via globalThis.
  bool InitStd();

  // Initialize Fuchsia-isms: zx_internal, fdio, etc.
  // fidl_path is the directory to look for the FIDL IR.
  // boot_js_path is the directory to look for builtin JS (like ls and friends).
  bool InitBuiltins(const std::string& fidl_path, const std::string& boot_js_path);

  // Initialize startup modules.
  // startup_js_path is the directory to look for startup JS and the sequence JSON.
  bool InitStartups(const std::string& startup_js_path);

  // Loads JS from the given lib.
  // If js_path is empty, it will load it from a predefined module  of that name.
  // If js_path is non-empty, it will load it from a similarly named JS file relative to js_path.
  // For example, if you pass "ns", it will load "$path/ns.js". This can obviously be made better
  // (e.g., support subdirectories, handle missing files), but we don't need that yet.
  bool Export(const std::string&, const std::string& js_path = "");

  // Loads JS from the given script.
  // If js_file_path is empty, it will load it from a predefined module of that name.
  // If js_file_path is non-empty, it will load it from the js script file.
  bool ExportScript(const std::string&, const std::string& js_file_path = "");

 private:
  JSContext* ctx_;
  bool is_valid_;
};

// Wrapper class for JSValue types that provides some convenience features.
class Value {
 public:
  Value(Context* ctx, JSValue value) : ctx_(ctx->Get()), val_(value) {}
  Value(JSContext* ctx, JSValue value) : ctx_(ctx), val_(value) {}

  // Returns a std::string with the value of the JSValue and any additional
  // exception/error information..
  std::string ToString() const;

 private:
  std::string RawValueToString() const;

  JSContext* ctx_;
  JSValue val_;
};

std::ostream& operator<<(std::ostream& os, const Value& value);

}  // namespace shell

#endif  // SRC_DEVELOPER_SHELL_JOSH_LIB_RUNTIME_H_
