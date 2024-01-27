// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "radarutil.h"

#include <fidl/fuchsia.hardware.radar/cpp/wire.h>
#include <getopt.h>
#include <lib/async/time.h>
#include <lib/fidl/cpp/wire/arena.h>
#include <lib/zx/clock.h>
#include <lib/zx/result.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <fbl/auto_lock.h>

namespace radarutil {

zx::result<zx::duration> ParseDuration(char* arg) {
  char* endptr;
  const int64_t duration = strtol(arg, &endptr, 10);
  if (endptr == arg || *endptr == '\0' || duration < 0) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  switch (*endptr) {
    case 'h':
      return zx::ok(zx::hour(duration));
    case 'm':
      if (strncmp("ms", endptr, 2) == 0) {
        return zx::ok(zx::msec(duration));
      }
      return zx::ok(zx::min(duration));
    case 's':
      return zx::ok(zx::sec(duration));
    case 'u':
      return zx::ok(zx::usec(duration));
    case 'n':
      return zx::ok(zx::nsec(duration));
  }

  return zx::error(ZX_ERR_INVALID_ARGS);
}

void Usage() {
  constexpr char kUsageString[] =
      "Usage: radarutil [options]\n"
      "\n"
      "Options:\n"
      "    --help/-h: Show this message.\n"
      "    --burst-process-time/-p [time]: Time to sleep after each burst to simulate\n"
      "            processing delay. Default: 0s\n"
      "    --time/-t [time]: Total time to read bursts. Mutually exclusive with\n"
      "            --burst-count. Default: 1s\n"
      "    --burst-count/-b [count]: Total number of bursts to ready. Mutually\n"
      "            exclusive with --time.\n"
      "    --vmos/-v [vmos]: Number of VMOs to register for receiving bursts.\n"
      "            Default: 10\n"
      "    --output/-o [path]: Path of the file to write radar bursts to, or \"-\" for\n"
      "            stdout. If omitted, received bursts are not written.\n"
      "    --burst-period-ns [period]: The time between radar bursts reported by this\n"
      "            sensor. Must be greater than zero.\n"
      "    --max-error-rate [rate]: The maximum allowable error rate in errors per\n"
      "            million bursts. radarutil will return nonzero if this rate is\n"
      "            exceeded. Requires either --burst-period-ns or --burst-count.\n"
      "\n"
      "    For time arguments, add a suffix (h,m,s,ms,us,ns) to indicate units.\n"
      "    For example: radarutil -p 3ms -t 5m -v 20\n";

  fprintf(stderr, "%s", kUsageString);
}

zx_status_t RadarUtil::Run(int argc, char** argv,
                           fidl::ClientEnd<fuchsia_hardware_radar::RadarBurstReaderProvider> device,
                           const RadarUtil::FileProvider* file_provider) {
  RadarUtil radarutil;

  zx_status_t status = radarutil.ParseArgs(argc, argv, file_provider);
  if (status != ZX_OK || radarutil.help_) {
    return status;
  }

  if ((status = radarutil.ConnectToDevice(std::move(device))) != ZX_OK) {
    return status;
  }

  if ((status = radarutil.RegisterVmos()) != ZX_OK) {
    return status;
  }

  status = radarutil.Run();

  zx_status_t unregister_status = radarutil.UnregisterVmos();

  if (status != ZX_OK) {
    return status;
  }

  return unregister_status;
}

RadarUtil::~RadarUtil() {
  if (client_) {
    // Block until the FIDL client is torn down to avoid |client_| calling into
    // a destroyed |RadarUtil| object from the radarutil-client-thread.
    client_.AsyncTeardown();
    sync_completion_wait(&client_teardown_completion_, ZX_TIME_INFINITE);
  }

  if (output_file_) {
    fclose(output_file_);
  }
}

fidl::AnyTeardownObserver RadarUtil::teardown_observer() {
  return fidl::ObserveTeardown([this] { sync_completion_signal(&client_teardown_completion_); });
}

zx_status_t RadarUtil::ParseArgs(int argc, char** argv,
                                 const RadarUtil::FileProvider* file_provider) {
  if (argc <= 1) {
    Usage();
    help_ = true;
    return ZX_OK;
  }

  constexpr option kLongOptions[] = {
      {"help", no_argument, nullptr, 'h'},
      {"burst-process-time", required_argument, nullptr, 'p'},
      {"time", required_argument, nullptr, 't'},
      {"burst-count", required_argument, nullptr, 'b'},
      {"vmos", required_argument, nullptr, 'v'},
      {"output", required_argument, nullptr, 'o'},
      {"burst-period-ns", required_argument, nullptr, 'n'},
      {"max-error-rate", required_argument, nullptr, 'm'},
      {nullptr, 0, nullptr, 0},
  };

  int opt, vmos, burst_count;
  while ((opt = getopt_long(argc, argv, "hp:t:b:v:o:", kLongOptions, nullptr)) != -1) {
    switch (opt) {
      case 'h':
        Usage();
        help_ = true;
        return ZX_OK;
      case 'p': {
        zx::result<zx::duration> burst_process_time = ParseDuration(optarg);
        if (burst_process_time.is_error()) {
          Usage();
          return burst_process_time.error_value();
        }
        burst_process_time_ = burst_process_time.value();
        break;
      }
      case 't': {
        if (burst_count_.has_value()) {
          Usage();
          return ZX_ERR_INVALID_ARGS;
        }

        zx::result<zx::duration> run_time = ParseDuration(optarg);
        if (run_time.is_error()) {
          Usage();
          return run_time.error_value();
        }
        run_time_.emplace(run_time.value());
        break;
      }
      case 'b': {
        if (run_time_.has_value()) {
          Usage();
          return ZX_ERR_INVALID_ARGS;
        }

        burst_count = atoi(optarg);
        if (burst_count <= 0) {
          Usage();
          return ZX_ERR_INVALID_ARGS;
        }
        burst_count_.emplace(burst_count);
        break;
      }
      case 'v':
        vmos = atoi(optarg);
        if (vmos <= 0) {
          Usage();
          return ZX_ERR_INVALID_ARGS;
        }
        vmo_count_ = vmos;
        break;
      case 'o':
        if (strcmp(optarg, "-") == 0) {
          output_file_ = stdout;
        } else {
          output_file_ = file_provider->OpenFile(optarg);
          if (!output_file_) {
            fprintf(stderr, "Failed to open %s: %s\n", optarg, strerror(errno));
            return ZX_ERR_IO;
          }
        }
        break;
      case 'n':
        burst_period_ = zx::duration(strtol(optarg, nullptr, 10));
        if (burst_period_.get() <= 0) {
          Usage();
          return ZX_ERR_INVALID_ARGS;
        }
        break;
      case 'm':
        max_error_rate_ = strtoul(optarg, nullptr, 10);
        break;
      default:
        Usage();
        return ZX_ERR_INVALID_ARGS;
    }
  }

  if (!run_time_.has_value() && !burst_count_.has_value()) {
    run_time_.emplace(kDefaultRunTime);
  }

  // --max-error-rate requires -b or --burst-period-ns.
  if (max_error_rate_ && burst_period_ == zx::duration::infinite_past() && !burst_count_) {
    Usage();
    return ZX_ERR_INVALID_ARGS;
  }

  return ZX_OK;
}

zx_status_t RadarUtil::ConnectToDevice(fidl::ClientEnd<BurstReaderProvider> device) {
  zx_status_t status = loop_.StartThread("radarutil-client-thread");
  if (status != ZX_OK) {
    fprintf(stderr, "Failed to start client thread: %s\n", zx_status_get_string(status));
    return status;
  }

  fidl::ClientEnd<BurstReader> client_end;
  fidl::ServerEnd<BurstReader> server_end;
  status = zx::channel::create(0, &client_end.channel(), &server_end.channel());
  if (status != ZX_OK) {
    fprintf(stderr, "Failed to create channel: %s\n", zx_status_get_string(status));
    return status;
  }

  client_.Bind(std::move(client_end), loop_.dispatcher(), this, teardown_observer());

  fidl::WireSyncClient<BurstReaderProvider> provider_client(std::move(device));
  auto result = provider_client->Connect(std::move(server_end));
  if (!result.ok()) {
    fprintf(stderr, "Failed to connect to radar device: %s\n",
            zx_status_get_string(result.status()));
    return result.status();
  }
  if (result->is_error()) {
    fprintf(stderr, "Radar device failed to bind: %u\n",
            static_cast<unsigned int>(result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t RadarUtil::RegisterVmos() {
  const auto burst_properties = client_.sync()->GetBurstProperties();
  if (!burst_properties.ok()) {
    fprintf(stderr, "Failed to get burst size: %s\n",
            zx_status_get_string(burst_properties.status()));
    return burst_properties.status();
  }

  burst_buffer_ = fbl::Array(new uint8_t[burst_properties->size], burst_properties->size);

  fidl::Arena allocator;

  burst_vmos_.resize(vmo_count_);

  fidl::VectorView<zx::vmo> vmo_dups(allocator, vmo_count_);
  fidl::VectorView<uint32_t> vmo_ids(allocator, vmo_count_);

  zx_status_t status;
  for (uint32_t i = 0; i < vmo_count_; i++) {
    if ((status = zx::vmo::create(burst_buffer_.size(), 0, &burst_vmos_[i])) != ZX_OK) {
      fprintf(stderr, "Failed to create VMO: %s\n", zx_status_get_string(status));
      return status;
    }

    if ((status = burst_vmos_[i].duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dups[i])) != ZX_OK) {
      fprintf(stderr, "Failed to duplicate VMO: %s\n", zx_status_get_string(status));
      return status;
    }

    vmo_ids[i] = i;
  }

  const auto result = client_.sync()->RegisterVmos(vmo_ids, vmo_dups);
  if (!result.ok()) {
    fprintf(stderr, "Failed to register VMOs: %s\n", zx_status_get_string(result.status()));
    return result.status();
  }
  if (result->is_error()) {
    fprintf(stderr, "Failed to register VMOs: %d\n", static_cast<int>(result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t RadarUtil::UnregisterVmos() {
  fidl::Arena allocator;

  fidl::VectorView<uint32_t> vmo_ids(allocator, vmo_count_);

  for (uint32_t i = 0; i < vmo_count_; i++) {
    vmo_ids[i] = i;
  }

  const auto result = client_.sync()->UnregisterVmos(vmo_ids);
  if (!result.ok()) {
    fprintf(stderr, "Failed to register VMOs: %s\n", zx_status_get_string(result.status()));
    return result.status();
  }
  if (result->is_error()) {
    fprintf(stderr, "Failed to register VMOs: %d\n", static_cast<int>(result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t RadarUtil::Run() {
  {
    const auto result = client_->StartBursts();
    if (!result.ok()) {
      fprintf(stderr, "Failed to start bursts: %s\n", zx_status_get_string(result.status()));
      return result.status();
    }
  }

  const zx::time start = zx::clock::get_monotonic();

  zx_status_t status = ReadBursts();

  const zx::duration elapsed = zx::clock::get_monotonic() - start;

  const auto result = client_.sync()->StopBursts();
  if (status != ZX_OK) {
    return status;
  }
  if (!result.ok()) {
    fprintf(stderr, "Failed to stop bursts: %s\n", zx_status_get_string(result.status()));
    return result.status();
  }

  if (burst_count_.has_value()) {
    fprintf(stderr, "Received %lu/%lu bursts in %lu seconds\n", bursts_received_, *burst_count_,
            elapsed.to_secs());
  } else {
    fprintf(stderr, "Received %lu bursts and %lu burst errors in %lu seconds\n", bursts_received_,
            burst_errors_, elapsed.to_secs());
  }

  // We have some notion of the expected number of bursts; use that to calculate the error rate.
  if (burst_period_ != zx::duration::infinite_past() || burst_count_) {
    const uint64_t expected_bursts =
        burst_count_ ? *burst_count_ : elapsed.to_nsecs() / burst_period_.to_nsecs();
    const uint64_t diff = (expected_bursts < bursts_received_)
                              ? (bursts_received_ - expected_bursts)
                              : (expected_bursts - bursts_received_);
    const uint64_t error_rate = (diff * 1'000'000) / expected_bursts;

    if (burst_count_) {
      fprintf(stderr, "Error rate %lu\n", error_rate);
    } else {
      fprintf(stderr, "Expected %lu bursts, error rate %lu\n", expected_bursts, error_rate);
    }

    if (max_error_rate_) {
      return error_rate > *max_error_rate_ ? ZX_ERR_IO : ZX_OK;
    }
  }

  return burst_errors_ == 0 ? ZX_OK : ZX_ERR_IO;
}

zx_status_t RadarUtil::ReadBursts() {
  struct Task {
    async_task_t task;
    RadarUtil* object;
  } stop_burst_loop;

  stop_burst_loop.object = this;
  stop_burst_loop.task.state = ASYNC_STATE_INIT;
  stop_burst_loop.task.handler = [](async_dispatcher_t* dispatcher, async_task_t* task,
                                    zx_status_t status) {
    RadarUtil* const object = reinterpret_cast<Task*>(task)->object;
    object->run_ = false;
    object->worker_event_.Broadcast();
  };

  // Post a task to stop the burst reading loop, set to run after the amount of time requested.
  if (run_time_.has_value()) {
    stop_burst_loop.task.deadline = async_now(loop_.dispatcher()) + run_time_->get();
    zx_status_t status = async_post_task(loop_.dispatcher(), &stop_burst_loop.task);
    if (status != ZX_OK) {
      fprintf(stderr, "Failed to post timer task: %s\n", zx_status_get_string(status));
      return status;
    }
  }

  while (run_) {
    fbl::AutoLock lock(&lock_);
    while (burst_vmo_ids_.empty() && run_) {
      worker_event_.Wait(&lock_);
    }

    while (!burst_vmo_ids_.empty()) {
      const uint32_t vmo_id = burst_vmo_ids_.front();
      burst_vmo_ids_.pop();
      if (vmo_id == kInvalidVmoId) {
        burst_errors_++;
      } else if (vmo_id >= burst_vmos_.size()) {
        fprintf(stderr, "Received invalid burst VMO ID %u\n", vmo_id);
        return ZX_ERR_INTERNAL;
      } else {
        bursts_received_++;

        zx_status_t status = burst_vmos_[vmo_id].read(burst_buffer_.get(), 0, burst_buffer_.size());
        if (status != ZX_OK) {
          fprintf(stderr, "Failed to read burst VMO: %s\n", zx_status_get_string(status));
          return status;
        }

        if (burst_process_time_.to_nsecs() > 0) {
          zx::nanosleep(zx::deadline_after(burst_process_time_));
        }

        [[maybe_unused]] auto result = client_->UnlockVmo(vmo_id);

        if (output_file_) {
          fwrite(burst_buffer_.get(), 1, burst_buffer_.size(), output_file_);
        }
      }

      if (burst_count_.has_value() && (burst_errors_ + bursts_received_) >= burst_count_.value()) {
        return ZX_OK;
      }
    }
  }

  return ZX_OK;
}

void RadarUtil::OnBurst(fidl::WireEvent<BurstReader::OnBurst>* event) {
  {
    fbl::AutoLock lock(&lock_);
    if (event->is_burst()) {
      burst_vmo_ids_.push(event->burst().vmo_id);
    } else if (event->is_error()) {
      burst_vmo_ids_.push(kInvalidVmoId);
    }
  }

  worker_event_.Broadcast();
}

// TODO(fxbug.dev/99924): Remove this after all servers have switched to OnBurst.
void RadarUtil::OnBurst2(fidl::WireEvent<BurstReader::OnBurst2>* event) {
  {
    fbl::AutoLock lock(&lock_);
    if (event->is_burst()) {
      burst_vmo_ids_.push(event->burst().vmo_id);
    } else if (event->is_error()) {
      burst_vmo_ids_.push(kInvalidVmoId);
    }
  }

  worker_event_.Broadcast();
}

}  // namespace radarutil
