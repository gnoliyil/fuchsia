// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/unwinder/registers.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace unwinder {

namespace {

RegisterID GetPcReg(Registers::Arch arch) {
  switch (arch) {
    case Registers::Arch::kX64:
      return RegisterID::kX64_rip;
    case Registers::Arch::kArm64:
      return RegisterID::kArm64_pc;
    case Registers::Arch::kRiscv64:
      return RegisterID::kRiscv64_pc;
  }
}

RegisterID GetSpReg(Registers::Arch arch) {
  switch (arch) {
    case Registers::Arch::kX64:
      return RegisterID::kX64_rsp;
    case Registers::Arch::kArm64:
      return RegisterID::kArm64_sp;
    case Registers::Arch::kRiscv64:
      return RegisterID::kRiscv64_sp;
  }
}

}  // namespace

Error Registers::Get(RegisterID reg_id, uint64_t& val) const {
  auto it = regs_.find(reg_id);
  if (it == regs_.end()) {
    return Error("register %s is undefined", GetRegName(reg_id));
  }
  val = it->second;
  return Success();
}

Error Registers::Set(RegisterID reg_id, uint64_t val) {
  regs_[reg_id] = val;
  return Success();
}

Error Registers::Unset(RegisterID reg_id) {
  regs_.erase(reg_id);
  return Success();
}

Error Registers::GetSP(uint64_t& sp) const { return Get(GetSpReg(arch_), sp); }

Error Registers::SetSP(uint64_t sp) { return Set(GetSpReg(arch_), sp); }

Error Registers::GetPC(uint64_t& pc) const { return Get(GetPcReg(arch_), pc); }

Error Registers::SetPC(uint64_t pc) { return Set(GetPcReg(arch_), pc); }

std::string Registers::Describe() const {
  std::vector<RegisterID> keys;
  keys.reserve(regs_.size());
  for (const auto& [id, val] : regs_) {
    keys.push_back(id);
  }
  std::sort(keys.begin(), keys.end());

  std::stringstream ss;
  for (auto id : keys) {
    ss << GetRegName(id) << "=0x" << std::hex << regs_.at(id) << " ";
  }

  std::string s = std::move(ss).str();
  // Remove the last space
  if (!s.empty()) {
    s.pop_back();
  }
  return s;
}

const char* Registers::GetRegName(RegisterID reg_id) const {
  static const char* x64_names[] = {
      "rax", "rdx", "rcx", "rbx", "rsi", "rdi", "rbp", "rsp", "r8",
      "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip",
  };
  static const char* arm64_names[] = {
      "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
      "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
      "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "lr",  "sp",  "pc",
  };
  static const char* riscv64_names[] = {
      "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
      "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
      "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
  };

  const char** names;
  size_t length;
  switch (arch_) {
    case Arch::kX64:
      names = x64_names;
      length = sizeof(x64_names) / sizeof(char*);
      break;
    case Arch::kArm64:
      names = arm64_names;
      length = sizeof(arm64_names) / sizeof(char*);
      break;
    case Arch::kRiscv64:
      if (reg_id == RegisterID::kRiscv64_pc) {
        return "pc";
      }
      names = riscv64_names;
      length = sizeof(riscv64_names) / sizeof(char*);
      break;
  }

  if (static_cast<size_t>(reg_id) < length) {
    return names[static_cast<size_t>(reg_id)];
  }

  return "<unknown>";
}

}  // namespace unwinder
