// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/network/mdns/service/agents/instance_responder.h"

#include <lib/zx/time.h>

#include <gtest/gtest.h>

#include "src/connectivity/network/mdns/service/common/mdns_names.h"
#include "src/connectivity/network/mdns/service/common/service_instance.h"
#include "src/connectivity/network/mdns/service/test/agent_test.h"

namespace mdns {
namespace test {

const inet::IpPort kPort = inet::IpPort::From_uint16_t(2525);
const inet::IpPort kZeroPort = inet::IpPort::From_uint16_t(0);
constexpr char kServiceName[] = "_test._tcp.";
constexpr char kOtherServiceName[] = "_other._tcp.";
constexpr char kInstanceName[] = "testinstance";
constexpr char kHostName[] = "test2host";
constexpr char kHostFullName[] = "test2host.local.";
constexpr char kLocalHostNameLong[] = "testhostnamethatislong";
constexpr char kLocalHostFullNameLong[] = "testhostnamethatislong.local.";
constexpr char kAltHostName[] = "123456789ABC";
constexpr char kAltHostFullName[] = "123456789ABC.local.";
const std::vector<inet::IpAddress> kAddresses{inet::IpAddress(192, 168, 1, 200),
                                              inet::IpAddress(192, 168, 1, 201)};
const std::vector<inet::SocketAddress> kSocketAddresses{
    inet::SocketAddress(192, 168, 1, 200, kPort), inet::SocketAddress(192, 168, 1, 201, kPort)};
const std::vector<inet::SocketAddress> kSocketAddressesZeroPort{
    inet::SocketAddress(192, 168, 1, 200, kZeroPort),
    inet::SocketAddress(192, 168, 1, 201, kZeroPort)};
const std::vector<HostAddress> kHostAddresses{
    HostAddress(inet::IpAddress(192, 168, 1, 200), 1, zx::sec(450)),
    HostAddress(inet::IpAddress(192, 168, 1, 201), 1, zx::sec(450))};
const std::vector<HostAddress> kAlternateHostAddresses{
    HostAddress(inet::IpAddress(192, 168, 1, 202), 1, zx::sec(450)),
    HostAddress(inet::IpAddress(192, 168, 1, 203), 1, zx::sec(450))};
static constexpr size_t kMaxSenderAddresses = 64;
constexpr uint32_t kInterfaceId = 1;

class InstanceResponderTest : public AgentTest, public Mdns::Publisher {
 public:
  InstanceResponderTest() {}

 protected:
  static std::string service_full_name() { return MdnsNames::ServiceFullName(kServiceName); }

  static std::string instance_full_name() {
    return MdnsNames::InstanceFullName(kInstanceName, kServiceName);
  }

  // Expects that the agent has not called |ReportSuccess|.
  void ExpectNoReportSuccessCall() { EXPECT_FALSE(report_success_parameter_.has_value()); }

  // Expects that the agent has not called |GetPublication|.
  void ExpectNoGetPublicationCall() { EXPECT_TRUE(get_publication_calls_.empty()); }

  // Expects that the agent has called |GetPublication| with the given parameters. Returns the
  // callback passed to |GetPublication|.
  fit::function<void(std::unique_ptr<Mdns::Publication>)> ExpectGetPublicationCall(
      PublicationCause publication_cause, const std::string& subtype,
      const std::vector<inet::SocketAddress>& source_addresses);

  // Expects that nothing else has happened.
  void ExpectNoOther() override;

  // Expects a sequence of announcements made after startup.
  void ExpectAnnouncements(Media media = Media::kBoth, IpVersions ip_versions = IpVersions::kBoth,
                           const std::string& host_full_name = kLocalHostFullName,
                           const std::vector<inet::IpAddress>& addresses = {},
                           inet::IpPort port = kPort);

  // Expects a single announcement (a 'GetPublication' call and subsequent publication).
  void ExpectAnnouncement(Media media = Media::kBoth, IpVersions ip_versions = IpVersions::kBoth,
                          const std::string& host_full_name = kLocalHostFullName,
                          const std::vector<inet::IpAddress>& addresses = {},
                          inet::IpPort port = kPort);

  // Expects a single multicast publication.
  void ExpectPublication(Media media = Media::kBoth, IpVersions ip_versions = IpVersions::kBoth,
                         const std::string& host_full_name = kLocalHostFullName,
                         const std::vector<inet::IpAddress>& addresses = {},
                         inet::IpPort port = kPort);

  // Expects a single publication to the given reply address and subtype.
  void ExpectPublication(ReplyAddress reply_address, const std::string& subtype = "",
                         const std::string& host_full_name = kLocalHostFullName,
                         const std::vector<inet::IpAddress>& addresses = {},
                         inet::IpPort port = kPort,
                         MdnsResourceSection ptr_section = MdnsResourceSection::kAnswer,
                         MdnsResourceSection srv_section = MdnsResourceSection::kAdditional,
                         MdnsResourceSection txt_section = MdnsResourceSection::kAdditional);

  ReplyAddress MulticastReply(Media media, IpVersions ip_versions);

 private:
  struct GetPublicationCall {
    PublicationCause publication_cause_;
    const std::string subtype_;
    std::vector<inet::SocketAddress> source_addresses_;
    fit::function<void(std::unique_ptr<Mdns::Publication>)> callback_;
  };

  // |Mdns::Publisher| implementation.
  void ReportSuccess(bool success) override { report_success_parameter_.emplace(success); }

  void GetPublication(PublicationCause publication_cause, const std::string& subtype,
                      const std::vector<inet::SocketAddress>& source_addresses,
                      fit::function<void(std::unique_ptr<Mdns::Publication>)> callback) override {
    get_publication_calls_.push(GetPublicationCall{.publication_cause_ = publication_cause,
                                                   .subtype_ = subtype,
                                                   .source_addresses_ = source_addresses,
                                                   .callback_ = std::move(callback)});
  }

  std::optional<bool> report_success_parameter_;
  std::queue<GetPublicationCall> get_publication_calls_;
};

fit::function<void(std::unique_ptr<Mdns::Publication>)>
InstanceResponderTest::ExpectGetPublicationCall(
    PublicationCause publication_cause, const std::string& subtype,
    const std::vector<inet::SocketAddress>& source_addresses) {
  EXPECT_FALSE(get_publication_calls_.empty());
  EXPECT_EQ(publication_cause, get_publication_calls_.front().publication_cause_);
  EXPECT_EQ(subtype, get_publication_calls_.front().subtype_);
  auto callback = std::move(get_publication_calls_.front().callback_);
  EXPECT_EQ(source_addresses, get_publication_calls_.front().source_addresses_);
  EXPECT_NE(nullptr, callback);
  get_publication_calls_.pop();
  return callback;
}

void InstanceResponderTest::ExpectNoOther() {
  AgentTest::ExpectNoOther();
  ExpectNoReportSuccessCall();
  ExpectNoGetPublicationCall();
}

void InstanceResponderTest::ExpectAnnouncements(Media media, IpVersions ip_versions,
                                                const std::string& host_full_name,
                                                const std::vector<inet::IpAddress>& addresses,
                                                inet::IpPort port) {
  ExpectAnnouncement(media, ip_versions, host_full_name, addresses, port);
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));
  ExpectAnnouncement(media, ip_versions, host_full_name, addresses, port);
  ExpectPostTaskForTimeAndInvoke(zx::sec(2), zx::sec(2));
  ExpectAnnouncement(media, ip_versions, host_full_name, addresses, port);
  ExpectPostTaskForTimeAndInvoke(zx::sec(4), zx::sec(4));
  ExpectAnnouncement(media, ip_versions, host_full_name, addresses, port);
  ExpectNoOther();
}

void InstanceResponderTest::ExpectAnnouncement(Media media, IpVersions ip_versions,
                                               const std::string& host_full_name,
                                               const std::vector<inet::IpAddress>& addresses,
                                               inet::IpPort port) {
  ExpectGetPublicationCall(PublicationCause::kAnnouncement, "",
                           {})(Mdns::Publication::Create(port));
  ExpectPublication(media, ip_versions, host_full_name, addresses, port);
}

ReplyAddress InstanceResponderTest::MulticastReply(Media media, IpVersions ip_versions) {
  return ReplyAddress::Multicast(media, ip_versions);
}

void InstanceResponderTest::ExpectPublication(Media media, IpVersions ip_versions,
                                              const std::string& host_full_name,
                                              const std::vector<inet::IpAddress>& addresses,
                                              inet::IpPort port) {
  ExpectPublication(MulticastReply(media, ip_versions), "", host_full_name, addresses, port);
}

void InstanceResponderTest::ExpectPublication(ReplyAddress reply_address,
                                              const std::string& subtype,
                                              const std::string& host_full_name,
                                              const std::vector<inet::IpAddress>& addresses,
                                              inet::IpPort port, MdnsResourceSection ptr_section,
                                              MdnsResourceSection srv_section,
                                              MdnsResourceSection txt_section) {
  auto message = ExpectOutboundMessage(reply_address);

  auto resource = ExpectResource(message.get(), ptr_section, service_full_name(), DnsType::kPtr,
                                 DnsClass::kIn, false);
  EXPECT_EQ(instance_full_name(), resource->ptr_.pointer_domain_name_.dotted_string_);

  if (subtype != "") {
    resource = ExpectResource(message.get(), ptr_section, subtype + "._sub." + service_full_name(),
                              DnsType::kPtr, DnsClass::kIn, false);
    EXPECT_EQ(instance_full_name(), resource->ptr_.pointer_domain_name_.dotted_string_);
  }

  resource = ExpectResource(message.get(), srv_section, instance_full_name(), DnsType::kSrv);
  EXPECT_EQ(0, resource->srv_.priority_);
  EXPECT_EQ(0, resource->srv_.weight_);
  EXPECT_EQ(port, resource->srv_.port_);
  EXPECT_EQ(host_full_name, resource->srv_.target_.dotted_string_);

  resource = ExpectResource(message.get(), txt_section, instance_full_name(), DnsType::kTxt);
  EXPECT_TRUE(resource->txt_.strings_.empty());

  if (addresses.empty()) {
    ExpectResource(message.get(), MdnsResourceSection::kAdditional, host_full_name, DnsType::kA);
  } else {
    ExpectAddresses(message.get(), MdnsResourceSection::kAdditional, host_full_name, addresses);
  }

  ExpectNoOtherQuestionOrResource(message.get());
}

// Tests initial startup of the responder.
TEST_F(InstanceResponderTest, Startup) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();
}

// Tests that multicast sends are rate-limited.
TEST_F(InstanceResponderTest, MulticastRateLimit) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address0(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);
  ReplyAddress sender_address1(
      inet::SocketAddress(192, 168, 1, 2, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  // First question.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address0);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address0.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Second question - answer should be delayed 1s.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address0);
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address0.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTimeAndInvoke(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Third question - no delay, because 60 virtual seconds have passed.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address0);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address0.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Fourth and fifth questions - one answer, delayed 1s.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address0);
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address1);
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address0.socket_address(), sender_address1.socket_address()})(
      Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTimeAndInvoke(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that source addresses are limited to pertinent queries.
TEST_F(InstanceResponderTest, SourceAddresses) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address0(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);
  ReplyAddress sender_address1(
      inet::SocketAddress(192, 168, 1, 2, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  // Irrelevant question.
  under_test.ReceiveQuestion(
      DnsQuestion(MdnsNames::ServiceFullName(kOtherServiceName), DnsType::kPtr),
      ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth), sender_address0);

  // Pertient question.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address1);

  // Expect only pertinent sender address.
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address1.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that at most 64 source addresses are sent.
TEST_F(InstanceResponderTest, SourceAddressLimit) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  // First question.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);

  // Expect one sender address.
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  // Second question asked 65 times.
  for (size_t i = 0; i <= kMaxSenderAddresses; ++i) {
    under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                               ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                               sender_address);
  }
  ExpectPostTaskForTimeAndInvoke(zx::sec(1), zx::sec(1));

  // Expect 64 sender addresses.
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address(),
                            sender_address.socket_address(), sender_address.socket_address()})(
      Mdns::Publication::Create(kPort));
  ExpectPublication();
  ExpectPostTaskForTimeAndInvoke(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a wireless-only responder announces over wireless only and only responds to
// questions received via wireless interfaces.
TEST_F(InstanceResponderTest, WirelessOnly) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kWireless,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kWireless);

  ReplyAddress wired_sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  ReplyAddress wireless_sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWireless, IpVersions::kBoth);

  // Question from wired should be ignored.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             wired_sender_address);
  ExpectNoGetPublicationCall();
  ExpectNoOther();

  // Question from wireless should be answered.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             wireless_sender_address);
  ExpectGetPublicationCall(
      PublicationCause::kQueryMulticastResponse, "",
      {wireless_sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kWireless);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a wired-only responder announces over wired only and only responds to questions
// received via wired interfaces.
TEST_F(InstanceResponderTest, WiredOnly) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kWired,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kWired);

  ReplyAddress wireless_sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWireless, IpVersions::kBoth);

  // Question from wireless should be ignored.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             wireless_sender_address);
  ExpectNoGetPublicationCall();
  ExpectNoOther();

  ReplyAddress wired_sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  // Question from wired should be answered.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             wired_sender_address);
  ExpectGetPublicationCall(
      PublicationCause::kQueryMulticastResponse, "",
      {wired_sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kWired);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a IPv4-only responder announces over IPv4 only and only responds to
// questions received via IPv4 interfaces.
TEST_F(InstanceResponderTest, V4Only) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kV4, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kBoth, IpVersions::kV4);

  ReplyAddress v6_sender_address(inet::SocketAddress(0xfe80, 1, inet::IpPort::From_uint16_t(5353)),
                                 inet::IpAddress(0xfe80, 100), kInterfaceId, Media::kWired,
                                 IpVersions::kV6);

  ReplyAddress v4_sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWireless, IpVersions::kV4);

  // Question from IPv6 should be ignored.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             v6_sender_address);
  ExpectNoGetPublicationCall();
  ExpectNoOther();

  // Question from IPv4 should be answered.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             v4_sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {v4_sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kBoth, IpVersions::kV4);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a IPv6-only responder announces over IPv6 only and only responds to
// questions received via IPv6 interfaces.
TEST_F(InstanceResponderTest, V6Only) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kV6, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kBoth, IpVersions::kV6);

  ReplyAddress v4_sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWireless, IpVersions::kV4);

  ReplyAddress v6_sender_address(inet::SocketAddress(0xfe80, 1, inet::IpPort::From_uint16_t(5353)),
                                 inet::IpAddress(0xfe80, 100), kInterfaceId, Media::kWired,
                                 IpVersions::kV6);

  // Question from IPv4 should be ignored.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             v4_sender_address);
  ExpectNoGetPublicationCall();
  ExpectNoOther();

  // Question from IPv6 should be answered.
  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             v6_sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {v6_sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kBoth, IpVersions::kV6);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a query for a unicast response is recognized as such.
TEST_F(InstanceResponderTest, Unicast) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(inet::SocketAddress(192, 168, 1, 1, kPort),
                              inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired,
                              IpVersions::kBoth);

  auto question = DnsQuestion(service_full_name(), DnsType::kPtr);
  question.unicast_response_ = true;
  under_test.ReceiveQuestion(question, sender_address, sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryUnicastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(sender_address);
  ExpectNoOther();
}

// Tests that subtypes are properly communicated.
TEST_F(InstanceResponderTest, Subtype) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(inet::SocketAddress(192, 168, 1, 1, kPort),
                              inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired,
                              IpVersions::kBoth);

  under_test.ReceiveQuestion(DnsQuestion("_cookies._sub." + service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "_cookies",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(MulticastReply(Media::kBoth, IpVersions::kBoth), "_cookies");
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a responder for an alternate host announces and responds correctly.
TEST_F(InstanceResponderTest, HostAndAddresses) {
  InstanceResponder under_test(this, kHostName, kAddresses, kServiceName, kInstanceName,
                               Media::kBoth, IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kBoth, IpVersions::kBoth, kHostFullName, kAddresses);

  ReplyAddress sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kBoth, IpVersions::kBoth, kHostFullName, kAddresses);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that local service instance notifications are properly generated.
TEST_F(InstanceResponderTest, LocalServiceInstanceNotifications) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);
  SetLocalHostAddresses(kHostAddresses);

  // Normal startup. We use a long host name, because there was a bug that only happened when the
  // host full name was greater than sizeof(std::string), which is 24 bytes.
  under_test.Start(kLocalHostFullNameLong);
  ExpectAnnouncements(Media::kBoth, IpVersions::kBoth, kLocalHostFullNameLong);

  ReplyAddress sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kBoth, IpVersions::kBoth, kLocalHostFullNameLong);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  ExpectAddLocalServiceInstanceCall(
      ServiceInstance(kServiceName, kInstanceName, kLocalHostNameLong, kSocketAddresses), false);
}

// Tests that local service instance notifications are properly generated for proxy instances.
TEST_F(InstanceResponderTest, LocalServiceInstanceNotificationsProxy) {
  InstanceResponder under_test(this, kHostName, kAddresses, kServiceName, kInstanceName,
                               Media::kBoth, IpVersions::kBoth, this);
  SetAgent(under_test);
  // Use alternate host addresses to test for regression of fxb/105871 fix.
  SetLocalHostAddresses(kAlternateHostAddresses);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kBoth, IpVersions::kBoth, kHostFullName, kAddresses);

  ReplyAddress sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(Media::kBoth, IpVersions::kBoth, kHostFullName, kAddresses);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  ExpectAddLocalServiceInstanceCall(
      ServiceInstance(kServiceName, kInstanceName, kHostName, kSocketAddresses), true);
}

// Tests that local service instance notifications are properly generated when the port is zero.
TEST_F(InstanceResponderTest, LocalServiceInstanceNotificationsZeroPort) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);
  SetLocalHostAddresses(kHostAddresses);

  // Normal startup. We use a long host name, because there was a bug that only happened when the
  // host full name was greater than sizeof(std::string), which is 24 bytes.
  under_test.Start(kLocalHostFullNameLong);
  ExpectAnnouncements(Media::kBoth, IpVersions::kBoth, kLocalHostFullNameLong, {}, kZeroPort);

  ReplyAddress sender_address(
      inet::SocketAddress(192, 168, 1, 1, inet::IpPort::From_uint16_t(5353)),
      inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired, IpVersions::kBoth);

  under_test.ReceiveQuestion(DnsQuestion(service_full_name(), DnsType::kPtr),
                             ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kZeroPort));
  ExpectPublication(Media::kBoth, IpVersions::kBoth, kLocalHostFullNameLong, {}, kZeroPort);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();

  ExpectAddLocalServiceInstanceCall(
      ServiceInstance(kServiceName, kInstanceName, kLocalHostNameLong, kSocketAddressesZeroPort),
      false);
}

// Tests the the responder works with an alternate host name.
TEST_F(InstanceResponderTest, AltHostName) {
  InstanceResponder under_test(this, kAltHostName, {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements(Media::kBoth, IpVersions::kBoth, kAltHostFullName);
}

// Tests that a query for SRV elicits the correct response.
TEST_F(InstanceResponderTest, SrvQuestion) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(inet::SocketAddress(192, 168, 1, 1, kPort),
                              inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired,
                              IpVersions::kBoth);

  auto question = DnsQuestion(instance_full_name(), DnsType::kSrv);
  under_test.ReceiveQuestion(question, ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(MulticastReply(Media::kBoth, IpVersions::kBoth), "", kLocalHostFullName, {},
                    kPort, MdnsResourceSection::kAdditional, MdnsResourceSection::kAnswer,
                    MdnsResourceSection::kAdditional);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a query for TXT elicits the correct response.
TEST_F(InstanceResponderTest, TxtQuestion) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(inet::SocketAddress(192, 168, 1, 1, kPort),
                              inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired,
                              IpVersions::kBoth);

  auto question = DnsQuestion(instance_full_name(), DnsType::kTxt);
  under_test.ReceiveQuestion(question, ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(MulticastReply(Media::kBoth, IpVersions::kBoth), "", kLocalHostFullName, {},
                    kPort, MdnsResourceSection::kAdditional, MdnsResourceSection::kAdditional,
                    MdnsResourceSection::kAnswer);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a query for ANY for the instance elicits the correct response.
TEST_F(InstanceResponderTest, AnyForInstanceQuestion) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(inet::SocketAddress(192, 168, 1, 1, kPort),
                              inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired,
                              IpVersions::kBoth);

  auto question = DnsQuestion(instance_full_name(), DnsType::kAny);
  under_test.ReceiveQuestion(question, ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(MulticastReply(Media::kBoth, IpVersions::kBoth), "", kLocalHostFullName, {},
                    kPort, MdnsResourceSection::kAdditional, MdnsResourceSection::kAnswer,
                    MdnsResourceSection::kAnswer);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

// Tests that a query for ANY for the service type elicits the correct response.
TEST_F(InstanceResponderTest, AnyForServiceTypeQuestion) {
  InstanceResponder under_test(this, "", {}, kServiceName, kInstanceName, Media::kBoth,
                               IpVersions::kBoth, this);
  SetAgent(under_test);

  // Normal startup.
  under_test.Start(kLocalHostFullName);
  ExpectAnnouncements();

  ReplyAddress sender_address(inet::SocketAddress(192, 168, 1, 1, kPort),
                              inet::IpAddress(192, 168, 1, 100), kInterfaceId, Media::kWired,
                              IpVersions::kBoth);

  auto question = DnsQuestion(service_full_name(), DnsType::kAny);
  under_test.ReceiveQuestion(question, ReplyAddress::Multicast(Media::kBoth, IpVersions::kBoth),
                             sender_address);
  ExpectGetPublicationCall(PublicationCause::kQueryMulticastResponse, "",
                           {sender_address.socket_address()})(Mdns::Publication::Create(kPort));
  ExpectPublication(MulticastReply(Media::kBoth, IpVersions::kBoth), "", kLocalHostFullName, {},
                    kPort, MdnsResourceSection::kAnswer, MdnsResourceSection::kAdditional,
                    MdnsResourceSection::kAdditional);
  ExpectPostTaskForTime(zx::sec(60), zx::sec(60));  // idle cleanup
  ExpectNoOther();
}

}  // namespace test
}  // namespace mdns
