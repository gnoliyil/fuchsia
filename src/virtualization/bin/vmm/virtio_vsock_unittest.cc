// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/vmm/virtio_vsock.h"

#include <lib/gtest/test_loop_fixture.h>

#include <iterator>

#include <gtest/gtest.h>

#include "src/virtualization/bin/vmm/phys_mem_fake.h"
#include "src/virtualization/bin/vmm/virtio_device_fake.h"
#include "src/virtualization/bin/vmm/virtio_queue_fake.h"

namespace {

using ::fuchsia::virtualization::HostVsockConnector_Connect_Response;
using ::fuchsia::virtualization::HostVsockConnector_Connect_Result;

static constexpr size_t kDataSize = 4;

struct RxBuffer {
  // The number of virtio descriptors to use for this buffer.
  static constexpr size_t kNumDescriptors = 3;

  // The number of used bytes, as reported by the device when the descriptor
  // was returned.
  uint32_t used_bytes;

  virtio_vsock_hdr_t header __ALIGNED(16);
  uint8_t data[kDataSize] __ALIGNED(16);
  uint8_t data2[kDataSize] __ALIGNED(16);
};

// Ensure we have padding bytes in our structure so we don't have contiguous
// descriptors.
static_assert(offsetof(RxBuffer, data) > sizeof(RxBuffer::header),
              "RxBuffer::data is adjacent to RxBuffer::header");
static_assert(offsetof(RxBuffer, data2) > offsetof(RxBuffer, data) + sizeof(RxBuffer::data),
              "RxBuffer::data2 is adjacent to RxBuffer::data");

static constexpr uint32_t kVirtioVsockHostPort = 22;
static constexpr uint32_t kVirtioVsockGuestCid = 3;
static constexpr uint32_t kVirtioVsockGuestPort = 23;
static constexpr uint32_t kVirtioVsockGuestEphemeralPort = 1024;
static constexpr uint16_t kVirtioVsockRxBuffers = 8;
static constexpr uint16_t kVirtioVsockQueueSize = kVirtioVsockRxBuffers * RxBuffer::kNumDescriptors;
static const std::vector<uint8_t> kDefaultData = {1, 9, 8, 5};

struct ConnectionRequest {
  uint32_t src_cid;
  uint32_t src_port;
  uint32_t cid;
  uint32_t port;
  fuchsia::virtualization::HostVsockConnector::ConnectCallback callback;
};

class TestConnection {
 public:
  TestConnection() {
    FX_CHECK(zx::socket::create(ZX_SOCKET_STREAM, &socket_, &remote_socket_) == ZX_OK);
  }

  uint32_t count = 0;
  zx_status_t status = ZX_ERR_BAD_STATE;

  fuchsia::virtualization::GuestVsockAcceptor::AcceptCallback callback() {
    return [this](fuchsia::virtualization::GuestVsockAcceptor_Accept_Result result) {
      count++;
      status = result.is_err() ? result.err() : ZX_OK;
    };
  }

  zx::socket& socket() { return socket_; }
  zx::socket take_remote() { return std::move(remote_socket_); }

  bool remote_closed() const {
    zx_signals_t observed = 0;
    zx_status_t status = socket_.wait_one(__ZX_OBJECT_PEER_CLOSED, zx::time(), &observed);
    switch (status) {
      case ZX_ERR_TIMED_OUT:
        return false;
      case ZX_OK:
        return observed & __ZX_OBJECT_PEER_CLOSED;
      default:
        FX_CHECK(false) << "Unexpected status " << status;
        __UNREACHABLE;
    }
  }

  zx_status_t write(const uint8_t* data, uint32_t size, size_t* actual) {
    return socket_.write(0, data, size, actual);
  }

  zx_status_t read(uint8_t* data, uint32_t size, size_t* actual) {
    return socket_.read(0, data, size, actual);
  }

 private:
  zx::socket socket_;
  zx::socket remote_socket_;
};

using CreateCallback = fit::function<void(zx::socket*, zx::socket*)>;

static void CreateSocket(zx::socket* socket_handle, zx::socket* remote_socket_handle) {
  zx::socket socket, remote_socket;
  ASSERT_EQ(ZX_OK, zx::socket::create(ZX_SOCKET_STREAM, &socket, &remote_socket));
  *socket_handle = std::move(socket);
  *remote_socket_handle = std::move(remote_socket);
}

class VirtioVsockTest : public ::gtest::TestLoopFixture,
                        public fuchsia::virtualization::HostVsockConnector {
 public:
  VirtioVsockTest()
      : vsock_(nullptr, phys_mem_, dispatcher()),
        rx_queue_(vsock_.rx_queue(), kVirtioVsockQueueSize),
        tx_queue_(vsock_.tx_queue(), kVirtioVsockQueueSize) {}

  void SetUp() override {
    FillRxQueue();

    // Set up a GuestVsockEndpoint and GuestVsockAcceptor interface connected
    // to the VirtioVsock object.
    vsock_.Bind(endpoint_.NewRequest());
    endpoint_->SetContextId(kVirtioVsockGuestCid, connector_binding_.NewBinding(),
                            acceptor_.NewRequest());
    endpoint_.events().OnShutdown = [this](uint64_t local_cid, uint32_t local_port,
                                           uint64_t guest_cid, uint32_t remote_port) {
      received_shutdown_events_.push_back({local_cid, local_port, guest_cid, remote_port});
    };

    RunLoopUntilIdle();
  }

 protected:
  struct ShutdownEvent {
    uint64_t local_cid;
    uint32_t local_port;
    uint64_t guest_cid;
    uint32_t remote_port;
  };

  PhysMemFake phys_mem_;
  VirtioVsock vsock_;
  VirtioQueueFake rx_queue_;
  VirtioQueueFake tx_queue_;
  fuchsia::virtualization::GuestVsockEndpointPtr endpoint_;
  fuchsia::virtualization::GuestVsockAcceptorPtr acceptor_;
  fidl::Binding<fuchsia::virtualization::HostVsockConnector> connector_binding_{this};
  std::vector<zx::handle> remote_handles_;
  std::vector<ConnectionRequest> connection_requests_;
  std::vector<ConnectionRequest> connections_established_;
  std::vector<ShutdownEvent> received_shutdown_events_;
  RxBuffer rx_buffers[kVirtioVsockRxBuffers] = {};

  // Set some default credit parameters that should suffice for most tests.
  // Tests of the credit system will want to assign a more reasonable buf_alloc
  // value.
  uint32_t buf_alloc = UINT32_MAX;
  uint32_t fwd_cnt = 0;

  // |fuchsia::virtualization::HostVsockConnector|
  void Connect(uint32_t src_cid, uint32_t src_port, uint32_t cid, uint32_t port,
               fuchsia::virtualization::HostVsockConnector::ConnectCallback callback) override {
    connection_requests_.emplace_back(
        ConnectionRequest{src_cid, src_port, cid, port, std::move(callback)});
  }

  void VerifyHeader(RxBuffer* buffer, uint32_t host_port, uint32_t guest_port, size_t len,
                    uint16_t op, uint32_t flags) {
    virtio_vsock_hdr_t* header = &buffer->header;
    EXPECT_EQ(header->src_cid, fuchsia::virtualization::HOST_CID);
    EXPECT_EQ(header->dst_cid, kVirtioVsockGuestCid);
    EXPECT_EQ(header->src_port, host_port);
    EXPECT_EQ(header->dst_port, guest_port);
    EXPECT_EQ(header->len, len);
    EXPECT_EQ(header->type, VIRTIO_VSOCK_TYPE_STREAM);
    EXPECT_EQ(header->op, op);
    EXPECT_EQ(header->flags, flags);
    // Verify the bytes reported in the virtio used element matches
    // how many bytes specified in our header.
    EXPECT_EQ(buffer->used_bytes, header->len + sizeof(*header));
  }

  // Read a packet sent from the host to the guest.
  RxBuffer* DoReceive() {
    RunLoopUntilIdle();
    if (!rx_queue_.HasUsed()) {
      return nullptr;
    }
    vring_used_elem used_elem = rx_queue_.NextUsed();
    RxBuffer* buffer = &rx_buffers[used_elem.id / RxBuffer::kNumDescriptors];
    buffer->used_bytes = used_elem.len;
    return buffer;
  }

  // Send a packet from the guest to the host.
  void DoSend(uint32_t host_port, uint32_t guest_cid, uint32_t guest_port, uint16_t type,
              uint16_t op, uint32_t flags = 0) {
    virtio_vsock_hdr_t tx_header = {
        .src_cid = guest_cid,
        .dst_cid = fuchsia::virtualization::HOST_CID,
        .src_port = guest_port,
        .dst_port = host_port,
        .type = type,
        .op = op,
        .flags = flags,
        .buf_alloc = this->buf_alloc,
        .fwd_cnt = this->fwd_cnt,
    };
    ASSERT_EQ(tx_queue_.BuildDescriptor().AppendReadable(&tx_header, sizeof(tx_header)).Build(),
              ZX_OK);

    RunLoopUntilIdle();
  }

  void HostConnectOnPortRequest(uint32_t host_port, TestConnection* connection) {
    acceptor_->Accept(fuchsia::virtualization::HOST_CID, host_port, kVirtioVsockGuestPort,
                      connection->take_remote(), connection->callback());

    RxBuffer* rx_buffer = DoReceive();
    ASSERT_NE(nullptr, rx_buffer);
    VerifyHeader(rx_buffer, host_port, kVirtioVsockGuestPort, 0, VIRTIO_VSOCK_OP_REQUEST, 0);
  }

  void HostConnectOnPortResponse(uint32_t host_port) {
    DoSend(host_port, kVirtioVsockGuestCid, kVirtioVsockGuestPort, VIRTIO_VSOCK_TYPE_STREAM,
           VIRTIO_VSOCK_OP_RESPONSE);
  }

  void FillRxQueue() {
    for (size_t i = 0; i < std::size(rx_buffers); ++i) {
      ASSERT_EQ(rx_queue_.BuildDescriptor()
                    .AppendWritable(&rx_buffers[i].header, sizeof(rx_buffers[i].header))
                    .AppendWritable(&rx_buffers[i].data, sizeof(rx_buffers[i].data))
                    .AppendWritable(&rx_buffers[i].data2, sizeof(rx_buffers[i].data2))
                    .Build(),
                ZX_OK);
    }
  }

  void HostReadOnPort(uint32_t host_port, TestConnection* connection,
                      std::vector<uint8_t> expected = kDefaultData) {
    size_t actual;
    ASSERT_EQ(ZX_OK,
              connection->write(expected.data(), static_cast<uint32_t>(expected.size()), &actual));
    EXPECT_EQ(expected.size(), actual);

    RxBuffer* rx_buffer = DoReceive();
    ASSERT_NE(nullptr, rx_buffer);
    VerifyHeader(rx_buffer, host_port, kVirtioVsockGuestPort, expected.size(), VIRTIO_VSOCK_OP_RW,
                 0);

    // Verify the data, which may be spread across multiple descriptors.
    EXPECT_EQ(memcmp(rx_buffer->data, expected.data(),
                     expected.size() > kDataSize ? kDataSize : expected.size()),
              0);
    if (expected.size() > kDataSize) {
      EXPECT_EQ(memcmp(rx_buffer->data2, expected.data() + kDataSize, expected.size() - kDataSize),
                0);
    }
  }

  void HostQueueWriteOnPort(uint32_t host_port, uint8_t* tx_buffer, size_t len) {
    auto tx_header = reinterpret_cast<virtio_vsock_hdr_t*>(tx_buffer);
    ASSERT_GE(len, sizeof(*tx_header));
    *tx_header = {
        .src_cid = kVirtioVsockGuestCid,
        .dst_cid = fuchsia::virtualization::HOST_CID,
        .src_port = kVirtioVsockGuestPort,
        .dst_port = host_port,
        .len = static_cast<uint32_t>(len - sizeof(*tx_header)),
        .type = VIRTIO_VSOCK_TYPE_STREAM,
        .op = VIRTIO_VSOCK_OP_RW,
        .buf_alloc = this->buf_alloc,
        .fwd_cnt = this->fwd_cnt,
    };

    ASSERT_EQ(tx_queue_.BuildDescriptor().AppendReadable(tx_buffer, len).Build(), ZX_OK);
  }

  void HostWriteOnPort(uint32_t host_port, TestConnection* connection) {
    uint8_t tx_buffer[sizeof(virtio_vsock_hdr_t) + kDataSize] = {};
    uint8_t expected_data[kDataSize] = {2, 3, 0, 1};
    memcpy(tx_buffer + sizeof(virtio_vsock_hdr_t), expected_data, kDataSize);
    HostQueueWriteOnPort(host_port, tx_buffer, sizeof(tx_buffer));

    RunLoopUntilIdle();

    uint8_t actual_data[kDataSize];
    size_t actual;
    ASSERT_EQ(ZX_OK, connection->read(actual_data, sizeof(actual_data), &actual));
    EXPECT_EQ(actual, kDataSize);
    EXPECT_EQ(memcmp(actual_data, expected_data, kDataSize), 0);
  }

  void HostConnectOnPort(uint32_t host_port, TestConnection* connection) {
    HostConnectOnPortRequest(host_port, connection);
    HostConnectOnPortResponse(host_port);
    RunLoopUntilIdle();
    ASSERT_EQ(1u, connection->count);
    ASSERT_EQ(ZX_OK, connection->status);
    ASSERT_FALSE(connection->remote_closed());
    HostReadOnPort(host_port, connection);
  }

  void HostShutdownOnPort(uint32_t host_port, uint32_t flags) {
    RxBuffer* rx_buffer = DoReceive();
    ASSERT_NE(nullptr, rx_buffer);
    VerifyHeader(rx_buffer, host_port, kVirtioVsockGuestPort, 0, VIRTIO_VSOCK_OP_SHUTDOWN, flags);
  }

  void ExpectHostResetOnPort(uint32_t host_port) {
    RxBuffer* rx_buffer = DoReceive();
    ASSERT_NE(nullptr, rx_buffer);
    VerifyHeader(rx_buffer, host_port, kVirtioVsockGuestPort, 0, VIRTIO_VSOCK_OP_RST, 0);
  }

  void GuestConnectOnPortRequest(uint32_t host_port, uint32_t guest_port) {
    DoSend(host_port, kVirtioVsockGuestCid, guest_port, VIRTIO_VSOCK_TYPE_STREAM,
           VIRTIO_VSOCK_OP_REQUEST);
    RunLoopUntilIdle();
  }

  void GuestConnectInvokeCallbacks(zx_status_t status, CreateCallback create_callback) {
    for (auto it = connection_requests_.begin(); it != connection_requests_.end();
         it = connection_requests_.erase(it)) {
      zx::socket socket, remote_socket;
      if (status == ZX_OK) {
        create_callback(&socket, &remote_socket);
        remote_handles_.emplace_back(std::move(remote_socket));
      }

      it->callback(status == ZX_OK ? HostVsockConnector_Connect_Result::WithResponse(
                                         HostVsockConnector_Connect_Response(std::move(socket)))
                                   : HostVsockConnector_Connect_Result::WithErr(std::move(status)));

      connections_established_.emplace_back(
          ConnectionRequest{it->src_cid, it->src_port, it->cid, it->port, nullptr});
      RunLoopUntilIdle();
    }
  }

  void GuestConnectOnPortResponse(uint32_t host_port, uint16_t op, uint32_t guest_port) {
    RxBuffer* rx_buffer = DoReceive();
    ASSERT_NE(nullptr, rx_buffer);
    VerifyHeader(rx_buffer, host_port, guest_port, 0, op, 0);
    if (remote_handles_.empty()) {
      EXPECT_EQ(rx_buffer->header.buf_alloc, 0u);
    } else {
      EXPECT_GT(rx_buffer->header.buf_alloc, 0u);
    }
    EXPECT_EQ(rx_buffer->header.fwd_cnt, 0u);
  }

  void GuestConnectOnPort(uint32_t host_port, uint32_t guest_port, CreateCallback create_callback) {
    GuestConnectOnPortRequest(host_port, guest_port);
    GuestConnectInvokeCallbacks(ZX_OK, std::move(create_callback));
    GuestConnectOnPortResponse(host_port, VIRTIO_VSOCK_OP_RESPONSE, guest_port);
  }

  virtio_vsock_hdr_t* GetCreditUpdate() {
    DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
           VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_CREDIT_REQUEST);
    RxBuffer* rx_buffer = DoReceive();
    if (rx_buffer == nullptr) {
      return nullptr;
    }
    VerifyHeader(rx_buffer, kVirtioVsockHostPort, kVirtioVsockGuestPort, 0,
                 VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0);
    return &rx_buffer->header;
  }

  void SendCreditUpdate(uint32_t host_port, uint32_t guest_port) {
    DoSend(host_port, kVirtioVsockGuestCid, guest_port, VIRTIO_VSOCK_TYPE_STREAM,
           VIRTIO_VSOCK_OP_CREDIT_UPDATE);
  }

  // Return a list of `OnShutdown` events that have been received from the
  // virtio-vsock device to the endpoints.
  const std::vector<ShutdownEvent>& received_shutdown_events() { return received_shutdown_events_; }
};

TEST_F(VirtioVsockTest, Connect) {
  TestConnection connection;
  HostConnectOnPort(kVirtioVsockHostPort, &connection);
}

TEST_F(VirtioVsockTest, ConnectMultipleTimes) {
  {
    TestConnection connection;
    HostConnectOnPort(kVirtioVsockHostPort, &connection);
  }
  HostShutdownOnPort(kVirtioVsockHostPort, VIRTIO_VSOCK_FLAG_SHUTDOWN_BOTH);
  {
    TestConnection connection;
    HostConnectOnPort(kVirtioVsockHostPort + 1000, &connection);
  }
}

TEST_F(VirtioVsockTest, ConnectMultipleTimesSamePort) {
  {
    TestConnection connection;
    HostConnectOnPort(kVirtioVsockHostPort, &connection);
  }

  TestConnection connection;
  acceptor_->Accept(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort, kVirtioVsockGuestPort,
                    connection.take_remote(), connection.callback());
  RunLoopUntilIdle();
  ASSERT_EQ(1u, connection.count);
  ASSERT_EQ(ZX_ERR_ALREADY_BOUND, connection.status);
  ASSERT_TRUE(connection.remote_closed());
}

TEST_F(VirtioVsockTest, ConnectEarlyData) {
  TestConnection connection;

  // Put some initial data on the connection
  size_t actual;
  ASSERT_EQ(
      connection.write(kDefaultData.data(), static_cast<uint32_t>(kDefaultData.size()), &actual),
      ZX_OK);

  HostConnectOnPort(kVirtioVsockHostPort, &connection);
}

TEST_F(VirtioVsockTest, ConnectRefused) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);

  // Test connection reset.
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
         VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_RST);
  RunLoopUntilIdle();

  ASSERT_EQ(1u, connection.count);
  ASSERT_EQ(ZX_ERR_CONNECTION_REFUSED, connection.status);
  ASSERT_TRUE(connection.remote_closed());
  EXPECT_FALSE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                    kVirtioVsockGuestPort));
}

TEST_F(VirtioVsockTest, Listen) {
  GuestConnectOnPort(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort, CreateSocket);
  ASSERT_EQ(1u, connections_established_.size());
  ASSERT_TRUE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                   kVirtioVsockGuestEphemeralPort));
}

TEST_F(VirtioVsockTest, ListenMultipleTimes) {
  GuestConnectOnPort(kVirtioVsockHostPort + 1, kVirtioVsockGuestEphemeralPort + 1, CreateSocket);
  GuestConnectOnPort(kVirtioVsockHostPort + 2, kVirtioVsockGuestEphemeralPort + 2, CreateSocket);
  ASSERT_EQ(2u, connections_established_.size());
  ASSERT_TRUE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort + 1,
                                   kVirtioVsockGuestEphemeralPort + 1));
  ASSERT_TRUE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort + 2,
                                   kVirtioVsockGuestEphemeralPort + 2));
}

TEST_F(VirtioVsockTest, ListenMultipleTimesSameHostPort) {
  GuestConnectOnPort(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort, CreateSocket);
  GuestConnectOnPort(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort + 1, CreateSocket);

  EXPECT_TRUE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                   kVirtioVsockGuestEphemeralPort));
  EXPECT_TRUE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                   kVirtioVsockGuestEphemeralPort + 1));
}

TEST_F(VirtioVsockTest, ListenMultipleTimesSamePort) {
  GuestConnectOnPort(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort, CreateSocket);
  EXPECT_TRUE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                   kVirtioVsockGuestEphemeralPort));

  GuestConnectOnPortRequest(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort);
  GuestConnectOnPortResponse(kVirtioVsockHostPort, VIRTIO_VSOCK_OP_RST,
                             kVirtioVsockGuestEphemeralPort);
  EXPECT_FALSE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                    kVirtioVsockGuestEphemeralPort));
}

TEST_F(VirtioVsockTest, ListenRefused) {
  GuestConnectOnPortRequest(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort);
  GuestConnectInvokeCallbacks(ZX_ERR_CONNECTION_REFUSED, CreateSocket);
  GuestConnectOnPortResponse(kVirtioVsockHostPort, VIRTIO_VSOCK_OP_RST,
                             kVirtioVsockGuestEphemeralPort);
  EXPECT_FALSE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                    kVirtioVsockGuestEphemeralPort));
}

TEST_F(VirtioVsockTest, ListenWrongCid) {
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid + 1000, kVirtioVsockGuestEphemeralPort,
         VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_REQUEST);
  RunLoopUntilIdle();

  EXPECT_FALSE(vsock_.HasConnection(fuchsia::virtualization::HOST_CID, kVirtioVsockHostPort,
                                    kVirtioVsockGuestEphemeralPort));
}

TEST_F(VirtioVsockTest, Reset) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);
  connection.socket().reset();
  RunLoopUntilIdle();
  HostShutdownOnPort(kVirtioVsockHostPort, VIRTIO_VSOCK_FLAG_SHUTDOWN_BOTH);
}

// The device should not send any response to a spurious reset packet.
TEST_F(VirtioVsockTest, NoResponseToSpuriousReset) {
  // Send a reset from the guest.
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
         VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_RST);
  RunLoopUntilIdle();

  // Expect no response from the host.
  RxBuffer* buffer = DoReceive();
  EXPECT_EQ(nullptr, buffer);
}

TEST_F(VirtioVsockTest, ShutdownRead) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  ASSERT_EQ(connection.socket().set_disposition(ZX_SOCKET_DISPOSITION_WRITE_DISABLED, 0), ZX_OK);
  HostShutdownOnPort(kVirtioVsockHostPort, VIRTIO_VSOCK_FLAG_SHUTDOWN_RECV);
}

TEST_F(VirtioVsockTest, ShutdownWrite) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  ASSERT_EQ(connection.socket().set_disposition(0, ZX_SOCKET_DISPOSITION_WRITE_DISABLED), ZX_OK);
  HostShutdownOnPort(kVirtioVsockHostPort, VIRTIO_VSOCK_FLAG_SHUTDOWN_SEND);
}

// Ensure endpoints are notified when a socket has been shutdown.
TEST_F(VirtioVsockTest, ShutdownNotifiesEndpoint) {
  TestConnection connection;

  // Establish a connection between the host and guest.
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  // Have the guest shutdown the stream.
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
         VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_SHUTDOWN,
         /*flags=*/VIRTIO_VSOCK_FLAG_SHUTDOWN_BOTH);
  RunLoopUntilIdle();

  // Ensure the host reset the connection.
  ExpectHostResetOnPort(kVirtioVsockHostPort);
  RunLoopUntilIdle();

  // Ensure the endpoint was sent a shutdown event.
  ASSERT_EQ(received_shutdown_events().size(), 1u);
  const ShutdownEvent& shutdown_event = received_shutdown_events()[0];
  EXPECT_EQ(shutdown_event.guest_cid, kVirtioVsockGuestCid);
  EXPECT_EQ(shutdown_event.local_cid, fuchsia::virtualization::HOST_CID);
  EXPECT_EQ(shutdown_event.local_port, kVirtioVsockHostPort);
  EXPECT_EQ(shutdown_event.remote_port, kVirtioVsockGuestPort);
}

// Endpoints should not be sent OnShutdown events when virtio-vsock is
// merely responding to spurious packets.
TEST_F(VirtioVsockTest, SpuriousPacketsDoNotNotifyEndpoints) {
  for (uint16_t packet_op : std::vector<uint16_t>{
           VIRTIO_VSOCK_OP_SHUTDOWN,
           VIRTIO_VSOCK_OP_RESPONSE,
           VIRTIO_VSOCK_OP_CREDIT_UPDATE,
           VIRTIO_VSOCK_OP_CREDIT_REQUEST,
           VIRTIO_VSOCK_OP_INVALID,
           VIRTIO_VSOCK_OP_RW,
       }) {
    SCOPED_TRACE(testing::Message() << "Testing packet operation #" << packet_op);

    // Guest sends a spurious shutdown event.
    DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
           VIRTIO_VSOCK_TYPE_STREAM, packet_op);
    RunLoopUntilIdle();

    // We expect a reset from the host.
    ExpectHostResetOnPort(kVirtioVsockHostPort);
    RunLoopUntilIdle();

    // Ensure that no shutdown events were sent to the endpoints.
    EXPECT_TRUE(received_shutdown_events().empty());
  }
}

TEST_F(VirtioVsockTest, WriteAfterShutdown) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  ASSERT_EQ(connection.socket().set_disposition(0, ZX_SOCKET_DISPOSITION_WRITE_DISABLED), ZX_OK);
  HostShutdownOnPort(kVirtioVsockHostPort, VIRTIO_VSOCK_FLAG_SHUTDOWN_SEND);

  // Test write after shutdown.
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
         VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_RW);

  RxBuffer* rx_buffer = DoReceive();
  ASSERT_NE(nullptr, rx_buffer);
  VerifyHeader(rx_buffer, kVirtioVsockHostPort, kVirtioVsockGuestPort, 0, VIRTIO_VSOCK_OP_RST, 0);
}

TEST_F(VirtioVsockTest, Read) {
  // Fill a single data buffer in the RxBuffer.
  std::vector<uint8_t> data = {1, 2, 3, 4};
  ASSERT_EQ(data.size(), kDataSize);

  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);
  HostReadOnPort(kVirtioVsockHostPort, &connection, data);
  HostReadOnPort(kVirtioVsockHostPort, &connection, data);
}

TEST_F(VirtioVsockTest, ReadChained) {
  // Fill both data buffers in the RxBuffer.
  std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_EQ(data.size(), 2 * kDataSize);

  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);
  HostReadOnPort(kVirtioVsockHostPort, &connection, data);
  HostReadOnPort(kVirtioVsockHostPort, &connection, data);
}

TEST_F(VirtioVsockTest, ReadNoBuffer) {
  // Set the guest buf_alloc to something smaller than our data transfer.
  buf_alloc = 2;
  std::vector<uint8_t> expected = {1, 2, 3, 4};
  ASSERT_EQ(expected.size(), 2 * buf_alloc);

  // Setup connection.
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  // Write data to socket.
  size_t actual;
  ASSERT_EQ(connection.socket().write(0, expected.data(), expected.size(), &actual), ZX_OK);
  EXPECT_EQ(actual, expected.size());

  // Expect the guest to pull off |buf_alloc| bytes.
  RxBuffer* rx_buffer = DoReceive();
  ASSERT_NE(nullptr, rx_buffer);
  VerifyHeader(rx_buffer, kVirtioVsockHostPort, kVirtioVsockGuestPort, buf_alloc,
               VIRTIO_VSOCK_OP_RW, 0);
  EXPECT_EQ(memcmp(rx_buffer->data, expected.data(), buf_alloc), 0);

  // Update credit to indicate the in-flight bytes have been free'd.
  fwd_cnt += buf_alloc;
  SendCreditUpdate(kVirtioVsockHostPort, kVirtioVsockGuestPort);

  // Expect to receive the remaining bytes
  rx_buffer = DoReceive();
  ASSERT_NE(nullptr, rx_buffer);
  VerifyHeader(rx_buffer, kVirtioVsockHostPort, kVirtioVsockGuestPort, buf_alloc,
               VIRTIO_VSOCK_OP_RW, 0);
  EXPECT_EQ(memcmp(rx_buffer->data, expected.data() + buf_alloc, buf_alloc), 0);
}

TEST_F(VirtioVsockTest, Write) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);
  HostWriteOnPort(kVirtioVsockHostPort, &connection);
  HostWriteOnPort(kVirtioVsockHostPort, &connection);
}

struct SingleBytePacket {
  virtio_vsock_hdr_t header;
  char c;

  SingleBytePacket(char c_) : c(c_) {}
} __PACKED;

TEST_F(VirtioVsockTest, WriteMultiple) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  SingleBytePacket p1('a');
  SingleBytePacket p2('b');
  HostQueueWriteOnPort(kVirtioVsockHostPort, reinterpret_cast<uint8_t*>(&p1), sizeof(p1));
  HostQueueWriteOnPort(kVirtioVsockHostPort, reinterpret_cast<uint8_t*>(&p2), sizeof(p2));
  RunLoopUntilIdle();

  size_t actual_len = 0;
  uint8_t actual_data[3] = {};
  ASSERT_EQ(connection.socket().read(0, actual_data, sizeof(actual_data), &actual_len), ZX_OK);
  ASSERT_EQ(2u, actual_len);
  ASSERT_EQ('a', actual_data[0]);
  ASSERT_EQ('b', actual_data[1]);
}

TEST_F(VirtioVsockTest, WriteUpdateCredit) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  SingleBytePacket p1('a');
  SingleBytePacket p2('b');
  HostQueueWriteOnPort(kVirtioVsockHostPort, reinterpret_cast<uint8_t*>(&p1), sizeof(p1));
  HostQueueWriteOnPort(kVirtioVsockHostPort, reinterpret_cast<uint8_t*>(&p2), sizeof(p2));
  RunLoopUntilIdle();

  // Request credit update, expect 0 fwd_cnt bytes as the data is still in the
  // socket.
  virtio_vsock_hdr_t* header = GetCreditUpdate();
  EXPECT_EQ(header->op, VIRTIO_VSOCK_OP_CREDIT_UPDATE);
  EXPECT_GT(header->buf_alloc, 0u);
  EXPECT_EQ(header->fwd_cnt, 0u);

  // Read from socket.
  size_t actual_len = 0;
  uint8_t actual_data[3] = {};
  ASSERT_EQ(connection.socket().read(0, actual_data, sizeof(actual_data), &actual_len), ZX_OK);
  ASSERT_EQ(2u, actual_len);
  ASSERT_EQ('a', actual_data[0]);
  ASSERT_EQ('b', actual_data[1]);

  // Request credit update, expect 2 fwd_cnt bytes as the data has been
  // extracted from the socket.
  header = GetCreditUpdate();
  EXPECT_EQ(header->op, VIRTIO_VSOCK_OP_CREDIT_UPDATE);
  EXPECT_GT(header->buf_alloc, 0u);
  EXPECT_EQ(header->fwd_cnt, 2u);
}

TEST_F(VirtioVsockTest, WriteSocketFullReset) {
  // If the guest writes enough bytes to overflow our socket buffer then we
  // must reset the connection as we would lose data.
  //
  // 5.7.6.3.1: VIRTIO_VSOCK_OP_RW data packets MUST only be transmitted when
  // the peer has sufficient free buffer space for the payload.
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  zx_info_socket_t info = {};
  ASSERT_EQ(ZX_OK,
            connection.socket().get_info(ZX_INFO_SOCKET, &info, sizeof(info), nullptr, nullptr));
  size_t buf_size = info.tx_buf_max + sizeof(virtio_vsock_hdr_t) + 1;
  auto buf = std::make_unique<uint8_t[]>(buf_size);
  memset(buf.get(), 'a', buf_size);

  // Queue one descriptor that will completely fill the socket (and then some),
  // We'll verify that this resets the connection.
  HostQueueWriteOnPort(kVirtioVsockHostPort, buf.get(), buf_size);
  RunLoopUntilIdle();

  RxBuffer* reset = DoReceive();
  ASSERT_NE(nullptr, reset);
  VerifyHeader(reset, kVirtioVsockHostPort, kVirtioVsockGuestPort, 0, VIRTIO_VSOCK_OP_RST, 0);
}

TEST_F(VirtioVsockTest, SendCreditUpdateWhenSocketIsDrained) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  // Fill socket buffer completely.
  zx_info_socket_t info = {};
  ASSERT_EQ(ZX_OK,
            connection.socket().get_info(ZX_INFO_SOCKET, &info, sizeof(info), nullptr, nullptr));
  size_t buf_size = info.tx_buf_max + sizeof(virtio_vsock_hdr_t);
  auto buf = std::make_unique<uint8_t[]>(buf_size);
  memset(buf.get(), 'a', buf_size);
  HostQueueWriteOnPort(kVirtioVsockHostPort, buf.get(), buf_size);
  RunLoopUntilIdle();

  // No buffers should be available to read.
  ASSERT_EQ(nullptr, DoReceive());

  // Read a single byte from socket to free up space in the socket buffer and
  // make the socket writable again.
  memset(buf.get(), 0, buf_size);
  uint8_t byte;
  size_t actual_len = 0;
  ASSERT_EQ(connection.socket().read(0, &byte, 1, &actual_len), ZX_OK);
  ASSERT_EQ(1u, actual_len);
  ASSERT_EQ('a', byte);

  // Verify we get a credit update now that the socket is wirtable.
  RunLoopUntilIdle();
  RxBuffer* credit_update = DoReceive();
  ASSERT_NE(credit_update, nullptr);
  ASSERT_EQ(info.tx_buf_max, credit_update->header.buf_alloc);
  ASSERT_EQ(actual_len, credit_update->header.fwd_cnt);
}

TEST_F(VirtioVsockTest, MultipleConnections) {
  TestConnection a_connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort + 1000, &a_connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort + 1000);

  TestConnection b_connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort + 2000, &b_connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort + 2000);

  for (auto i = 0; i < (kVirtioVsockRxBuffers / 4); i++) {
    HostReadOnPort(kVirtioVsockHostPort + 1000, &a_connection);
    HostReadOnPort(kVirtioVsockHostPort + 2000, &b_connection);
    HostWriteOnPort(kVirtioVsockHostPort + 1000, &a_connection);
    HostWriteOnPort(kVirtioVsockHostPort + 2000, &b_connection);
  }
}

TEST_F(VirtioVsockTest, InvalidBuffer) {
  zx::socket mock_socket;
  ConnectionKey mock_key{0, 0, 0, 0};
  std::unique_ptr<VirtioVsock::Connection> conn =
      VirtioVsock::Connection::Create(mock_key, std::move(mock_socket), nullptr, nullptr, nullptr);

  conn.get()->SetCredit(0, 2);
  ASSERT_EQ(conn.get()->op(), VIRTIO_VSOCK_OP_RST);
}

TEST_F(VirtioVsockTest, CreditRequest) {
  TestConnection connection;
  HostConnectOnPortRequest(kVirtioVsockHostPort, &connection);
  HostConnectOnPortResponse(kVirtioVsockHostPort);

  // Test credit request.
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort,
         VIRTIO_VSOCK_TYPE_STREAM, VIRTIO_VSOCK_OP_CREDIT_REQUEST);

  RxBuffer* rx_buffer = DoReceive();
  ASSERT_NE(nullptr, rx_buffer);
  VerifyHeader(rx_buffer, kVirtioVsockHostPort, kVirtioVsockGuestPort, 0,
               VIRTIO_VSOCK_OP_CREDIT_UPDATE, 0);
  EXPECT_GT(rx_buffer->header.buf_alloc, 0u);
  EXPECT_EQ(rx_buffer->header.fwd_cnt, 0u);
}

TEST_F(VirtioVsockTest, UnsupportedSocketType) {
  // Test connection request with invalid type.
  DoSend(kVirtioVsockHostPort, kVirtioVsockGuestCid, kVirtioVsockGuestPort, UINT16_MAX,
         VIRTIO_VSOCK_OP_REQUEST);

  RxBuffer* rx_buffer = DoReceive();
  ASSERT_NE(nullptr, rx_buffer);
  virtio_vsock_hdr_t* rx_header = &rx_buffer->header;
  EXPECT_EQ(rx_header->src_cid, fuchsia::virtualization::HOST_CID);
  EXPECT_EQ(rx_header->dst_cid, kVirtioVsockGuestCid);
  EXPECT_EQ(rx_header->src_port, kVirtioVsockHostPort);
  EXPECT_EQ(rx_header->dst_port, kVirtioVsockGuestPort);
  EXPECT_EQ(rx_header->type, VIRTIO_VSOCK_TYPE_STREAM);
  EXPECT_EQ(rx_header->op, VIRTIO_VSOCK_OP_RST);
  EXPECT_EQ(rx_header->flags, 0u);
}

TEST_F(VirtioVsockTest, InitialCredit) {
  GuestConnectOnPort(kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort, CreateSocket);
  ASSERT_TRUE(remote_handles_.size() == 1);

  zx::socket socket(std::move(remote_handles_.back()));

  std::vector<uint8_t> expected = {1, 2, 3, 4};

  // Write data to socket.
  size_t actual;
  ASSERT_EQ(socket.write(0, expected.data(), expected.size(), &actual), ZX_OK);
  EXPECT_EQ(actual, expected.size());

  // Expect that the connection has correct initial credit and so the guest
  // will be able to pull out the data.
  RxBuffer* rx_buffer = DoReceive();
  ASSERT_NE(nullptr, rx_buffer);
  VerifyHeader(rx_buffer, kVirtioVsockHostPort, kVirtioVsockGuestEphemeralPort, expected.size(),
               VIRTIO_VSOCK_OP_RW, 0);
  EXPECT_EQ(memcmp(rx_buffer->data, expected.data(), expected.size()), 0);
}

TEST(VirtioVsockChain, AllocateAndFree) {
  VirtioDeviceFake device;
  VirtioQueue* queue = device.queue();
  VirtioQueueFake* fake_queue = device.queue_fake();

  // Add an item to the queue.
  virtio_vsock_hdr_t header = {
      .src_port = 1234,
  };
  uint16_t index;
  fake_queue->BuildDescriptor().AppendReadable(&header, sizeof(header)).Build(&index);

  // Ensure we can take the item off the queue and read the header value from it.
  std::optional<VsockChain> chain = VsockChain::FromQueue(queue, /*writable=*/false);
  ASSERT_TRUE(chain.has_value());
  EXPECT_EQ(chain->header()->src_port, 1234u);
  EXPECT_TRUE(!queue->HasAvail());

  // Return the item.
  chain->Return(/*used=*/0);
  EXPECT_TRUE(fake_queue->HasUsed());
}

TEST(VirtioVsockChain, AllocateEmptyQueue) {
  VirtioDeviceFake device;

  // Attempt to take an item off the queue. It should fail.
  std::optional<VsockChain> chain = VsockChain::FromQueue(device.queue(), /*writable=*/false);
  EXPECT_FALSE(chain.has_value());
}

TEST(VirtioVsockChain, AllocateSkipsBadDescriptors) {
  VirtioDeviceFake device;
  VirtioQueue* queue = device.queue();
  VirtioQueueFake* fake_queue = device.queue_fake();

  // Add a too-short descriptor.
  uint8_t byte;
  uint16_t too_small_id;
  fake_queue->BuildDescriptor().AppendReadable(&byte, sizeof(byte)).Build(&too_small_id);

  // Add a writable descriptor when the caller will ask for a readable descriptor.
  virtio_vsock_hdr_t writable_header{};
  uint16_t writable_header_id;
  fake_queue->BuildDescriptor()
      .AppendWritable(&writable_header, sizeof(writable_header))
      .Build(&writable_header_id);

  // Add a valid descriptor.
  virtio_vsock_hdr_t header{
      .src_port = 1234,
  };
  uint16_t valid_id;
  fake_queue->BuildDescriptor().AppendReadable(&header, sizeof(header)).Build(&valid_id);

  // Ensure the valid descriptor was read.
  std::optional<VsockChain> chain = VsockChain::FromQueue(queue, /*writable=*/false);
  ASSERT_TRUE(chain.has_value());
  EXPECT_EQ(chain->header()->src_port, 1234u);

  // Ensure the two invalid descriptors were returned.
  EXPECT_EQ(too_small_id, fake_queue->NextUsed().id);
  EXPECT_EQ(writable_header_id, fake_queue->NextUsed().id);

  chain->Return(/*used=*/0);
}

}  // namespace
