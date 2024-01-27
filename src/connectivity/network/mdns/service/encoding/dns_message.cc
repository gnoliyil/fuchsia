// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/network/mdns/service/encoding/dns_message.h"

namespace mdns {

void DnsHeader::SetResponse(bool value) {
  if (value) {
    flags_ |= kQueryResponseMask;
  } else {
    flags_ &= ~kQueryResponseMask;
  }
}

void DnsHeader::SetOpCode(DnsOpCode op_code) {
  flags_ &= ~kOpCodeMask;
  flags_ |= static_cast<uint16_t>(op_code) << kOpCodeShift;
}

void DnsHeader::SetAuthoritativeAnswer(bool value) {
  if (value) {
    flags_ |= kAuthoritativeAnswerMask;
  } else {
    flags_ &= ~kAuthoritativeAnswerMask;
  }
}

void DnsHeader::SetTruncated(bool value) {
  if (value) {
    flags_ |= kTruncationMask;
  } else {
    flags_ &= ~kTruncationMask;
  }
}

void DnsHeader::SetRecursionDesired(bool value) {
  if (value) {
    flags_ |= kRecursionDesiredMask;
  } else {
    flags_ &= ~kRecursionDesiredMask;
  }
}

void DnsHeader::SetRecursionAvailable(bool value) {
  if (value) {
    flags_ |= kRecursionAvailableMask;
  } else {
    flags_ &= ~kRecursionAvailableMask;
  }
}

void DnsHeader::SetResponseCode(DnsResponseCode response_code) {
  flags_ &= ~kResponseCodeMask;
  flags_ |= static_cast<uint16_t>(response_code);
}

DnsQuestion::DnsQuestion() {}

DnsQuestion::DnsQuestion(const std::string& name, DnsType type)
    : name_(DnsName(name)), type_(type) {}

DnsResource::DnsResource() {}

DnsResource::DnsResource(const std::string& name, DnsType type)
    : name_(DnsName(name)), type_(type) {
  switch (type_) {
    case DnsType::kA:
      new (&a_) DnsResourceDataA();
      time_to_live_ = kShortTimeToLive;
      break;
    case DnsType::kNs:
      new (&ns_) DnsResourceDataNs();
      time_to_live_ = kLongTimeToLive;
      break;
    case DnsType::kCName:
      new (&cname_) DnsResourceDataCName();
      time_to_live_ = kLongTimeToLive;
      break;
    case DnsType::kPtr:
      new (&ptr_) DnsResourceDataPtr();
      time_to_live_ = kLongTimeToLive;
      break;
    case DnsType::kTxt:
      new (&txt_) DnsResourceDataTxt();
      time_to_live_ = kLongTimeToLive;
      break;
    case DnsType::kAaaa:
      new (&aaaa_) DnsResourceDataAaaa();
      time_to_live_ = kShortTimeToLive;
      break;
    case DnsType::kSrv:
      new (&srv_) DnsResourceDataSrv();
      time_to_live_ = kShortTimeToLive;
      break;
    case DnsType::kOpt:
      new (&opt_) DnsResourceDataOpt();
      time_to_live_ = kShortTimeToLive;
      break;
    case DnsType::kNSec:
      new (&nsec_) DnsResourceDataNSec();
      time_to_live_ = kLongTimeToLive;
      break;
    default:
      break;
  }
}

DnsResource::DnsResource(const std::string& name, inet::IpAddress address) : name_(DnsName(name)) {
  if (address.is_v4()) {
    type_ = DnsType::kA;
    new (&a_) DnsResourceDataA();
    time_to_live_ = kShortTimeToLive;
    a_.address_.address_ = address;
  } else {
    type_ = DnsType::kAaaa;
    new (&aaaa_) DnsResourceDataAaaa();
    time_to_live_ = kShortTimeToLive;
    aaaa_.address_.address_ = address;
  }
}

DnsResource::DnsResource(const DnsResource& other) {
  name_ = other.name_;
  type_ = other.type_;
  class_ = other.class_;
  cache_flush_ = other.cache_flush_;
  time_to_live_ = other.time_to_live_;

  switch (type_) {
    case DnsType::kA:
      new (&a_) DnsResourceDataA();
      a_ = other.a_;
      break;
    case DnsType::kNs:
      new (&ns_) DnsResourceDataNs();
      ns_ = other.ns_;
      break;
    case DnsType::kCName:
      new (&cname_) DnsResourceDataCName();
      cname_ = other.cname_;
      break;
    case DnsType::kPtr:
      new (&ptr_) DnsResourceDataPtr();
      ptr_ = other.ptr_;
      break;
    case DnsType::kTxt:
      new (&txt_) DnsResourceDataTxt();
      txt_ = other.txt_;
      break;
    case DnsType::kAaaa:
      new (&aaaa_) DnsResourceDataAaaa();
      aaaa_ = other.aaaa_;
      break;
    case DnsType::kSrv:
      new (&srv_) DnsResourceDataSrv();
      srv_ = other.srv_;
      break;
    case DnsType::kOpt:
      new (&opt_) DnsResourceDataOpt();
      opt_ = other.opt_;
      break;
    case DnsType::kNSec:
      new (&nsec_) DnsResourceDataNSec();
      nsec_ = other.nsec_;
      break;
    default:
      break;
  }
}

DnsResource& DnsResource::operator=(const DnsResource& other) {
  name_ = other.name_;
  type_ = other.type_;
  class_ = other.class_;
  cache_flush_ = other.cache_flush_;
  time_to_live_ = other.time_to_live_;

  switch (type_) {
    case DnsType::kA:
      new (&a_) DnsResourceDataA();
      a_ = other.a_;
      break;
    case DnsType::kNs:
      new (&ns_) DnsResourceDataNs();
      ns_ = other.ns_;
      break;
    case DnsType::kCName:
      new (&cname_) DnsResourceDataCName();
      cname_ = other.cname_;
      break;
    case DnsType::kPtr:
      new (&ptr_) DnsResourceDataPtr();
      ptr_ = other.ptr_;
      break;
    case DnsType::kTxt:
      new (&txt_) DnsResourceDataTxt();
      txt_ = other.txt_;
      break;
    case DnsType::kAaaa:
      new (&aaaa_) DnsResourceDataAaaa();
      aaaa_ = other.aaaa_;
      break;
    case DnsType::kSrv:
      new (&srv_) DnsResourceDataSrv();
      srv_ = other.srv_;
      break;
    case DnsType::kOpt:
      new (&opt_) DnsResourceDataOpt();
      opt_ = other.opt_;
      break;
    case DnsType::kNSec:
      new (&nsec_) DnsResourceDataNSec();
      nsec_ = other.nsec_;
      break;
    default:
      break;
  }

  return *this;
}

DnsResource::~DnsResource() {
  switch (type_) {
    case DnsType::kA:
      a_.~DnsResourceDataA();
      break;
    case DnsType::kNs:
      ns_.~DnsResourceDataNs();
      break;
    case DnsType::kCName:
      cname_.~DnsResourceDataCName();
      break;
    case DnsType::kPtr:
      ptr_.~DnsResourceDataPtr();
      break;
    case DnsType::kTxt:
      txt_.~DnsResourceDataTxt();
      break;
    case DnsType::kAaaa:
      aaaa_.~DnsResourceDataAaaa();
      break;
    case DnsType::kSrv:
      srv_.~DnsResourceDataSrv();
      break;
    case DnsType::kOpt:
      opt_.~DnsResourceDataOpt();
      break;
    case DnsType::kNSec:
      nsec_.~DnsResourceDataNSec();
      break;
    default:
      break;
  }
}

}  // namespace mdns
