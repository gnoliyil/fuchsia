// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_FIDL_TRANSACTION_H_
#define SRC_DEVICES_LIB_FIDL_TRANSACTION_H_

#include <lib/fidl/cpp/wire/transaction.h>

#include <memory>
#include <variant>

#include <ddktl/fidl.h>

// Utilities for converting our fidl::Transactions to something usable by the driver C ABI
ddk::internal::Transaction MakeDdkInternalTransaction(fidl::Transaction* txn);
ddk::internal::Transaction MakeDdkInternalTransaction(std::unique_ptr<fidl::Transaction> txn);
// This returns a variant because, as an optimization, we skip performing allocations when
// synchronously processing requests. If a request has ownership taken over using
// device_fidl_transaction_take_ownership, we will perform an allocation to extend the lifetime
// of the transaction. In that case, we'll have a unique_ptr.
//
// This operation will modify its input, invalidating it.
std::variant<fidl::Transaction*, std::unique_ptr<fidl::Transaction>> FromDdkInternalTransaction(
    ddk::internal::Transaction* txn);

#endif  // SRC_DEVICES_LIB_FIDL_TRANSACTION_H_
