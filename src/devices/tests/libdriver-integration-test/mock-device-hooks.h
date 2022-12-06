// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_TESTS_LIBDRIVER_INTEGRATION_TEST_MOCK_DEVICE_HOOKS_H_
#define SRC_DEVICES_TESTS_LIBDRIVER_INTEGRATION_TEST_MOCK_DEVICE_HOOKS_H_

#include <fuchsia/device/mock/cpp/fidl.h>
#include <lib/fidl/coding.h>
#include <lib/fit/function.h>
#include <lib/fpromise/bridge.h>
#include <lib/zx/channel.h>

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "action-list.h"

namespace libdriver_integration_test {

// Base class of the hook hierarchy.  It provides default implementations that
// will return errors if invoked.
class MockDeviceHooks : public fuchsia::device::mock::MockDevice {
 public:
  using Completer = fpromise::completer<void, std::string>;
  explicit MockDeviceHooks(Completer completer);
  ~MockDeviceHooks() override = default;

  using HookInvocation = fuchsia::device::mock::HookInvocation;
  void Bind(HookInvocation record, BindCallback callback) override { Fail(__FUNCTION__); }

  void Release(HookInvocation record) override { Fail(__FUNCTION__); }

  void GetProtocol(HookInvocation record, uint32_t protocol_id,
                   GetProtocolCallback callback) override {
    Fail(__FUNCTION__);
  }

  void Unbind(HookInvocation record, UnbindCallback callback) override { Fail(__FUNCTION__); }

  void Suspend(HookInvocation record, uint8_t requested_state, bool enable_wake,
               uint8_t suspend_reason, SuspendCallback callback) override {
    Fail(__FUNCTION__);
  }

  void Resume(HookInvocation record, uint32_t requested_state, ResumeCallback callback) override {
    Fail(__FUNCTION__);
  }

  void Message(HookInvocation record, MessageCallback callback) override { Fail(__FUNCTION__); }

  void Rxrpc(HookInvocation record, RxrpcCallback callback) override { Fail(__FUNCTION__); }

  void AddDeviceDone(uint64_t action_id) final { ZX_ASSERT(false); }
  void UnbindReplyDone(uint64_t action_id) final { ZX_ASSERT(false); }
  void SuspendReplyDone(uint64_t action_id) final { ZX_ASSERT(false); }
  void ResumeReplyDone(uint64_t action_id) final { ZX_ASSERT(false); }

  void Fail(const char* function) {
    std::string message("Unexpected ");
    message.append(function);
    ADD_FAILURE() << message;
    if (completer_) {
      completer_.complete_error(std::move(message));
    }
  }

  void set_action_list_finalizer(
      fit::function<std::vector<ActionList::Action>(ActionList)> finalizer) {
    action_list_finalizer_ = std::move(finalizer);
  }

 protected:
  Completer completer_;
  fit::function<std::vector<ActionList::Action>(ActionList)> action_list_finalizer_;
};

class BindOnce : public MockDeviceHooks {
 public:
  using Callback = fit::function<ActionList(HookInvocation, Completer)>;

  BindOnce(Completer completer, Callback callback)
      : MockDeviceHooks(std::move(completer)), callback_(std::move(callback)) {}
  ~BindOnce() override = default;

  void Bind(HookInvocation record, BindCallback callback) override {
    if (!completer_) {
      return Fail(__FUNCTION__);
    }
    callback(action_list_finalizer_(callback_(record, std::move(completer_))));
  }

 private:
  Callback callback_;
};

class UnbindOnce : public MockDeviceHooks {
 public:
  using Callback = fit::function<ActionList(HookInvocation, Completer)>;

  UnbindOnce(Completer completer, Callback callback)
      : MockDeviceHooks(std::move(completer)), callback_(std::move(callback)) {}
  ~UnbindOnce() override = default;

  void Unbind(HookInvocation record, UnbindCallback callback) override {
    if (!completer_) {
      return Fail(__FUNCTION__);
    }
    callback(action_list_finalizer_(callback_(record, std::move(completer_))));
  }

 private:
  Callback callback_;
};

class ReleaseOnce : public MockDeviceHooks {
 public:
  using Callback = fit::function<void(HookInvocation, Completer)>;

  ReleaseOnce(Completer completer, Callback callback)
      : MockDeviceHooks(std::move(completer)), callback_(std::move(callback)) {}
  ~ReleaseOnce() override = default;

  void Release(HookInvocation record) override {
    if (!completer_) {
      return Fail(__FUNCTION__);
    }
    callback_(record, std::move(completer_));
  }

 private:
  Callback callback_;
};

class IgnoreGetProtocol : public MockDeviceHooks {
 public:
  explicit IgnoreGetProtocol() : MockDeviceHooks({}) {}
  ~IgnoreGetProtocol() override = default;

  void GetProtocol(HookInvocation record, uint32_t proto, GetProtocolCallback callback) override {
    ActionList actions;
    actions.AppendReturnStatus(ZX_ERR_NOT_SUPPORTED);
    callback(action_list_finalizer_(std::move(actions)));
  }

 private:
};

}  // namespace libdriver_integration_test

#endif  // SRC_DEVICES_TESTS_LIBDRIVER_INTEGRATION_TEST_MOCK_DEVICE_HOOKS_H_
