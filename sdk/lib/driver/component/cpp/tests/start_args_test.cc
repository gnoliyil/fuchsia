// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/driver/component/cpp/internal/start_args.h>
#include <lib/driver/component/cpp/internal/symbols.h>
#include <lib/zx/eventpair.h>

#include <array>

#include <gtest/gtest.h>

#include "src/lib/testing/predicates/status.h"

namespace fdf {
using namespace fuchsia_driver_framework;
}  // namespace fdf

namespace fdata = fuchsia_data;
namespace frunner = fuchsia_component_runner;

TEST(StartArgsTest, SymbolValueWire) {
  fidl::Arena arena;
  fidl::VectorView<fdf::wire::NodeSymbol> symbol_entries(arena, 1);
  symbol_entries[0].Allocate(arena);
  symbol_entries[0].set_name(arena, "sym").set_address(arena, 0xfeed);
  fdf::wire::DriverStartArgs args(arena);
  args.set_symbols(arena, symbol_entries);

  EXPECT_EQ(0xfeedu, *fdf_internal::SymbolValue<zx_vaddr_t>(args, "sym"));
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::SymbolValue<zx_vaddr_t>(args, "unknown").error_value());
}

TEST(StartArgsTest, SymbolValue) {
  std::vector<fdf::NodeSymbol> symbol_entries(1);
  symbol_entries[0] = fdf::NodeSymbol({
      .name = "sym",
      .address = 0xfeed,
  });

  EXPECT_EQ(0xfeedu, *fdf_internal::SymbolValue<zx_vaddr_t>(symbol_entries, "sym"));
  EXPECT_EQ(ZX_ERR_NOT_FOUND,
            fdf_internal::SymbolValue<zx_vaddr_t>(symbol_entries, "unknown").error_value());
}

TEST(StartArgsTest, ProgramValueWire) {
  fidl::Arena arena;
  fidl::VectorView<fdata::wire::DictionaryEntry> program_entries(arena, 2);
  program_entries[0].key.Set(arena, "key-for-str");
  program_entries[0].value = fdata::wire::DictionaryValue::WithStr(arena, "value-for-str");
  program_entries[1].key.Set(arena, "key-for-strvec");
  program_entries[1].value = fdata::wire::DictionaryValue::WithStrVec(arena);
  fdata::wire::Dictionary program(arena);
  program.set_entries(arena, std::move(program_entries));

  EXPECT_EQ("value-for-str", *fdf_internal::ProgramValue(program, "key-for-str"));
  EXPECT_EQ(ZX_ERR_WRONG_TYPE, fdf_internal::ProgramValue(program, "key-for-strvec").error_value());
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::ProgramValue(program, "key-unkown").error_value());

  fdata::wire::Dictionary empty_program;
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::ProgramValue(empty_program, "").error_value());
}

TEST(StartArgsTest, ProgramValue) {
  std::vector<fdata::DictionaryEntry> program_entries(2);
  program_entries[0] = fdata::DictionaryEntry(
      "key-for-str", std::make_unique<fuchsia_data::DictionaryValue>(
                         fuchsia_data::DictionaryValue::WithStr("value-for-str")));

  program_entries[1] =
      fdata::DictionaryEntry("key-for-strvec", std::make_unique<fuchsia_data::DictionaryValue>(
                                                   fuchsia_data::DictionaryValue::WithStrVec({})));

  auto program = fdata::Dictionary({
      .entries = std::move(program_entries),
  });

  EXPECT_EQ("value-for-str", *fdf_internal::ProgramValue(program, "key-for-str"));
  EXPECT_EQ(ZX_ERR_WRONG_TYPE, fdf_internal::ProgramValue(program, "key-for-strvec").error_value());
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::ProgramValue(program, "key-unkown").error_value());

  fdata::Dictionary empty_program;
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::ProgramValue(empty_program, "").error_value());
}

TEST(StartArgsTest, ProgramValueAsVectorWire) {
  fidl::Arena arena;
  fidl::VectorView<fdata::wire::DictionaryEntry> program_entries(arena, 2);
  program_entries[0].key.Set(arena, "key-for-str");
  program_entries[0].value = fdata::wire::DictionaryValue::WithStr(arena, "value-for-str");

  fidl::StringView strs[]{
      fidl::StringView{"test"},
      fidl::StringView{"test2"},
  };
  program_entries[1].key.Set(arena, "key-for-strvec");
  program_entries[1].value = fdata::wire::DictionaryValue::WithStrVec(
      arena, fidl::VectorView<fidl::StringView>::FromExternal(strs));

  fdata::wire::Dictionary program(arena);
  program.set_entries(arena, std::move(program_entries));

  auto values = fdf_internal::ProgramValueAsVector(program, "key-for-strvec");
  EXPECT_EQ(2lu, values->size());
  std::sort(values->begin(), values->end());
  EXPECT_EQ("test", (*values)[0]);
  EXPECT_EQ("test2", (*values)[1]);

  EXPECT_EQ(ZX_ERR_WRONG_TYPE,
            fdf_internal::ProgramValueAsVector(program, "key-for-str").error_value());
  EXPECT_EQ(ZX_ERR_NOT_FOUND,
            fdf_internal::ProgramValueAsVector(program, "key-unkown").error_value());

  fdata::wire::Dictionary empty_program;
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::ProgramValueAsVector(empty_program, "").error_value());
}

TEST(StartArgsTest, ProgramValueAsVector) {
  std::vector<fdata::DictionaryEntry> program_entries(2);
  program_entries[0] = fdata::DictionaryEntry(
      "key-for-str", std::make_unique<fuchsia_data::DictionaryValue>(
                         fuchsia_data::DictionaryValue::WithStr("value-for-str")));

  program_entries[1] = fdata::DictionaryEntry(
      "key-for-strvec",
      std::make_unique<fuchsia_data::DictionaryValue>(fuchsia_data::DictionaryValue::WithStrVec({
          "test",
          "test2",
      })));

  fdata::Dictionary program({
      .entries = std::move(program_entries),
  });

  auto values = fdf_internal::ProgramValueAsVector(program, "key-for-strvec");
  EXPECT_EQ(2lu, values->size());
  std::sort(values->begin(), values->end());
  EXPECT_EQ("test", (*values)[0]);
  EXPECT_EQ("test2", (*values)[1]);

  EXPECT_EQ(ZX_ERR_WRONG_TYPE,
            fdf_internal::ProgramValueAsVector(program, "key-for-str").error_value());
  EXPECT_EQ(ZX_ERR_NOT_FOUND,
            fdf_internal::ProgramValueAsVector(program, "key-unkown").error_value());

  fdata::Dictionary empty_program;
  EXPECT_EQ(ZX_ERR_NOT_FOUND, fdf_internal::ProgramValueAsVector(empty_program, "").error_value());
}

TEST(StartArgsTest, NsValueWire) {
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_EQ(ZX_OK, endpoints.status_value());
  zx_handle_t client_handle = endpoints->client.handle()->get();
  fidl::Arena arena;
  fidl::VectorView<frunner::wire::ComponentNamespaceEntry> ns_entries(arena, 1);
  ns_entries[0].Allocate(arena);
  ns_entries[0].set_path(arena, "/svc").set_directory(std::move(endpoints->client));

  auto svc = fdf_internal::NsValue(ns_entries, "/svc");
  ASSERT_EQ(svc->handle()->get(), client_handle);

  auto pkg = fdf_internal::NsValue(ns_entries, "/pkg");
  EXPECT_EQ(ZX_ERR_NOT_FOUND, pkg.error_value());
}

TEST(StartArgsTest, NsValue) {
  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_EQ(ZX_OK, endpoints.status_value());
  zx_handle_t client_handle = endpoints->client.handle()->get();
  std::vector<frunner::ComponentNamespaceEntry> ns_entries(1);
  ns_entries[0] = frunner::ComponentNamespaceEntry({
      .path = "/svc",
      .directory = std::move(endpoints->client),
  });

  auto svc = fdf_internal::NsValue(ns_entries, "/svc");
  ASSERT_EQ(svc->handle()->get(), client_handle);

  auto pkg = fdf_internal::NsValue(ns_entries, "/pkg");
  EXPECT_EQ(ZX_ERR_NOT_FOUND, pkg.error_value());
}

TEST(StartArgsTest, AdoptEncodedFidlMessage) {
  std::array<uint8_t, 16> bytes;
  bytes.fill(1);

  zx::eventpair ep1, ep2;
  ASSERT_OK(zx::eventpair::create(0, &ep1, &ep2));
  zx_handle_t handles[] = {
      ep1.get(),
  };

  EncodedFidlMessage msg{
      .bytes = bytes.data(),
      .handles = handles,
      .num_bytes = static_cast<uint32_t>(bytes.size()),
      .num_handles = static_cast<uint32_t>(std::size(handles)),
  };

  // Handles should be closed when the message goes out of scope.
  { fdf_internal::AdoptEncodedFidlMessage adopted{msg}; }

  zx_signals_t observed = ZX_SIGNAL_NONE;
  ASSERT_OK(ep2.wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::time::infinite_past(), &observed));
  EXPECT_EQ(observed, ZX_EVENTPAIR_PEER_CLOSED);
  [[maybe_unused]] zx_handle_t h = ep1.release();
}

TEST(StartArgsTest, AdoptEncodedFidlMessageToFidl) {
  std::array<uint8_t, 16> bytes;
  bytes.fill(1);

  zx::eventpair ep1, ep2;
  ASSERT_OK(zx::eventpair::create(0, &ep1, &ep2));
  zx_handle_t handles[] = {
      ep1.get(),
  };

  EncodedFidlMessage msg{
      .bytes = bytes.data(),
      .handles = handles,
      .num_bytes = static_cast<uint32_t>(bytes.size()),
      .num_handles = static_cast<uint32_t>(std::size(handles)),
  };

  fdf_internal::AdoptEncodedFidlMessage adopted{msg};
  // Handles should be closed when the message goes out of scope.
  { fidl::EncodedMessage encoded = adopted.TakeMessage(); }

  zx_signals_t observed = ZX_SIGNAL_NONE;
  ASSERT_OK(ep2.wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::time::infinite_past(), &observed));
  EXPECT_EQ(observed, ZX_EVENTPAIR_PEER_CLOSED);
  [[maybe_unused]] zx_handle_t h = ep1.release();
}
