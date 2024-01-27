// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_DDKTL_INCLUDE_DDKTL_FIDL_H_
#define SRC_LIB_DDKTL_INCLUDE_DDKTL_FIDL_H_

#include <lib/ddk/device.h>
#include <lib/fidl/cpp/wire/message.h>
#include <lib/fidl/cpp/wire/transaction.h>
#include <lib/zx/channel.h>
#include <zircon/fidl.h>

#include <memory>
#include <type_traits>

namespace ddk {

namespace internal {

class Transaction {
 public:
  explicit Transaction(device_fidl_txn_t txn) : txn_(txn) {}

  fidl_txn_t* Txn() { return &txn_.txn; }
  const fidl_txn_t* Txn() const { return &txn_.txn; }

  uintptr_t DriverHostCtx() const { return txn_.driver_host_context; }

  device_fidl_txn_t* DeviceFidlTxn() { return &txn_; }

  // Utilizes a |fidl_txn_t| object as a wrapped Transaction.
  //
  // Only safe to call if |txn| was previously returned by |Transaction.Txn()|.
  static Transaction* FromTxn(fidl_txn_t* txn);

  // Moves txn into a new Transaction.
  //
  // Only intended to be used by ddk::Transaction.
  // This is useful for copying a Transaction out of stack-allocated scope,
  // so a response may be generated asynchronously.
  //
  // Only safe to call if |txn| was previously returned by |Transaction.Txn()|.
  static Transaction MoveTxn(fidl_txn_t* txn);

 private:
  device_fidl_txn_t txn_;
};

inline Transaction* Transaction::FromTxn(fidl_txn_t* txn) {
  static_assert(std::is_standard_layout<Transaction>::value,
                "Cannot cast from non-standard layout class");
  static_assert(offsetof(Transaction, txn_) == 0, "Transaction must be convertable to txn");
  return reinterpret_cast<Transaction*>(txn);
}

inline Transaction Transaction::MoveTxn(fidl_txn_t* txn) {
  auto real_txn = FromTxn(txn);

  auto new_value = *real_txn;
  // Invalidate the old version
  real_txn->txn_.driver_host_context = 0;
  return new_value;
}

}  // namespace internal

// An implementation of |fidl::Transaction| for using LLCPP bindings in drivers,
// designed to work with ::DdkMessage.  If can be used to reply synchronously as in:
// zx_status_t DdkFidlDevice::DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
//     ddk::Transaction transaction(txn);
//     fidl::WireDispatch<fuchsia::hardware::serial::Device>(this, msg, &transaction);
//     return transaction.Status();
// }
// void DdkFidlDevice::GetClass(GetClassCompleter::Sync& completer) {
//     completer.Reply(fuchsia::hardware::serial::Class::CONSOLE);
// }
//
// And also can be used to reply asynchronously via ToAsync() call as in:
//
// zx_status_t DdkFidlDevice::DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
//   ddk::Transaction transaction(txn);
//   fidl::WireDispatch<fuchsia::hardware::serial::Device>(this, msg, &transaction);
//   return ZX_ERR_AYSNC; // Ownership of transaction was taken, can't use transaction.Status()
//   here.
// }
// void DdkFidlDevice::GetClass(GetClassCompleter::Sync& completer) {
//   auto async_completer = new Wrapper(completer.ToAsync());
//   DdkScheduleWork([](void* ctx) {
//     auto* wrapper = reinterpret_cast<Wrapper*>(ctx);
//     wrapper->completer.Reply(fuchsia::hardware::serial::Class::CONSOLE);
//     delete wrapper;
//   });
// }
//
// Note that this class is not thread safe.
class Transaction final : public fidl::Transaction {
 public:
  explicit Transaction(fidl_txn_t* txn) : connection_(ddk::internal::Transaction::MoveTxn(txn)) {}

  ~Transaction() override {
    ZX_ASSERT_MSG(ownership_taken_ || status_called_,
                  "Sync Transaction must have it's Status() method used.\n"
                  "This provides ::DdkMessage with the correct status value.\n"
                  "If ToAsync() was called, the Transaction ownership was taken and\n"
                  "Status() must not be called in ::DdkMessage\n");
  }

  /// Status() return the internal state of the DDK transaction. This MUST be called
  /// to bridge the Transaction and DDK dispatcher.
  zx_status_t Status() __WARN_UNUSED_RESULT {
    status_called_ = true;
    if (status_ == ZX_OK && ownership_taken_) {
      return ZX_ERR_ASYNC;
    }
    return status_;
  }

  void set_status(zx_status_t status) { status_ = status; }
  fidl_txn_t* fidl_txn() { return connection_.Txn(); }

 protected:
  zx_status_t Reply(fidl::OutgoingMessage* message, fidl::WriteOptions write_options) final {
    if (closed_) {
      return ZX_ERR_CANCELED;
    }
    fidl_outgoing_msg_t c_msg = std::move(*message).ReleaseToEncodedCMessage();
    status_ = connection_.Txn()->reply(connection_.Txn(), &c_msg);
    return status_;
  }

  void Close(zx_status_t epitaph) final {
    closed_ = true;
    status_ = epitaph;
  }

  std::unique_ptr<fidl::Transaction> TakeOwnership() final {
    ownership_taken_ = true;

    device_fidl_txn_t new_fidl_txn;
    device_fidl_transaction_take_ownership(connection_.Txn(), &new_fidl_txn);
    auto new_txn = std::make_unique<Transaction>(std::move(*this));
    new_txn->connection_ = ddk::internal::Transaction(new_fidl_txn);
    return new_txn;
  }

 private:
  ddk::internal::Transaction connection_;  // includes a fidl_txn_t.
  zx_status_t status_ = ZX_OK;
  bool closed_ = false;
  bool status_called_ = false;
  bool ownership_taken_ = false;
};

}  // namespace ddk

#endif  // SRC_LIB_DDKTL_INCLUDE_DDKTL_FIDL_H_
