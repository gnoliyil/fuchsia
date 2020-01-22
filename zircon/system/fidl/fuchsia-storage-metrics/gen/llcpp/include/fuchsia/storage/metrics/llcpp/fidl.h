// WARNING: This file is machine generated by fidlgen.

#pragma once

#include <lib/fidl/internal.h>
#include <lib/fidl/txn_header.h>
#include <lib/fidl/llcpp/array.h>
#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/connect_service.h>
#include <lib/fidl/llcpp/service_handler_interface.h>
#include <lib/fidl/llcpp/string_view.h>
#include <lib/fidl/llcpp/sync_call.h>
#include <lib/fidl/llcpp/traits.h>
#include <lib/fidl/llcpp/transaction.h>
#include <lib/fidl/llcpp/vector_view.h>
#include <lib/fit/function.h>
#include <zircon/fidl.h>

namespace llcpp {

namespace fuchsia {
namespace storage {
namespace metrics {

struct CallStatRaw;
struct CallStat;
struct FsMetrics;

extern "C" const fidl_type_t v1_fuchsia_storage_metrics_CallStatRawTable;

struct CallStatRaw {
  static constexpr const fidl_type_t* Type = &v1_fuchsia_storage_metrics_CallStatRawTable;
  static constexpr uint32_t MaxNumHandles = 0;
  static constexpr uint32_t PrimarySize = 40;
  [[maybe_unused]]
  static constexpr uint32_t MaxOutOfLine = 0;
  static constexpr bool HasPointer = false;

  uint64_t minimum_latency = {};

  uint64_t maximum_latency = {};

  uint64_t total_time_spent = {};

  uint64_t total_calls = {};

  uint64_t bytes_transferred = {};
};

extern "C" const fidl_type_t v1_fuchsia_storage_metrics_CallStatTable;

struct CallStat {
  static constexpr const fidl_type_t* Type = &v1_fuchsia_storage_metrics_CallStatTable;
  static constexpr uint32_t MaxNumHandles = 0;
  static constexpr uint32_t PrimarySize = 80;
  [[maybe_unused]]
  static constexpr uint32_t MaxOutOfLine = 0;
  static constexpr bool HasPointer = false;

  ::llcpp::fuchsia::storage::metrics::CallStatRaw success = {};

  ::llcpp::fuchsia::storage::metrics::CallStatRaw failure = {};
};

extern "C" const fidl_type_t v1_fuchsia_storage_metrics_FsMetricsTable;

struct FsMetrics {
  static constexpr const fidl_type_t* Type = &v1_fuchsia_storage_metrics_FsMetricsTable;
  static constexpr uint32_t MaxNumHandles = 0;
  static constexpr uint32_t PrimarySize = 640;
  [[maybe_unused]]
  static constexpr uint32_t MaxOutOfLine = 0;
  static constexpr bool HasPointer = false;

  ::llcpp::fuchsia::storage::metrics::CallStat create = {};

  ::llcpp::fuchsia::storage::metrics::CallStat read = {};

  ::llcpp::fuchsia::storage::metrics::CallStat write = {};

  ::llcpp::fuchsia::storage::metrics::CallStat truncate = {};

  ::llcpp::fuchsia::storage::metrics::CallStat unlink = {};

  ::llcpp::fuchsia::storage::metrics::CallStat rename = {};

  ::llcpp::fuchsia::storage::metrics::CallStat lookup = {};

  ::llcpp::fuchsia::storage::metrics::CallStat open = {};
};

}  // namespace metrics
}  // namespace storage
}  // namespace fuchsia
}  // namespace llcpp

namespace fidl {

template <>
struct IsFidlType<::llcpp::fuchsia::storage::metrics::CallStatRaw> : public std::true_type {};
static_assert(std::is_standard_layout_v<::llcpp::fuchsia::storage::metrics::CallStatRaw>);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStatRaw, minimum_latency) == 0);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStatRaw, maximum_latency) == 8);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStatRaw, total_time_spent) == 16);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStatRaw, total_calls) == 24);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStatRaw, bytes_transferred) == 32);
static_assert(sizeof(::llcpp::fuchsia::storage::metrics::CallStatRaw) == ::llcpp::fuchsia::storage::metrics::CallStatRaw::PrimarySize);

template <>
struct IsFidlType<::llcpp::fuchsia::storage::metrics::CallStat> : public std::true_type {};
static_assert(std::is_standard_layout_v<::llcpp::fuchsia::storage::metrics::CallStat>);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStat, success) == 0);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::CallStat, failure) == 40);
static_assert(sizeof(::llcpp::fuchsia::storage::metrics::CallStat) == ::llcpp::fuchsia::storage::metrics::CallStat::PrimarySize);

template <>
struct IsFidlType<::llcpp::fuchsia::storage::metrics::FsMetrics> : public std::true_type {};
static_assert(std::is_standard_layout_v<::llcpp::fuchsia::storage::metrics::FsMetrics>);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, create) == 0);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, read) == 80);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, write) == 160);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, truncate) == 240);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, unlink) == 320);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, rename) == 400);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, lookup) == 480);
static_assert(offsetof(::llcpp::fuchsia::storage::metrics::FsMetrics, open) == 560);
static_assert(sizeof(::llcpp::fuchsia::storage::metrics::FsMetrics) == ::llcpp::fuchsia::storage::metrics::FsMetrics::PrimarySize);

}  // namespace fidl
