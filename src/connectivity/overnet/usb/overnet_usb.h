// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_OVERNET_USB_OVERNET_USB_H_
#define SRC_CONNECTIVITY_OVERNET_USB_OVERNET_USB_H_

#include <fidl/fuchsia.hardware.overnet/cpp/wire.h>
#include <fuchsia/hardware/usb/function/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/function.h>
#include <lib/zx/socket.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <array>
#include <memory>
#include <optional>
#include <queue>
#include <thread>
#include <variant>

#include <ddktl/device.h>
#include <fbl/mutex.h>
#include <usb/request-cpp.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

#include "fbl/auto_lock.h"

class OvernetUsb;

using OvernetUsbType = ddk::Device<OvernetUsb, ddk::Unbindable, ddk::Suspendable,
                                   ddk::Messageable<fuchsia_hardware_overnet::Device>::Mixin>;
class OvernetUsb : public OvernetUsbType, public ddk::UsbFunctionInterfaceProtocol<OvernetUsb> {
 public:
  explicit OvernetUsb(zx_device_t* parent)
      : OvernetUsbType(parent),
        loop_(&kAsyncLoopConfigNoAttachToCurrentThread),
        function_(parent) {}

  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkSuspend(ddk::SuspendTxn txn);
  void DdkRelease();

  void SetCallback(fuchsia_hardware_overnet::wire::DeviceSetCallbackRequest* request,
                   SetCallbackCompleter::Sync& completer) override;

  size_t UsbFunctionInterfaceGetDescriptorsSize();
  void UsbFunctionInterfaceGetDescriptors(uint8_t* out_descriptors_buffer, size_t descriptors_size,
                                          size_t* out_descriptors_actual);
  zx_status_t UsbFunctionInterfaceControl(const usb_setup_t* setup, const uint8_t* write_buffer,
                                          size_t write_size, uint8_t* out_read_buffer,
                                          size_t read_size, size_t* out_read_actual);
  zx_status_t UsbFunctionInterfaceSetConfigured(bool configured, usb_speed_t speed);
  zx_status_t UsbFunctionInterfaceSetInterface(uint8_t interface, uint8_t alt_setting);

  static zx_status_t Create(void* ctx, zx_device_t* parent);
  zx_status_t Bind();

 private:
  // Called whenever the socket from RCS is readable. Reads data out of the socket and places it
  // into bulk IN requests.
  void HandleSocketReadable(async_dispatcher_t*, async::WaitBase*, zx_status_t status,
                            const zx_packet_signal_t*);
  // Called whenever the socket from RCS is writable. Pumps data from Running::socket_out_queue_
  // into the socket.
  void HandleSocketWritable(async_dispatcher_t*, async::WaitBase*, zx_status_t status,
                            const zx_packet_signal_t*);

  // Internal state machine for this driver. There are four states:
  //
  // Unconfigured, Ready, Running, Shutting Down.
  //
  // We can transition from the Unconfigured to the Ready state if
  // UsbFunctionInterfaceSetConfigured is called.
  //
  // We transition from Ready to Running if we receive a magic transaction from the host.
  //
  // We transition from Running to Ready if any faults happen while handling the connection, such as
  // RCS disconnecting the socket.
  //
  // We transition from any state to Shutting Down if DdkUnbind or DdkSuspend are called.
  class Unconfigured;
  class Ready;
  class Running;
  class ShuttingDown;
  using State = std::variant<Unconfigured, Ready, Running, ShuttingDown>;

  // Common interface for states where no socket is available.
  class BaseNoSocketState {
   public:
    static bool WritesWaiting() { return false; }
    static bool ReadsWaiting() { return false; }
    static bool NewSocket() { return false; }
  };

  // Unconfigured state. We have no transactions queued and cannot receive data.
  class Unconfigured : public BaseNoSocketState {
   public:
    // Called when we receive data from the host while  in this state. Warns and discards it.
    State ReceiveData(uint8_t* data, size_t len, std::optional<zx::socket>* peer_socket,
                      OvernetUsb* owner) &&;
    // Called when we are asked to send data from this state. Should never be called.
    State SendData(uint8_t*, size_t, size_t*, zx_status_t* status) && {
      *status = ZX_ERR_SHOULD_WAIT;
      return *this;
    }
    State Writable() && { return *this; }
  };

  // Ready state. We are reading data from the host, but will ignore it until we receive a 16-byte
  // magic transaction. We will not send data while in this state.
  class Ready : public BaseNoSocketState {
   public:
    // Called when we receive data from the host while in this state. Transitions to Running if the
    // data is a magic transaction, otherwise warns and discards.
    State ReceiveData(uint8_t* data, size_t len, std::optional<zx::socket>* peer_socket,
                      OvernetUsb* owner) &&;
    // Called when we are asked to send data from this state. Should never be called.
    State SendData(uint8_t*, size_t, size_t*, zx_status_t* status) && {
      *status = ZX_ERR_SHOULD_WAIT;
      return *this;
    }
    State Writable() && { return *this; }
  };

  // Running. We will send a response magic transaction to the host, followed by any data we get
  // from the RCS socket. Data we receive will be queued on the RCS socket.
  class Running {
   public:
    Running(zx::socket socket, OvernetUsb* owner)
        : socket_(std::move(socket)),
          read_waiter_(
              std::make_unique<async::WaitMethod<OvernetUsb, &OvernetUsb::HandleSocketReadable>>(
                  owner, socket_.get(), ZX_SOCKET_READABLE)),
          write_waiter_(
              std::make_unique<async::WaitMethod<OvernetUsb, &OvernetUsb::HandleSocketWritable>>(
                  owner, socket_.get(), ZX_SOCKET_WRITABLE)),
          owner_(owner) {}
    Running(Running&&) = default;
    Running& operator=(Running&& other) noexcept {
      this->~Running();
      socket_ = std::move(other.socket_);
      socket_out_queue_ = std::move(other.socket_out_queue_);
      socket_is_new_ = other.socket_is_new_;
      read_waiter_ = std::move(other.read_waiter_);
      write_waiter_ = std::move(other.write_waiter_);
      owner_ = other.owner_;
      return *this;
    }
    // Called when we receive data from the host while in this state. Pushes the data into socket_.
    State ReceiveData(uint8_t* data, size_t len, std::optional<zx::socket>* peer_socket,
                      OvernetUsb* owner) &&;
    // Called when we ware asked to send data from this state. Populates the given buffer with data
    // read from socket_.
    State SendData(uint8_t* data, size_t len, size_t* actual, zx_status_t* status) &&;
    // Whether we have data waiting to be sent to the host.
    bool WritesWaiting() { return !socket_out_queue_.empty(); }
    // Whether we are waiting to read data from the host.
    static bool ReadsWaiting() { return true; }
    bool NewSocket() const { return socket_is_new_; }
    // Whether we've successfully sent a reply magic header.
    void MagicSent();

    // Called when socket_ is writable. Pumps socket_out_queue_ into socket_.
    State Writable() &&;

    zx::socket* socket() { return &socket_; }
    async::WaitMethod<OvernetUsb, &OvernetUsb::HandleSocketReadable>* read_waiter() {
      return read_waiter_.get();
    }
    async::WaitMethod<OvernetUsb, &OvernetUsb::HandleSocketWritable>* write_waiter() {
      return write_waiter_.get();
    }

    ~Running() {
      async::PostTask(owner_->loop_.dispatcher(),
                      [read_waiter = std::move(read_waiter_),
                       write_waiter = std::move(write_waiter_), socket = std::move(socket_)]() {
                        if (read_waiter)
                          read_waiter->Cancel();
                        if (write_waiter)
                          write_waiter->Cancel();
                        (void)socket;
                      });
    }

   private:
    zx::socket socket_;
    std::vector<uint8_t> socket_out_queue_;
    bool socket_is_new_ = true;
    std::unique_ptr<async::WaitMethod<OvernetUsb, &OvernetUsb::HandleSocketReadable>> read_waiter_;
    std::unique_ptr<async::WaitMethod<OvernetUsb, &OvernetUsb::HandleSocketWritable>> write_waiter_;
    OvernetUsb* owner_;
  };
  class ShuttingDown : public BaseNoSocketState {
   public:
    explicit ShuttingDown(fit::function<void()> callback) : callback_(std::move(callback)) {}
    // Called when we receive data from the host while in this state. Warns and discards it.
    State ReceiveData(uint8_t* data, size_t len, std::optional<zx::socket>* peer_socket,
                      OvernetUsb* owner) &&;
    // Called when we receive data from the host while in this state. Ignores with SHOULD_WAIT.
    State SendData(uint8_t*, size_t, size_t*, zx_status_t* status) && {
      *status = ZX_ERR_SHOULD_WAIT;
      return std::move(*this);
    }
    // Called when a socket is writable. Shouldn't happen.
    State Writable() && { return std::move(*this); }
    // Called when shutdown has been successful.
    void FinishWithCallback() { callback_(); }

   private:
    fit::function<void()> callback_;
  };

  // Callback called when we start a new connection. Dispatches the other end of the socket we
  // create to RCS.
  class Callback {
   public:
    explicit Callback(fidl::WireSharedClient<fuchsia_hardware_overnet::Callback> fidl)
        : fidl_(std::move(fidl)) {}
    void operator()(zx::socket socket);

   private:
    fidl::WireSharedClient<fuchsia_hardware_overnet::Callback> fidl_;
  };

  // Whether we are in a state that is actively receiving data.
  bool Online() const __TA_REQUIRES(lock_) {
    return !std::holds_alternative<Unconfigured>(state_) &&
           !std::holds_alternative<ShuttingDown>(state_);
  }

  // Transition from Running to Ready, usually due to a connection error.
  void ResetState() __TA_REQUIRES(lock_) {
    if (std::holds_alternative<Running>(state_)) {
      state_ = Ready();
    }
  }

  // Get an IN request ready for use.
  std::optional<usb::Request<>> PrepareTx() __TA_REQUIRES(lock_);

  // Send a 16-byte magic packet to the host.
  void SendMagicReply(usb::Request<> request) __TA_REQUIRES(lock_);
  // Handle when RCS connects to us and is ready to receive a socket, or when we have a socket and
  // need to hand it to RCS.
  void HandleSocketAvailable() __TA_REQUIRES(lock_);

  // Transition to the ShuttingDown state and begin cleaning up driver resources (cancel and wait
  // for all pending transactions).
  void Shutdown(fit::function<void()> callback);

  // The driver framework may call DdkRelease at any point after we trigger the shutdown callback,
  // so we should not hold the lock when executing the callback or we might get a use-after-free
  // when the lock is released.
  void ShutdownComplete() __TA_EXCLUDES(lock_);

  // Handle the completion of an outstanding USB read request.
  void ReadComplete(usb_request_t* request);
  // Handle the completion of an outstanding USB write request.
  void WriteComplete(usb_request_t* request);

  // Endpoint address of our IN endpoint.
  uint8_t BulkInAddress() const { return descriptors_.in_ep.b_endpoint_address; }
  // Endpoint address of our OUT endpoint.
  uint8_t BulkOutAddress() const { return descriptors_.out_ep.b_endpoint_address; }

  // Start watching our RCS socket for readability and call HandleSocketReadable when it is
  // readable.
  void ProcessReadsFromSocket() {
    async::PostTask(loop_.dispatcher(), [this]() {
      fbl::AutoLock lock(&lock_);
      if (auto state = std::get_if<Running>(&state_)) {
        auto status = state->read_waiter()->Begin(loop_.dispatcher());
        if (status != ZX_OK && status != ZX_ERR_ALREADY_EXISTS) {
          zxlogf(ERROR, "Failed to wait on socket: %s", zx_status_get_string(status));
          ResetState();
        }
      }
    });
  }

  // Start watching our RCS socket for writability and call HandleSocketReadable when it is
  // writable.
  void ProcessWritesToSocket() {
    async::PostTask(loop_.dispatcher(), [this]() {
      fbl::AutoLock lock(&lock_);
      if (auto state = std::get_if<Running>(&state_)) {
        auto status = state->write_waiter()->Begin(loop_.dispatcher());
        if (status != ZX_OK && status != ZX_ERR_ALREADY_EXISTS) {
          zxlogf(ERROR, "Failed to wait on socket: %s", zx_status_get_string(status));
          ResetState();
        }
      }
    });
  }

  // Number of USB requests in both free_read_pool_ and free_write_pool_.
  static constexpr size_t kRequestPoolSize = 8;

  // Amount of data buffer allocated for requests in free_read_pool_ and free_write_pool_.
  static constexpr size_t kMtu = 1024;

  // USB max packet size for our interface descriptor.
  static constexpr uint16_t kMaxPacketSize = 512;

  async::Loop loop_;
  std::optional<Callback> callback_ __TA_GUARDED(lock_);
  std::optional<zx::socket> peer_socket_;

  ddk::UsbFunctionProtocolClient function_;
  size_t usb_request_size_;

  fbl::Mutex lock_;
  State state_ __TA_GUARDED(lock_) = Unconfigured();

  usb::RequestPool<> free_read_pool_ __TA_GUARDED(lock_);
  usb::RequestPool<> free_write_pool_ __TA_GUARDED(lock_);

  size_t pending_requests_ __TA_GUARDED(lock_) = 0;

  usb_request_complete_callback_t read_request_complete_ = {
      .callback =
          [](void* ctx, usb_request_t* request) {
            auto overnet_usb = reinterpret_cast<OvernetUsb*>(ctx);
            async::PostTask(overnet_usb->loop_.dispatcher(),
                            [overnet_usb, request]() { overnet_usb->ReadComplete(request); });
          },
      .ctx = this,
  };

  usb_request_complete_callback_t write_request_complete_ = {
      .callback =
          [](void* ctx, usb_request_t* request) {
            auto overnet_usb = reinterpret_cast<OvernetUsb*>(ctx);
            async::PostTask(overnet_usb->loop_.dispatcher(),
                            [overnet_usb, request]() { overnet_usb->WriteComplete(request); });
          },
      .ctx = this,
  };

  struct {
    usb_interface_descriptor_t data_interface;
    usb_endpoint_descriptor_t out_ep;
    usb_endpoint_descriptor_t in_ep;
  } __PACKED descriptors_;
};

#endif  // SRC_CONNECTIVITY_OVERNET_USB_OVERNET_USB_H_
