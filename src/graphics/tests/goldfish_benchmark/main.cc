// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>

#include <memory>

namespace {

// Lines of text for each result are prefixed with this.
constexpr const char* kTestOutputPrefix = "  - ";

// The number of warm up iterations prior to test runs.
constexpr unsigned kWarmUpIterations = 5;

// The number of test runs to do.
constexpr unsigned kNumTestRuns = 10;

// Kilobyte.
constexpr unsigned kKb = 1024;

// Megabyte.
constexpr unsigned kMb = kKb * kKb;

unsigned SizeValue(unsigned size) {
  if (size >= kMb) {
    return size / kMb;
  }
  if (size >= kKb) {
    return size / kKb;
  }
  return size;
}

const char* SizeSuffix(unsigned size) {
  if (size >= kMb) {
    return "MiB";
  }
  if (size >= kKb) {
    return "KiB";
  }
  return "B";
}

// Measures how long it takes to run some number of iterations of a closure.
// Returns a value in microseconds.
template <typename T>
float Measure(unsigned iterations, const T& closure) {
  zx_ticks_t start = zx_ticks_get();
  for (unsigned i = 0; i < iterations; ++i) {
    closure();
  }
  zx_ticks_t stop = zx_ticks_get();
  return (static_cast<float>(stop - start) * 1000000.f / static_cast<float>(zx_ticks_per_second()));
}

// Runs a closure repeatedly and prints its timing.
template <typename T>
void RunAndMeasure(const char* test_name, unsigned iterations, const T& closure) {
  printf("\n* %s ...\n", test_name);

  float warm_up_time = Measure(kWarmUpIterations, closure);

  printf("%swarm-up: %u iterations in %.3f us, %.3f us per iteration\n", kTestOutputPrefix,
         kWarmUpIterations, warm_up_time, warm_up_time / kWarmUpIterations);

  float run_times[kNumTestRuns];
  for (unsigned i = 0; i < kNumTestRuns; ++i) {
    run_times[i] = Measure(iterations, closure);
    zx::nanosleep(zx::deadline_after(zx::msec(10)));
  }

  float min = 0, max = 0;
  float cumulative = 0;
  for (const auto rt : run_times) {
    if (min == 0 || min > rt) {
      min = rt;
    }
    if (max == 0 || max < rt) {
      max = rt;
    }
    cumulative += rt;
  }
  float average = cumulative / kNumTestRuns;

  printf("%srun: %u test runs, %u iterations per run\n", kTestOutputPrefix, kNumTestRuns,
         iterations);
  printf("%stotal (usec): min: %.3f, max: %.3f, ave: %.3f\n", kTestOutputPrefix, min, max, average);
  printf("%sper-iteration (usec): min: %.3f\n",
         // The static cast is to avoid a "may change value" warning.
         kTestOutputPrefix, min / static_cast<float>(iterations));
}

void RunPingPongBenchmark(fidl::WireSyncClient<fuchsia_hardware_goldfish::Pipe>& pipe,
                          unsigned size, unsigned iterations, bool skip_if_out_of_memory) {
  {
    auto result = pipe->SetBufferSize(size);
    ZX_ASSERT(result.ok());

    if (skip_if_out_of_memory && result.value().res == ZX_ERR_NO_MEMORY) {
      fprintf(stderr,
              "Failed to allocate memory (ZX_ERR_NO_MEMORY). "
              "buffer size: %u (bytes). Test skipped.\n",
              size);
      return;
    }

    ZX_ASSERT(result.value().res == ZX_OK);
  }

  zx::vmo vmo;
  {
    auto result = pipe->GetBuffer();
    ZX_ASSERT(result.ok() && result.value().res == ZX_OK);
    vmo = std::move(result.value().vmo);
  }

  {
    auto buffer = std::make_unique<uint8_t[]>(size);
    uint8_t* data = buffer.get();
    memset(data, 0xff, size);
    vmo.write(data, 0, size);
  }

  char test_name[64];
  snprintf(test_name, sizeof(test_name), "pingpong, %u%s", SizeValue(size), SizeSuffix(size));

  RunAndMeasure(test_name, iterations, [&pipe, size] {
    auto result = pipe->DoCall(size, 0, size, 0);
    // For the test purpose we expect the buffer is small enough
    // so that we can finish in one write-read round trip.
    ZX_ASSERT(result.ok() && result.value().res == ZX_OK);
    ZX_ASSERT(result.value().actual == 2 * size);
  });
}

}  // namespace

int main(int argc, char** argv) {
  // TODO(https://fxbug.dev/113830): Stop hardcoding the 000 in this path.
  zx::result controller =
      component::Connect<fuchsia_hardware_goldfish::Controller>("/dev/class/goldfish-pipe/000");
  ZX_ASSERT_MSG(controller.is_ok(), "%s", controller.status_string());

  zx::result pipe_device_endpoints = fidl::CreateEndpoints<fuchsia_hardware_goldfish::PipeDevice>();
  ZX_ASSERT_MSG(pipe_device_endpoints.is_ok(), "%s", pipe_device_endpoints.status_string());
  auto& [pipe_device_client, pipe_device_server] = pipe_device_endpoints.value();
  {
    fidl::Status status =
        fidl::WireCall(controller.value())->OpenSession(std::move(pipe_device_server));
    ZX_ASSERT_MSG(status.ok(), "%s", status.status_string());
  }

  fidl::WireSyncClient pipe_device(std::move(pipe_device_client));

  zx::result pipe_endpoints = fidl::CreateEndpoints<fuchsia_hardware_goldfish::Pipe>();
  ZX_ASSERT_MSG(pipe_endpoints.is_ok(), "%s", pipe_endpoints.status_string());
  auto& [pipe_client, pipe_server] = pipe_endpoints.value();
  {
    fidl::Status status = pipe_device->OpenPipe(std::move(pipe_server));
    ZX_ASSERT_MSG(status.ok(), "%s", status.status_string());
  }

  fidl::WireSyncClient pipe(std::move(pipe_client));

  zx::vmo vmo;

  {
    auto result = pipe->GetBuffer();
    ZX_ASSERT(result.ok() && result.value().res == ZX_OK);
    vmo = std::move(result.value().vmo);
  }

  // Connect to pingpong service.
  constexpr char kPipeName[] = "pipe:pingpong";
  size_t bytes = strlen(kPipeName) + 1;
  ZX_ASSERT(vmo.write(kPipeName, 0, bytes) == ZX_OK);

  {
    auto result = pipe->Write(bytes, 0);
    ZX_ASSERT(result.ok() && result.value().res == ZX_OK);
    ZX_ASSERT(result.value().actual == bytes);
  }

  if (argc > 1) {
    for (int i = 1; (i + 1) < argc; i += 2) {
      unsigned size = atoi(argv[i]);
      unsigned iterations = atoi(argv[i + 1]);

      RunPingPongBenchmark(pipe, size, iterations, /* skip_if_out_of_memory */ false);
    }
  } else {
    RunPingPongBenchmark(pipe, ZX_PAGE_SIZE, 500, /* skip_if_out_of_memory */ false);

    // In some cases the system might not be able to allocate a contiguous
    // memory space of 1MB due to out of memory. In that case we should just
    // skip the test.
    RunPingPongBenchmark(pipe, kMb, 5, /* skip_if_out_of_memory */ true);
  }

  printf("\nGoldfish benchmarks completed.\n");

  return 0;
}
