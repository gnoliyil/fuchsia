// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SYSTEM_SYMBOLS_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SYSTEM_SYMBOLS_H_

#include <map>
#include <memory>

#include "lib/fit/function.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/build_id_index.h"
#include "src/developer/debug/zxdb/symbols/debug_symbol_file_type.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/ref_counted.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class ModuleSymbols;

// Tracks a global view of all ModuleSymbols objects. Since each object is independent of load
// address, we can share these between processes that load the same binary.
//
// This is an internal object but since there is no public API, there is no "Impl" split.
class SystemSymbols {
 public:
  // What kind of downloading should be attempted for missing symbols.
  enum DownloadType {
    kNone,
    kSymbols,
    kBinary,
  };

  class DownloadHandler {
   public:
    virtual void RequestDownload(const std::string& build_id, DebugSymbolFileType file_type,
                                 bool quiet) = 0;
  };

  explicit SystemSymbols(DownloadHandler* download_handler);
  ~SystemSymbols();

  BuildIDIndex& build_id_index() { return build_id_index_; }

  // Whether to create index on ModuleSymbols. The index is used during symbol name to address
  // lookups. If you don't need this feature, disabling it will accelerate the loading time.
  void set_create_index(bool val) { create_index_ = val; }

  // Set to true to fallback for symbol searching by looking for absolute module paths on the
  // local system
  void set_enable_local_fallback(bool enable) { enable_local_fallback_ = enable; }

  // Injects a ModuleSymbols object for the given build ID. Used for testing. Normally the test
  // would provide a dummy implementation for ModuleSymbols.
  void InjectModuleForTesting(const std::string& build_id, ModuleSymbols* module);

  // Retrieves the symbols for the module with the given name and build ID. If the module's symbols
  // have already been loaded, just puts an owning reference into the given out param. If not, the
  // symbols will be loaded.
  //
  // The build ID will be used if a match is found in the index. The name will only be used if
  // local_fallback_ is set, in which case if the build ID is not found, the local filesystem will
  // be checked for the module name. The name can be empty to disable this behavior.
  //
  // Missing symbols is not counted as an error, so *module will be empty even on success in this
  // case. Errors will be from things like corrupted symbols. If a download is requested, downloads
  // will be kicked off for any missing debug files in this case.
  //
  // This function uses the build_id for loading symbols. The name is only used for generating
  // informational messages.
  Err GetModule(const std::string& name, const std::string& build_id, bool force_reload_symbols,
                fxl::RefPtr<ModuleSymbols>* module, DownloadType download_type = kSymbols);

 private:
  // Saves the given module in the modules_ map and registers for its deletion.
  void SaveModule(const std::string& build_id, ModuleSymbols* module);

  DownloadHandler* download_handler_;

  BuildIDIndex build_id_index_;

  // Index from module build ID to a non-owning ModuleSymbols pointer. The ModuleSymbols will notify
  // us when it's being deleted so the pointers stay up-to-date.
  std::map<std::string, ModuleSymbols*> modules_;

  bool create_index_ = true;
  bool enable_local_fallback_ = false;

  fxl::WeakPtrFactory<SystemSymbols> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(SystemSymbols);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_SYSTEM_SYMBOLS_H_
