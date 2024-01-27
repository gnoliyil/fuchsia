// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_INPUT_REPORT_READER_READER_H_
#define LIB_INPUT_REPORT_READER_READER_H_

#include <fidl/fuchsia.input.report/cpp/wire.h>
#include <lib/trace/event.h>
#include <zircon/compiler.h>

#include <array>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>

namespace input_report_reader {

using ReadInputReportsCompleterBase =
    fidl::internal::WireCompleterBase<fuchsia_input_report::InputReportsReader::ReadInputReports>;

// InputReportReaderManager is used to simplify implementation of input drivers. An input driver may
// use InputReportReaderManager to keep track of all upstream readers that want to receive reports.
// An upstream driver that wants to read input reports from this device may register with
// InputReportReaderManager, which calls CreateReader. When an input report arrives, whether in the
// form of HID reports or device readings by polling, etc., the report is pushed to all readers
// registered by calling SendReportToAllReaders where it is then translated to
// fuchsia_input_report::InputReport.
//
// This class creates and manages the InputReportReaders. It is able to send reports
// to all existing InputReportReaders.
// When this class is destructed, all of the InputReportReaders will be freed.
// This class is thread-safe.
// Typical Usage:
// An InputReport Driver should have one InputReportReaderManager member object.
// The Driver should also have some form of InputReport object that can be converted to Fidl.
//
// Eg:
//
// class MyTouchScreenDriver {
// ...
// private:
//   struct TouchScreenReport {
//      int64_t x;
//      int64_t y;
//      void ToFidlInputReport(fidl::WireTableBuilder<::fuchsia_input_report::wire::InputReport>&
//                             input_report, fidl::AnyArena& allocator);
//   };
//
//   InputReportReaderManager<TouchScreenReport> input_report_readers_;
// };
//
// See
// https://fuchsia.dev/fuchsia-src/development/drivers/concepts/driver_architectures/input_drivers/input?hl=en
template <class Report>
class InputReportReaderManager final {
 private:
  class InputReportReader;

 public:
  InputReportReaderManager() = default;
  // This object can't be moved, because InputReportReaders point to this object.
  InputReportReaderManager(const InputReportReaderManager&) = delete;
  InputReportReaderManager(InputReportReaderManager&&) = delete;
  InputReportReaderManager& operator=(const InputReportReaderManager&) = delete;
  InputReportReaderManager& operator=(InputReportReaderManager&&) = delete;

  // Create a new InputReportReader that is managed by this InputReportReaderManager.
  zx_status_t CreateReader(async_dispatcher_t* dispatcher,
                           fidl::ServerEnd<fuchsia_input_report::InputReportsReader> server) {
    ZX_ASSERT(dispatcher);
    std::scoped_lock lock(lock_);
    auto reader = InputReportReader::Create(this, next_reader_id_, dispatcher, std::move(server));
    if (!reader) {
      return ZX_ERR_INTERNAL;
    }
    next_reader_id_++;
    readers_list_.push_back(std::move(reader));
    return ZX_OK;
  }

  // Send a report to all InputReportReaders.
  void SendReportToAllReaders(const Report& report) {
    std::scoped_lock lock(lock_);
    for (auto& reader : readers_list_) {
      reader->ReceiveReport(report);
    }
  }

  // Remove a given reader from the list. This is called by the InputReportReader itself
  // when it wishes to be removed.
  void RemoveReaderFromList(InputReportReader* reader) {
    std::scoped_lock lock(lock_);
    for (auto iter = readers_list_.begin(); iter != readers_list_.end(); ++iter) {
      if (iter->get() == reader) {
        readers_list_.erase(iter);
        break;
      }
    }
  }

 private:
  // Assert that our template type `Report` has the following function:
  //      void ToFidlInputReport(fidl::WireTableBuilder<::fuchsia_input_report::wire::InputReport>&
  //                             input_report, fidl::AnyArena& allocator);
  template <typename T>
  struct has_to_fidl_input_report {
   private:
    template <typename C>
    static std::true_type test(
        decltype(static_cast<void (C::*)(
                     fidl::WireTableBuilder<fuchsia_input_report::wire::InputReport>& input_report,
                     fidl::AnyArena& allocator)>(&C::ToFidlInputReport)));
    template <typename C>
    static std::false_type test(...);

   public:
    static constexpr bool value = decltype(test<T>(nullptr))::value;
  };
  static_assert(
      has_to_fidl_input_report<Report>::value,
      "Report must implement void "
      "ToFidlInputReport(fidl::WireTableBuilder<::fuchsia_input_report::wire::InputReport>& "
      "input_report, fidl::AnyArena& allocator);");

  std::mutex lock_;
  size_t next_reader_id_ __TA_GUARDED(lock_) = 1;
  std::list<std::shared_ptr<InputReportReader>> readers_list_ __TA_GUARDED(lock_);
};

// This class represents an InputReportReader that sends InputReports out to a specific client.
// This class is thread safe.
// Typical usage:
//  This class shouldn't be touched directly. An InputReport driver should only manipulate
//  the InputReportReaderManager.
template <class Report>
class InputReportReaderManager<Report>::InputReportReader final
    : public fidl::WireServer<fuchsia_input_report::InputReportsReader> {
 public:
  // Create the InputReportReader. `manager` and `dispatcher` must outlive this InputReportReader.
  static std::shared_ptr<InputReportReader> Create(
      InputReportReaderManager<Report>* manager, size_t reader_id, async_dispatcher_t* dispatcher,
      fidl::ServerEnd<fuchsia_input_report::InputReportsReader> server);

  // This is only public to make std::unique_ptr work.
  explicit InputReportReader(InputReportReaderManager<Report>* manager, size_t reader_id)
      : reader_id_(reader_id), manager_(manager) {}

  void ReceiveReport(const Report& report) __TA_EXCLUDES(&report_lock_);

  void ReadInputReports(ReadInputReportsCompleter::Sync& completer)
      __TA_EXCLUDES(&report_lock_) override;

 private:
  static constexpr size_t kInputReportBufferSize = 4096 * 4;

  void ReplyWithReports(ReadInputReportsCompleterBase& completer) __TA_REQUIRES(&report_lock_);

  std::mutex report_lock_;
  std::optional<ReadInputReportsCompleter::Async> completer_ __TA_GUARDED(&report_lock_);
  fidl::Arena<kInputReportBufferSize> report_allocator_ __TA_GUARDED(report_lock_);
  std::deque<Report> reports_data_ __TA_GUARDED(report_lock_);

  const size_t reader_id_;
  InputReportReaderManager<Report>* const manager_;
};

// Template Implementation.
template <class Report>
inline std::shared_ptr<typename InputReportReaderManager<Report>::InputReportReader>
InputReportReaderManager<Report>::InputReportReader::Create(
    InputReportReaderManager<Report>* manager, size_t reader_id, async_dispatcher_t* dispatcher,
    fidl::ServerEnd<fuchsia_input_report::InputReportsReader> server) {
  if (!manager || !dispatcher) {
    return nullptr;
  }

  fidl::OnUnboundFn<InputReportReader> unbound_fn(
      [](InputReportReader* reader, fidl::UnbindInfo info,
         fidl::ServerEnd<fuchsia_input_report::InputReportsReader> channel) {
        if (reader && reader->manager_) {
          reader->manager_->RemoveReaderFromList(reader);
        }
      });

  auto reader = std::make_shared<InputReportReader>(manager, reader_id);
  fidl::BindServer(dispatcher, std::move(server), reader, std::move(unbound_fn));
  return reader;
}

template <class Report>
inline void InputReportReaderManager<Report>::InputReportReader::ReceiveReport(
    const Report& report) {
  std::scoped_lock lock(report_lock_);
  reports_data_.push_back(report);

  if (completer_) {
    ReplyWithReports(*completer_);
    completer_.reset();
  }
}

template <class Report>
inline void InputReportReaderManager<Report>::InputReportReader::ReadInputReports(
    ReadInputReportsCompleter::Sync& completer) {
  std::scoped_lock lock(report_lock_);
  if (completer_) {
    completer.ReplyError(ZX_ERR_ALREADY_BOUND);
    return;
  }
  if (reports_data_.empty()) {
    completer_.emplace(completer.ToAsync());
  } else {
    ReplyWithReports(completer);
  }
}

template <class Report>
inline void InputReportReaderManager<Report>::InputReportReader::ReplyWithReports(
    ReadInputReportsCompleterBase& completer) {
  std::array<fuchsia_input_report::wire::InputReport,
             fuchsia_input_report::wire::kMaxDeviceReportCount>
      reports;

  TRACE_DURATION("input", "InputReportInstance GetReports", "instance_id", reader_id_);
  size_t num_reports = 0;
  for (; !reports_data_.empty() && num_reports < reports.size(); num_reports++) {
    // Build the report.
    auto input_report = fuchsia_input_report::wire::InputReport::Builder(report_allocator_);

    // Add some common fields. Will be overwritten if set.
    input_report.trace_id(TRACE_NONCE());
    input_report.event_time(zx_clock_get_monotonic());

    reports_data_.front().ToFidlInputReport(input_report, report_allocator_);

    reports[num_reports] = input_report.Build();

    TRACE_FLOW_BEGIN("input", "input_report", reports[num_reports].trace_id());
    reports_data_.pop_front();
  }

  completer.ReplySuccess(
      fidl::VectorView(fidl::VectorView<fuchsia_input_report::wire::InputReport>::FromExternal(
          reports.data(), num_reports)));

  if (reports_data_.empty()) {
    report_allocator_.Reset();
  }
}

}  // namespace input_report_reader

#endif  // LIB_INPUT_REPORT_READER_READER_H_
